/*
 * QEMU Apple ParavirtualizedGraphics.framework device, MMIO (arm64) variant
 *
 * Copyright © 2023 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ParavirtualizedGraphics.framework is a set of libraries that macOS provides
 * which implements 3d graphics passthrough to the host as well as a
 * proprietary guest communication channel to drive it. This device model
 * implements support to drive that library from within QEMU as an MMIO-based
 * system device for macOS on arm64 VMs.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "block/aio-wait.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "system/address-spaces.h"
#include "apple-gfx.h"
#include "trace.h"

#import <ParavirtualizedGraphics/ParavirtualizedGraphics.h>

OBJECT_DECLARE_SIMPLE_TYPE(AppleGFXMMIOState, APPLE_GFX_MMIO)

/*
 * ParavirtualizedGraphics.Framework only ships header files for the PCI
 * variant which does not include IOSFC descriptors and host devices. We add
 * their definitions here so that we can also work with the ARM version.
 */
typedef bool(^IOSFCRaiseInterrupt)(uint32_t vector);
typedef bool(^IOSFCUnmapMemory)(void *, void *, void *, void *, void *, void *);
typedef bool(^IOSFCMapMemory)(uint64_t phys, uint64_t len, bool ro, void **va,
                              void *, void *);

@interface PGDeviceDescriptor (IOSurfaceMapper)
@property (readwrite, nonatomic) bool usingIOSurfaceMapper;
@end

@interface PGIOSurfaceHostDeviceDescriptor : NSObject
-(PGIOSurfaceHostDeviceDescriptor *)init;
@property (readwrite, nonatomic, copy, nullable) IOSFCMapMemory mapMemory;
@property (readwrite, nonatomic, copy, nullable) IOSFCUnmapMemory unmapMemory;
@property (readwrite, nonatomic, copy, nullable) IOSFCRaiseInterrupt raiseInterrupt;
@end

/*
 * These are 64-bit, not 32-bit. The runtime's own type encodings are
 * `-mmioWriteAtOffset:value:` = "v32@0:8Q16Q24" and `-mmioReadAtOffset:` =
 * "Q24@0:8Q16"; declaring the value as uint32_t makes the compiler pass it in
 * w2, so the framework sees the guest's write with its top 32 bits gone. t8030
 * puts DRAM at 0x800000000, so the IOSurface ring base 0x821b10000 arrived as
 * 0x21b10000 and the framework logged
 *   [iosurface] Failed to create ring mapping: ringBase=0x21b10000 length=0x4000
 * on every single boot. A macOS guest never noticed because nothing it puts
 * through this register lives above 4 GiB.
 */
@interface PGIOSurfaceHostDevice : NSObject
-(instancetype)initWithDescriptor:(PGIOSurfaceHostDeviceDescriptor *)desc;
-(uint64_t)mmioReadAtOffset:(size_t)offset;
-(void)mmioWriteAtOffset:(size_t)offset value:(uint64_t)value;
@end

/*
 * Inferno's AIC is level driven: apple_aic_set_irq() only sets or clears the
 * pending bit, and delivery happens in apple_aic_update(), which runs off a
 * 64us tick. Acknowledging a vector *masks* it rather than clearing the
 * pending bit, so a device is expected to hold its line until the guest has
 * serviced it. qemu_irq_pulse() raises and lowers inside one bottom half, so
 * the pending bit is set and cleared within nanoseconds and the tick
 * essentially never sees it -- every interrupt this device raised was dropped.
 * The GPU survived that because its driver waits on completion stamps in
 * shared memory, but AppleParavirtIOSurface really does wait for the
 * interrupt, and logged "IOSurfaceParavirtMapperDevice host command timeout"
 * every 60 seconds instead of ever mapping a surface for scanout.
 *
 * Hold the line instead, and drop it once the AIC has had time to deliver.
 */
typedef struct AppleGFXIRQLine {
    qemu_irq irq;
    QEMUTimer *deassert;
} AppleGFXIRQLine;

/*
 * 0.5 ms looked like plenty against a 64us tick, but the guest still logs
 * "IOSurfaceParavirtMapperDevice host command timeout" -- and when one of
 * those lands while the scanout is being set up, the display never starts and
 * the boot looks wedged. If the vCPU has not taken the interrupt before the
 * line drops, the AIC's pending bit goes with it and the driver waits out its
 * full 60 s timeout, so the hold has to outlast a busy guest rather than just
 * the tick. INFERNO_PVG_IRQ_HOLD_US overrides it for sweeping.
 */
#define APPLE_GFX_IRQ_HOLD_NS_DEFAULT (200 * 1000 * 1000) /* 200 ms fallback */

static int64_t apple_gfx_irq_hold_ns(void)
{
    static int64_t cached;

    if (cached == 0) {
        const char *env = getenv("INFERNO_PVG_IRQ_HOLD_US");
        cached = env != NULL ? (int64_t)strtoll(env, NULL, 0) * 1000 :
                               APPLE_GFX_IRQ_HOLD_NS_DEFAULT;
        if (cached <= 0) {
            cached = APPLE_GFX_IRQ_HOLD_NS_DEFAULT;
        }
    }
    return cached;
}

struct AppleGFXMapSurfaceMemoryJob;
struct AppleGFXMMIOState {
    SysBusDevice parent_obj;

    AppleGFXState common;

    AppleGFXIRQLine irq_gfx;
    AppleGFXIRQLine irq_iosfc;
    MemoryRegion iomem_iosfc;
    PGIOSurfaceHostDevice *pgiosfc;
};

typedef struct AppleGFXMMIOJob {
    AppleGFXMMIOState *state;
    uint64_t offset;
    uint64_t value;
    bool completed;
} AppleGFXMMIOJob;

static void apple_gfx_irq_deassert(void *opaque);

static void iosfc_do_read(void *opaque)
{
    AppleGFXMMIOJob *job = opaque;
    job->value = [job->state->pgiosfc mmioReadAtOffset:job->offset];
    qatomic_set(&job->completed, true);
    aio_wait_kick();
}

static uint64_t iosfc_read(void *opaque, hwaddr offset, unsigned size)
{
    AppleGFXMMIOJob job = {
        .state = opaque,
        .offset = offset,
        .completed = false,
    };
    dispatch_queue_t queue =
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

    dispatch_async_f(queue, &job, iosfc_do_read);
    AIO_WAIT_WHILE(NULL, !qatomic_read(&job.completed));

    /*
     * The guest only reads this region when its driver is servicing the
     * device, so treat the read as the acknowledgement and drop the line here.
     * The timer set in raise_irq_bh stays as a fallback for the case where the
     * guest never comes at all.
     */
    apple_gfx_irq_deassert(&((AppleGFXMMIOState *)opaque)->irq_iosfc);

    trace_apple_gfx_mmio_iosfc_read(offset, job.value);
    return job.value;
}

static void iosfc_do_write(void *opaque)
{
    AppleGFXMMIOJob *job = opaque;
    [job->state->pgiosfc mmioWriteAtOffset:job->offset value:job->value];
    qatomic_set(&job->completed, true);
    aio_wait_kick();
}

static void iosfc_write(void *opaque, hwaddr offset, uint64_t val,
                        unsigned size)
{
    AppleGFXMMIOJob job = {
        .state = opaque,
        .offset = offset,
        .value = val,
        .completed = false,
    };
    dispatch_queue_t queue =
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

    dispatch_async_f(queue, &job, iosfc_do_write);
    AIO_WAIT_WHILE(NULL, !qatomic_read(&job.completed));

    trace_apple_gfx_mmio_iosfc_write(offset, val);
}

static const MemoryRegionOps apple_iosfc_ops = {
    .read = iosfc_read,
    .write = iosfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void apple_gfx_irq_deassert(void *opaque)
{
    AppleGFXIRQLine *line = opaque;

    qemu_irq_lower(line->irq);
}

static void raise_irq_bh(void *opaque)
{
    AppleGFXIRQLine *line = opaque;

    qemu_irq_raise(line->irq);
    timer_mod_ns(line->deassert,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + apple_gfx_irq_hold_ns());
}

static void *apple_gfx_mmio_map_surface_memory(uint64_t guest_physical_address,
                                               uint64_t length, bool read_only)
{
    void *mem;
    MemoryRegion *region = NULL;

    RCU_READ_LOCK_GUARD();
    mem = apple_gfx_host_ptr_for_gpa_range(guest_physical_address,
                                           length, read_only, &region);
    if (mem) {
        memory_region_ref(region);
    }
    return mem;
}

static bool apple_gfx_mmio_unmap_surface_memory(void *ptr)
{
    MemoryRegion *region;
    ram_addr_t offset = 0;

    RCU_READ_LOCK_GUARD();
    region = memory_region_from_host(ptr, &offset);
    if (!region) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: memory at %p to be unmapped not found.\n",
                      __func__, ptr);
        return false;
    }

    trace_apple_gfx_iosfc_unmap_memory_region(ptr, region);
    memory_region_unref(region);
    return true;
}


@interface PGIOSurfaceHostDeviceDescriptor (AppleGFXModernMemoryMap)
@property (readwrite, nonatomic, strong, nullable)
    PGMemoryMapDescriptor *memoryMapDescriptor;
@end

/*
 * macOS 26's ParavirtualizedGraphics dropped PGIOSurfaceHostDeviceDescriptor's
 * mapMemory/unmapMemory blocks. Instead the VMM declares the guest's physical
 * memory up front through a PGMemoryMapDescriptor and the framework resolves
 * addresses itself. Setting the old properties on the new framework throws
 * "-[PGIOSurfaceHostDeviceDescriptor setMapMemory:]: unrecognized selector",
 * which aborts QEMU during realize. The map itself is built in apple-gfx.m,
 * because the GPU device needs the same one.
 */
static void apple_gfx_describe_guest_memory(
    PGIOSurfaceHostDeviceDescriptor *iosfc_desc)
{
    PGMemoryMapDescriptor *map_desc = apple_gfx_new_guest_memory_map();

    iosfc_desc.memoryMapDescriptor = map_desc;
    [map_desc release];
}

static PGIOSurfaceHostDevice *apple_gfx_prepare_iosurface_host_device(
    AppleGFXMMIOState *s)
{
    PGIOSurfaceHostDeviceDescriptor *iosfc_desc =
        [PGIOSurfaceHostDeviceDescriptor new];
    PGIOSurfaceHostDevice *iosfc_host_dev;

    if ([iosfc_desc respondsToSelector:@selector(setMapMemory:)]) {
        iosfc_desc.mapMemory =
            ^bool(uint64_t phys, uint64_t len, bool ro, void **va, void *e,
                  void *f) {
                *va = apple_gfx_mmio_map_surface_memory(phys, len, ro);

                trace_apple_gfx_iosfc_map_memory(phys, len, ro, va, e, f, *va);

                return *va != NULL;
            };

        iosfc_desc.unmapMemory =
            ^bool(void *va, void *b, void *c, void *d, void *e, void *f) {
                return apple_gfx_mmio_unmap_surface_memory(va);
            };
    } else {
        apple_gfx_describe_guest_memory(iosfc_desc);
    }

    iosfc_desc.raiseInterrupt = ^bool(uint32_t vector) {
        trace_apple_gfx_iosfc_raise_irq(vector);
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                raise_irq_bh, &s->irq_iosfc);
        return true;
    };

    iosfc_host_dev =
        [[PGIOSurfaceHostDevice alloc] initWithDescriptor:iosfc_desc];
    [iosfc_desc release];
    return iosfc_host_dev;
}

/*
 * Undeclared in the headers but present (see data/hosttrap/pgtypes):
 *   PGDeviceDescriptor -setExternalDisplayPortMask:   v20@0:8I16
 *   PGDeviceDescriptor -setDisplayPortConnectionTypes: v20@0:8I16
 * Left at their defaults iOS treats the paravirtual display as an external
 * monitor, and keeps the built-in touchscreen on a different display than the
 * one SpringBoard's scenes render on, so no touch ever reaches a view.
 */
@interface PGDeviceDescriptor (InfernoDisplayPorts)
- (void)setExternalDisplayPortMask:(unsigned int)mask;
- (void)setDisplayPortConnectionTypes:(unsigned int)types;
@end

static void apple_gfx_mmio_realize(DeviceState *dev, Error **errp)
{
    @autoreleasepool {
        AppleGFXMMIOState *s = APPLE_GFX_MMIO(dev);
        PGDeviceDescriptor *desc = [PGDeviceDescriptor new];

        desc.raiseInterrupt = ^(uint32_t vector) {
            trace_apple_gfx_raise_irq(vector);
            aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                    raise_irq_bh, &s->irq_gfx);
        };

        desc.usingIOSurfaceMapper = true;

        /* No external display ports: this panel is the built-in one. */
        if ([desc respondsToSelector:@selector(setExternalDisplayPortMask:)]) {
            const char *m = getenv("INFERNO_PVG_EXT_PORT_MASK");
            [desc setExternalDisplayPortMask:m ? (unsigned)strtoul(m, NULL, 0) : 0u];
        }
        if (getenv("INFERNO_PVG_DP_TYPES") != NULL &&
            [desc respondsToSelector:@selector(setDisplayPortConnectionTypes:)]) {
            [desc setDisplayPortConnectionTypes:
                      (unsigned)strtoul(getenv("INFERNO_PVG_DP_TYPES"), NULL, 0)];
        }
        s->pgiosfc = apple_gfx_prepare_iosurface_host_device(s);

        if (!apple_gfx_common_realize(&s->common, dev, desc, errp)) {
            [s->pgiosfc release];
            s->pgiosfc = nil;
        }

        [desc release];
        desc = nil;
    }
}

static void apple_gfx_mmio_init(Object *obj)
{
    AppleGFXMMIOState *s = APPLE_GFX_MMIO(obj);

    apple_gfx_common_init(obj, &s->common, TYPE_APPLE_GFX_MMIO);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->common.iomem_gfx);
    memory_region_init_io(&s->iomem_iosfc, obj, &apple_iosfc_ops, s,
                          TYPE_APPLE_GFX_MMIO, 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_iosfc);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq_gfx.irq);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq_iosfc.irq);
    s->irq_gfx.deassert = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       apple_gfx_irq_deassert, &s->irq_gfx);
    s->irq_iosfc.deassert = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         apple_gfx_irq_deassert, &s->irq_iosfc);
}

static void apple_gfx_mmio_reset(Object *obj, ResetType type)
{
    AppleGFXMMIOState *s = APPLE_GFX_MMIO(obj);
    [s->common.pgdev reset];
}

static const Property apple_gfx_mmio_properties[] = {
    DEFINE_PROP_ARRAY("display-modes", AppleGFXMMIOState,
                      common.num_display_modes, common.display_modes,
                      qdev_prop_apple_gfx_display_mode, AppleGFXDisplayMode),
};

static void apple_gfx_mmio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = apple_gfx_mmio_reset;
    dc->hotpluggable = false;
    dc->realize = apple_gfx_mmio_realize;

    device_class_set_props(dc, apple_gfx_mmio_properties);
}

static const TypeInfo apple_gfx_mmio_types[] = {
    {
        .name          = TYPE_APPLE_GFX_MMIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(AppleGFXMMIOState),
        .class_init    = apple_gfx_mmio_class_init,
        .instance_init = apple_gfx_mmio_init,
    }
};
DEFINE_TYPES(apple_gfx_mmio_types)
