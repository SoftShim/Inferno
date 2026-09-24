/*
 * Apple Device Address Resolution Table.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dart.h"
#include "hw/arm/apple-silicon/dt.h"
#include "hw/irq.h"
#include "hw/registerfields.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "monitor/hmp-target.h"
#include "monitor/monitor.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "system/dma.h"
#include "system/hvf.h"
#include "qobject/qdict.h"

#if 0
#define DPRINTF(fmt, ...)                             \
    do {                                              \
        fprintf(stderr, "dart: " fmt, ##__VA_ARGS__); \
    } while (0)
#else
#define DPRINTF(fmt, ...) \
    do {                  \
    } while (0)
#endif

#define DART_MAX_STREAMS (16)
#define DART_MAX_TTBR (4)
#define DART_MAX_VA_BITS (38)
#define DART_MAX_TLB_OP_SETS (1)

enum {
    DART_TLB_OP_INVALIDATE = 0,
};

// clang-format off
REG32(DAPF_CONFIG, 0x0)
REG32(DAPF_SID, 0x4)
REG32(DAPF_START_LSB, 0x8)
REG32(DAPF_START_MSB, 0xC)
REG32(DAPF_END_LSB, 0x10)
REG32(DAPF_END_MSB, 0x14)
// DAPF REG STRIDE 0x40

REG32(DART_PARAMS1, 0x0)
    REG_FIELD(DART_PARAMS1, PAGE_SHIFT, 24, 4)
    REG_FIELD(DART_PARAMS1, ACCESS_REGION_PROTECTION, 31, 1)
REG32(DART_PARAMS2, 0x4)
    REG_FIELD(DART_PARAMS2, BYPASS_SUPPORT, 0, 1)
    REG_FIELD(DART_PARAMS2, LOCK_SUPPORT, 1, 1)
// 0x8..0x1C = ??
REG32(DART_TLB_OP, 0x20)
    REG_FIELD(DART_TLB_OP, OP, 0, 2)
    REG_FIELD(DART_TLB_OP, BUSY, 2, 1)
    REG_FIELD(DART_TLB_OP, HARDWARE_FLUSH, 3, 1)
    REG_FIELD(DART_TLB_OP, WAY_INDEX, 4, 4)
    REG_FIELD(DART_TLB_OP, TE_INDEX, 8, 3)
    REG_FIELD(DART_TLB_OP, SET_INDEX, 20, 12)
// 0x24..0x30 = ??
REG32(DART_TLB_OP_SET_0_LOW, 0x34)
REG32(DART_TLB_OP_SET_0_HIGH, 0x38)
// 0x3C = ??
REG32(DART_ERROR_STATUS, 0x40)
    REG_FIELD(DART_ERROR_STATUS, TTBR_INVLD, 0, 1)
    REG_FIELD(DART_ERROR_STATUS, L2E_INVLD, 1, 1)
    REG_FIELD(DART_ERROR_STATUS, PTE_INVLD, 2, 1)
    REG_FIELD(DART_ERROR_STATUS, WRITE_PROT, 3, 1)
    REG_FIELD(DART_ERROR_STATUS, READ_PROT, 4, 1)
    REG_FIELD(DART_ERROR_STATUS, AXI_SLV_DECODE, 5, 1)
    REG_FIELD(DART_ERROR_STATUS, AXI_SLV_ERR, 6, 1)
    REG_FIELD(DART_ERROR_STATUS, REGION_PROT, 7, 1)
    REG_FIELD(DART_ERROR_STATUS, CTRR_WRITE_PROT, 8, 1)
    REG_FIELD(DART_ERROR_STATUS, UNKNOWN, 9, 1)
    REG_FIELD(DART_ERROR_STATUS, APF_REJECT, 11, 1)
    REG_FIELD(DART_ERROR_STATUS, SID, 24, 4)
    REG_FIELD(DART_ERROR_STATUS, FLAG, 31, 1)
// 0x44..0x4C = ??
REG32(DART_ERROR_ADDRESS_LOW, 0x50)
REG32(DART_ERROR_ADDRESS_HIGH, 0x54)
// 0x58..0x5C = ??
REG32(DART_CONFIG, 0x60)
    REG_FIELD(DART_CONFIG, LOCK, 15, 1)
#define A_DART_SID_REMAP(sid) (0x80 + ((sid) << 2))
#define R_DART_SID_REMAP(sid) (A_DART_SID_REMAP(sid) >> 2)
REG32(DART_SID_VALID, 0xFC)
#define A_DART_SID_CONFIG(sid) (0x100 + ((sid) << 2))
#define R_DART_SID_CONFIG(sid) (A_DART_SID_CONFIG(sid) >> 2)
    REG_FIELD(DART_SID_CONFIG, DISABLE_TTBR_INVALID_ERR, 0, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_STE_INVALID_ERR, 1, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_PTE_INVALID_ERR, 2, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_WRITE_PROTECT_EXCEPTION, 3, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_READ_PROTECT_EXCEPTION, 4, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_AXI_RRESP_EXCEPTION, 6, 1)
    REG_FIELD(DART_SID_CONFIG, TRANSLATION_ENABLE, 7, 1)
    REG_FIELD(DART_SID_CONFIG, FULL_BYPASS, 8, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_DROP_PROTECT_EXCEPTION, 9, 1)
    REG_FIELD(DART_SID_CONFIG, DISABLE_APF_REJECT_EXCEPTION, 10, 1)
    REG_FIELD(DART_SID_CONFIG, APF_BYPASS, 12, 1)
    REG_FIELD(DART_SID_CONFIG, BYPASS_ADDR_39_32, 16, 4)
#define A_DART_TLB_CONFIG(sid) (0x180 + ((sid) << 2))
#define R_DART_TLB_CONFIG(sid) (A_DART_TLB_CONFIG(sid) >> 2)
/*
 * TTBRs are laid out as DART_MAX_TTBR consecutive registers per stream:
 *   0x200 + 4 * (DART_MAX_TTBR * sid + idx)
 * matching Linux's apple-dart (ttbr + 4 * (ttbr_count * sid + idx)) and
 * confirmed against the guest: XNU's write to 0x2F0 is TTBR[15][0], which is
 * flat index 60 = 15 * 4.
 *
 * The multipliers used to be transposed (DART_MAX_STREAMS * sid + DART_MAX_TTBR
 * * idx). Only (0, 0) came out right, and the case ranges built from it spanned
 * 0x200..0x63C -- 272 flat indices into a 64-entry array, so every decoded
 * register above 0x2FC wrote past the end of regs.ttbr.
 */
#define A_DART_TTBR(sid, idx) \
    (0x200 + (((DART_MAX_TTBR * (sid)) + (idx)) << 2))
#define R_DART_TTBR(sid, idx) (A_DART_TTBR(sid, idx) >> 2)
REG_FIELD(DART_TTBR, VALID, 31, 1)
#define DART_TTBR_SHIFT (12)
#define DART_TTBR_MASK (0xFFFFFFF)
REG_FIELD(DART_PTE, NO_WRITE, 7, 1)
REG_FIELD(DART_PTE, NO_READ, 8, 1)
#define DART_PTE_AP_MASK (3 << 7)
#define DART_PTE_VALID (1 << 0) // wut?
#define DART_PTE_TYPE_TABLE (1 << 0)
#define DART_PTE_TYPE_BLOCK (3 << 0)
#define DART_PTE_TYPE_MASK (0x3)
#define DART_PTE_ADDR_MASK (0xFFFFFFFFFFull)
// 0x1000 = ??, default val 0x3B6D
REG32(DART_PERF_SID_ENABLE_LOW, 0x1004)
REG32(DART_PERF_SID_ENABLE_HIGH, 0x1008)
REG32(DART_PERF_STATUS, 0x100C)
REG32(DART_TLB_MISS_CTR, 0x1020)
REG32(DART_TLB_HIT_CTR, 0x1028)
REG32(DART_ST_MISS_CTR, 0x102C)
REG32(DART_ST_HIT_CTR, 0x1034)
// clang-format on

typedef enum {
    DART_UNKNOWN = 0,
    DART_DART,
    DART_SMMU,
    DART_DAPF,
} dart_instance_t;

static const char *dart_instance_name[] = {
    [DART_UNKNOWN] = "Unknown",
    [DART_DART] = "DART",
    [DART_SMMU] = "SMMU",
    [DART_DAPF] = "DAPF",
};

typedef struct AppleDARTMapperInstance AppleDARTMapperInstance;

typedef struct AppleDARTIOMMUMemoryRegion {
    IOMMUMemoryRegion iommu;
    AppleDARTMapperInstance *mapper;
    uint32_t sid;
} AppleDARTIOMMUMemoryRegion;

typedef struct {
    uint32_t params1;
    uint32_t params2;
    uint32_t tlb_op;
    uint64_t tlb_op_set[DART_MAX_TLB_OP_SETS];
    uint32_t error_status;
    uint64_t error_address;
    uint32_t config;
    uint8_t sid_remap[DART_MAX_STREAMS];
    uint32_t sid_config[DART_MAX_STREAMS];
    uint32_t ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];
} AppleDARTDARTRegs;

typedef struct {
    MemoryRegion iomem;
    QemuMutex mutex;
    AppleDARTState *dart;
    uint32_t id;
    dart_instance_t type;
} AppleDARTInstance;

struct AppleDARTMapperInstance {
    AppleDARTInstance common;
    AppleDARTIOMMUMemoryRegion *iommus[DART_MAX_STREAMS];
    AppleDARTDARTRegs regs;
};

struct AppleDARTState {
    SysBusDevice parent_obj;
    qemu_irq irq;
    AppleDARTInstance **instances;
    uint32_t num_instances;
    uint32_t page_size;
    uint32_t page_shift;
    uint64_t page_mask;
    uint64_t page_bits;
    uint32_t l_mask[3];
    uint32_t l_shift[3];
    uint64_t sid_mask;
    uint32_t dart_options;
    /*
     * HVF mirror (see apple_dart_hvf_mirror_rebuild). -1 disables it.
     */
    int32_t hvf_mirror_sid;
    uint64_t hvf_mirror_size;
    GPtrArray *hvf_mirror_regions;
    QEMUTimer *hvf_mirror_timer;
    uint64_t *hvf_mirror_last;
    uint32_t hvf_mirror_pages;
    AppleDARTMapperInstance *hvf_mirror_mapper;
};

static int apple_dart_device_list(Object *obj, void *opaque)
{
    GSList **list = opaque;

    if (object_dynamic_cast(obj, TYPE_APPLE_DART)) {
        *list = g_slist_append(*list, obj);
    }

    object_child_foreach(obj, apple_dart_device_list, opaque);
    return 0;
}

static GSList *apple_dart_get_device_list(void)
{
    GSList *list = NULL;

    object_child_foreach(qdev_get_machine(), apple_dart_device_list, &list);

    return list;
}

static void apple_dart_raise_irq(AppleDARTState *dart)
{
    qemu_irq_raise(dart->irq);
}

static void apple_dart_update_irq(AppleDARTState *dart)
{
    AppleDARTInstance *instance;
    AppleDARTMapperInstance *mapper;

    for (uint32_t i = 0; i < dart->num_instances; i++) {
        instance = dart->instances[i];
        if (instance->type != DART_DART) {
            continue;
        }

        mapper = container_of(instance, AppleDARTMapperInstance, common);

        if (mapper->regs.error_status == 0) {
            continue;
        }

        apple_dart_raise_irq(dart);
        return;
    }

    qemu_irq_lower(dart->irq);
}

static const char *apple_dart_sep_id(AppleDARTMapperInstance *mapper)
{
    const char *id = mapper->common.dart->parent_obj.parent_obj.id;

    return (id != NULL && strstr(id, "sep") != NULL) ? id : NULL;
}

static void apple_dart_hvf_mirror_rebuild(AppleDARTMapperInstance *mapper);

static void apple_dart_mapper_reg_write(void *opaque, hwaddr addr,
                                        uint64_t data, unsigned size)
{
    AppleDARTMapperInstance *mapper = opaque;
    uint32_t val = data;
    uint32_t i;
    uint32_t set_index;
    uint64_t sid_mask = 0;
    IOMMUTLBEvent event = { 0 };

    DPRINTF("%s[%d]: (DART) 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx "\n",
            mapper->common.dart->parent_obj.parent_obj.id, mapper->common.id,
            addr, data);

    /*
     * The switch below ends in `default: break;`, so any register this model
     * does not decode is silently dropped. Log every SEP DART write so an
     * unhandled one is visible. (The AP *does* install a valid TTBR for SEP --
     * late, after SID_CONFIG -- followed by a TLB invalidate with sid mask 1.
     * An earlier note here claimed it never did; that was a trace read too
     * early in the boot.)
     */
    if (getenv("INFERNO_DART_SEP_TRACE") != NULL &&
        apple_dart_sep_id(mapper) != NULL) {
        fprintf(stderr, "DART_SEP: reg write [%d] 0x%" HWADDR_PRIx
                " <- 0x%" PRIx64 " (size %u)\n",
                mapper->common.id, addr, data, size);
    }

    switch (addr >> 2) {
    case R_DART_TLB_OP:
        if (REG_FIELD_EX32(val, DART_TLB_OP, OP) != DART_TLB_OP_INVALIDATE ||
            REG_FIELD_EX32(val, DART_TLB_OP, SET_INDEX) == 0 ||
            REG_FIELD_EX32(qatomic_read(&mapper->regs.tlb_op), DART_TLB_OP,
                           BUSY)) {
            break;
        }

        qatomic_set(&mapper->regs.tlb_op,
                    REG_FIELD_DP32(val, DART_TLB_OP, BUSY, 1));

        set_index = REG_FIELD_EX32(val, DART_TLB_OP, SET_INDEX);

        WITH_QEMU_LOCK_GUARD(&mapper->common.mutex)
        {
            for (i = 0; i < DART_MAX_TLB_OP_SETS; ++i) {
                if ((set_index & BIT_ULL(i)) == 0) {
                    continue;
                }

                sid_mask |=
                    mapper->regs.tlb_op_set[i] & mapper->common.dart->sid_mask;
            }
        }

        if (sid_mask != 0) {
            for (i = 0; i < DART_MAX_STREAMS; ++i) {
                if ((sid_mask & BIT_ULL(i)) == 0) {
                    continue;
                }

                event.type = IOMMU_NOTIFIER_UNMAP;
                event.entry.target_as = &address_space_memory;
                event.entry.iova = 0;
                event.entry.perm = IOMMU_NONE;
                event.entry.addr_mask = HWADDR_MAX;

                memory_region_notify_iommu(&mapper->iommus[i]->iommu, 0, event);
            }
        }

        /*
         * An invalidate is the point at which the AP publishes new mappings, so
         * it is also where the HVF mirror has to be refreshed. XNU invalidates
         * the SEP stream (sid mask 1) right after installing its TTBR.
         */
        apple_dart_hvf_mirror_rebuild(mapper);

        qatomic_and(&mapper->regs.tlb_op, ~R_DART_TLB_OP_BUSY_MASK);
        break;
    case R_DART_TLB_OP_SET_0_LOW:
        if (!REG_FIELD_EX32(qatomic_read(&mapper->regs.tlb_op), DART_TLB_OP,
                            BUSY)) {
            mapper->regs.tlb_op_set[0] =
                deposit64(mapper->regs.tlb_op_set[0], 0, 32, val);
        }
        break;
    case R_DART_TLB_OP_SET_0_HIGH:
        if (!REG_FIELD_EX32(qatomic_read(&mapper->regs.tlb_op), DART_TLB_OP,
                            BUSY)) {
            mapper->regs.tlb_op_set[0] =
                deposit64(mapper->regs.tlb_op_set[0], 32, 32, val);
        }
        break;
    case R_DART_ERROR_STATUS:
        mapper->regs.error_status &= ~val;
        apple_dart_update_irq(mapper->common.dart);
        break;
    case R_DART_CONFIG:
        mapper->regs.config = val;
        break;
    case R_DART_SID_REMAP(0)...(R_DART_SID_REMAP(DART_MAX_STREAMS) - 1):
        WITH_QEMU_LOCK_GUARD(&mapper->common.mutex)
        {
            i = addr - A_DART_SID_REMAP(0);
            *(uint32_t *)&mapper->regs.sid_remap[i] = val;
        }
        break;
    case R_DART_SID_CONFIG(0)...(R_DART_SID_CONFIG(DART_MAX_STREAMS) - 1):
        WITH_QEMU_LOCK_GUARD(&mapper->common.mutex)
        {
            i = (addr >> 2) - R_DART_SID_CONFIG(0);
            mapper->regs.sid_config[i] = val;
        }
        break;
    case R_DART_TTBR(0, 0)...(R_DART_TTBR(DART_MAX_STREAMS, 0) - 1):
        WITH_QEMU_LOCK_GUARD(&mapper->common.mutex)
        {
            i = (addr >> 2) - R_DART_TTBR(0, 0);
            ((uint32_t *)mapper->regs.ttbr)[i] = val;
        }
        if (getenv("INFERNO_DART_SEP_TRACE") != NULL) {
            const char *dart_id = mapper->common.dart->parent_obj.parent_obj.id;

            if (dart_id != NULL && strstr(dart_id, "sep") != NULL) {
                fprintf(stderr,
                        "DART_SEP: TTBR write idx %u = 0x%" PRIx64
                        " (valid=%d)\n",
                        i, val, (int)((val >> 31) & 1));
            }
        }
        apple_dart_hvf_mirror_rebuild(mapper);
        break;
    default:
        break;
    }
}

static uint64_t apple_dart_mapper_reg_read(void *opaque, hwaddr addr,
                                           unsigned size)
{
    AppleDARTMapperInstance *mapper = opaque;
    uint32_t i;

    DPRINTF("%s[%d]: (DART) 0x" HWADDR_FMT_plx "\n",
            mapper->common.dart->parent_obj.parent_obj.id, mapper->common.id,
            addr);

    switch (addr >> 2) {
    case R_DART_PARAMS1:
        return mapper->regs.params1;
    case R_DART_PARAMS2:
        return mapper->regs.params2;
    case R_DART_TLB_OP:
        return qatomic_read(&mapper->regs.tlb_op);
    case R_DART_TLB_OP_SET_0_LOW:
        return extract64(mapper->regs.tlb_op_set[0], 0, 32);
    case R_DART_TLB_OP_SET_0_HIGH:
        return extract64(mapper->regs.tlb_op_set[0], 32, 32);
    case R_DART_ERROR_STATUS:
        return mapper->regs.error_status;
    case R_DART_ERROR_ADDRESS_LOW:
        return extract64(mapper->regs.error_address, 0, 32);
    case R_DART_ERROR_ADDRESS_HIGH:
        return extract64(mapper->regs.error_address, 32, 32);
    case R_DART_CONFIG:
        return mapper->regs.config;
    case R_DART_SID_REMAP(0)...(R_DART_SID_REMAP(DART_MAX_STREAMS) - 1):
        i = addr - A_DART_SID_REMAP(0);
        return *(uint32_t *)&mapper->regs.sid_remap[i];
    case R_DART_SID_CONFIG(0)...(R_DART_SID_CONFIG(DART_MAX_STREAMS) - 1):
        i = (addr >> 2) - R_DART_SID_CONFIG(0);
        return mapper->regs.sid_config[i];
    case R_DART_TTBR(0, 0)...(R_DART_TTBR(DART_MAX_STREAMS, 0) - 1):
        i = (addr >> 2) - R_DART_TTBR(0, 0);
        return ((uint32_t *)mapper->regs.ttbr)[i];
    default:
        return 0;
    }
}

static const MemoryRegionOps apple_dart_mapper_reg_ops = {
    .write = apple_dart_mapper_reg_write,
    .read = apple_dart_mapper_reg_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void apple_dart_dummy_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                       unsigned size)
{
    AppleDARTInstance *instance = opaque;

    QEMU_LOCK_GUARD(&instance->mutex);

    DPRINTF("%s[%d]: (%s) 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx "\n",
            instance->dart->parent_obj.parent_obj.id, instance->id,
            dart_instance_name[instance->type], addr, data);
}

static uint64_t apple_dart_dummy_reg_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    AppleDARTInstance *instance = opaque;

    QEMU_LOCK_GUARD(&instance->mutex);

    DPRINTF("%s[%d]: (%s) 0x" HWADDR_FMT_plx "\n",
            instance->dart->parent_obj.parent_obj.id, instance->id,
            dart_instance_name[instance->type], addr);

    return 0;
}

static const MemoryRegionOps apple_dart_dummy_reg_ops = {
    .write = apple_dart_dummy_reg_write,
    .read = apple_dart_dummy_reg_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static inline uint32_t apple_dart_mapper_ptw(AppleDARTMapperInstance *mapper,
                                             uint32_t sid, hwaddr iova,
                                             IOMMUTLBEntry *tlb_entry)
{
    AppleDARTState *dart = mapper->common.dart;

    uint64_t idx = (iova & (dart->l_mask[0])) >> dart->l_shift[0];
    uint64_t pte;
    uint64_t pa;
    int level;
    MemTxResult res;

    if (sid >= DART_MAX_STREAMS || (dart->sid_mask & BIT_ULL(sid)) == 0 ||
        idx >= DART_MAX_TTBR ||
        !REG_FIELD_EX32(mapper->regs.ttbr[sid][idx], DART_TTBR, VALID)) {
        return REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1),
                              DART_ERROR_STATUS, TTBR_INVLD, 1);
    }

    pte = mapper->regs.ttbr[sid][idx];
    pa = (pte & DART_TTBR_MASK) << DART_TTBR_SHIFT;

    for (level = 1; level < 3; level++) {
        idx = (iova & (dart->l_mask[level])) >> dart->l_shift[level];
        pa += 8 * idx;

        pte = address_space_ldq(&address_space_memory, pa,
                                MEMTXATTRS_UNSPECIFIED, &res);
        if (res != MEMTX_OK) {
            return REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1),
                                  DART_ERROR_STATUS, L2E_INVLD, 1);
        }
        DPRINTF("%s: level: %d, pa: 0x" HWADDR_FMT_plx " pte: 0x%llx(0x%llx)\n",
                __func__, level, pa, pte, idx);

        if ((pte & DART_PTE_VALID) == 0) {
            return REG_FIELD_DP32(REG_FIELD_DP32(0, DART_ERROR_STATUS, FLAG, 1),
                                  DART_ERROR_STATUS, PTE_INVLD, 1);
        }

        pa = pte & dart->page_mask & DART_PTE_ADDR_MASK;
    }

    tlb_entry->translated_addr = (pte & dart->page_mask & DART_PTE_ADDR_MASK);
    tlb_entry->perm =
        IOMMU_ACCESS_FLAG(!REG_FIELD_EX32(pte, DART_PTE, NO_READ),
                          !REG_FIELD_EX32(pte, DART_PTE, NO_WRITE));

    return 0;
}

/*
 * HVF mirror of a DART stream into the *global* address space.
 *
 * QEMU models the SEP's DMA window by giving the SEP core a private address
 * space in which its low 32 MiB is an alias of this DART (`sep_dma` over
 * `ool_mr`, see sep.c). That is the right model and it works under TCG, where
 * each CPU really does get its own AddressSpace.
 *
 * Under HVF it is silently ineffective. There is exactly one EPT for the whole
 * VM (accel/hvf/hvf-all.c registers a single listener on address_space_memory),
 * so a per-vCPU overlay placed on top of RAM never takes effect: the `SEPFW_`
 * RAM that backs system 0-32 MiB is mapped into the EPT and satisfies the SEP's
 * accesses directly, without ever consulting this DART. The measured result is
 * that the SEP reads the firmware image staged at 0x4000 where the AP's
 * out-of-line message buffers ought to be, and SEPOS's `sks` app rejects every
 * keystore request with -13 -- which is what blocks data protection.
 *
 * The AP's side is entirely correct: it installs a valid TTBR for the SEP
 * stream and page tables that map, for example, iova 0x4000 -> 0x82A628000.
 * So rather than fight HVF, publish those same translations as aliases in the
 * global address space, where HVF *will* map them into the EPT. They are
 * installed above the `SEPFW_` RAM (priority 1), so any iova the AP has not
 * mapped still falls through to it -- which is what SEPROM needs, since it
 * reads the firmware from that RAM before XNU programs the DART at all.
 */
static void apple_dart_hvf_mirror_clear(AppleDARTState *dart)
{
    if (dart->hvf_mirror_regions == NULL) {
        return;
    }

    for (guint i = 0; i < dart->hvf_mirror_regions->len; i++) {
        MemoryRegion *mr = g_ptr_array_index(dart->hvf_mirror_regions, i);

        memory_region_del_subregion(get_system_memory(), mr);
        object_unparent(OBJECT(mr));
    }
    g_ptr_array_set_size(dart->hvf_mirror_regions, 0);
}

/*
 * Walk the stream and publish its translations as aliases. Returns true if the
 * set of translations changed since the last call.
 *
 * This has to be polled rather than driven off register writes. The AP writes
 * the TTBR and invalidates once, early, while its page tables are still empty,
 * and from then on it adds mappings by writing PTEs straight into RAM -- across
 * a whole restore the SEP DART sees exactly twelve register writes and not one
 * of them accompanies a new buffer. Rebuilding only on register traffic
 * therefore always runs too early and finds nothing.
 *
 * The walk itself is cheap (two loads per page) and the expensive part --
 * tearing down and re-creating memory regions, which re-flattens the address
 * space and re-does the EPT mapping -- only happens when something actually
 * changed.
 */
static bool apple_dart_hvf_mirror_walk(AppleDARTMapperInstance *mapper)
{
    AppleDARTState *dart = mapper->common.dart;
    bool trace = getenv("INFERNO_DART_SEP_TRACE") != NULL;
    uint32_t sid = (uint32_t)dart->hvf_mirror_sid;
    uint32_t npages = dart->hvf_mirror_size / dart->page_size;
    g_autofree uint64_t *cur = g_new0(uint64_t, npages);
    bool changed = false;
    uint32_t mapped = 0;
    uint32_t regions = 0;
    uint32_t n;

    for (n = 0; n < npages; n++) {
        IOMMUTLBEntry entry = { 0 };

        /*
         * apple_dart_mapper_ptw() indexes by *page number*, not byte address --
         * apple_dart_mapper_translate() calls it with `addr >> page_shift`.
         * Passing a byte address here makes every level index wrong and the
         * walk silently find nothing.
         */
        if (apple_dart_mapper_ptw(mapper, sid, n, &entry) != 0 ||
            entry.perm == IOMMU_NONE ||
            entry.translated_addr < dart->hvf_mirror_size) {
            continue;
        }
        cur[n] = entry.translated_addr | 1;
        mapped++;
    }

    if (dart->hvf_mirror_last == NULL) {
        dart->hvf_mirror_last = g_new0(uint64_t, npages);
        dart->hvf_mirror_pages = npages;
        changed = mapped != 0;
    } else {
        changed = memcmp(dart->hvf_mirror_last, cur,
                         npages * sizeof(uint64_t)) != 0;
    }

    if (!changed) {
        return false;
    }

    /*
     * Experiment (INFERNO_DART_MIRROR_STICKY=1): hold on to the previous
     * mapping when the AP drops everything.
     *
     * The AP tears the SEP stream down at `seputil --gigalocker-shutdown` and
     * never re-arms the TTBR, so from then on the walk legitimately finds
     * nothing -- and `create_protected_filesystems` runs *after* that point.
     * Its D-key creation is the one keystore call that needs an out-of-line
     * payload, which is why it still fails with -13 while the later in-band
     * commands succeed. Keeping the last good translations alive across the
     * teardown tests whether that is the whole remaining story.
     *
     * Off by default: it is only safe while the AP's buffers stay put, and
     * nothing guarantees that once it has released the mapping.
     */
    if (mapped == 0 && getenv("INFERNO_DART_MIRROR_STICKY") != NULL) {
        if (trace) {
            fprintf(stderr,
                    "DART_SEP: mirror went empty, keeping previous mapping "
                    "(sticky)\n");
        }
        return false;
    }

    memcpy(dart->hvf_mirror_last, cur, npages * sizeof(uint64_t));

    if (dart->hvf_mirror_regions == NULL) {
        dart->hvf_mirror_regions = g_ptr_array_new();
    }

    memory_region_transaction_begin();
    apple_dart_hvf_mirror_clear(dart);

    /*
     * Coalesce runs of pages whose translations are contiguous into a single
     * alias. This is not an optimisation, it is a requirement: the accelerator
     * keeps a fixed-size table of memory slots, and one region per page exhausts
     * it ("No free slots", QEMU exits). The AP's mappings are highly contiguous
     * -- the 16 MiB firmware window arrives as one run of 1044 pages -- so this
     * turns thousands of regions into a handful.
     */
    n = 0;
    while (n < npages) {
        hwaddr iova;
        hwaddr pa;
        uint32_t run;
        MemoryRegion *alias;
        g_autofree char *name = NULL;

        if (cur[n] == 0) {
            n++;
            continue;
        }

        iova = (hwaddr)n * dart->page_size;
        pa = cur[n] & ~1ULL;

        for (run = 1; n + run < npages; run++) {
            if (cur[n + run] == 0 ||
                (cur[n + run] & ~1ULL) != pa + (hwaddr)run * dart->page_size) {
                break;
            }
        }

        name = g_strdup_printf("dart-hvf-mirror@0x%" HWADDR_PRIx, iova);
        alias = g_new0(MemoryRegion, 1);
        memory_region_init_alias(alias, OBJECT(dart), name, get_system_memory(),
                                 pa, (uint64_t)run * dart->page_size);
        memory_region_add_subregion_overlap(get_system_memory(), iova, alias, 1);
        g_ptr_array_add(dart->hvf_mirror_regions, alias);
        regions++;

        if (trace) {
            fprintf(stderr,
                    "DART_SEP: mirror iova 0x%" HWADDR_PRIx " -> 0x%"
                    HWADDR_PRIx " (%u page(s))\n", iova, pa, run);
        }
        n += run;
    }
    memory_region_transaction_commit();

    if (trace) {
        fprintf(stderr, "DART_SEP: mirror rebuilt, %u page(s) in %u region(s)\n",
                mapped, regions);
    }
    return true;
}

#define DART_HVF_MIRROR_POLL_MS 50

static void apple_dart_hvf_mirror_tick(void *opaque)
{
    AppleDARTMapperInstance *mapper = opaque;
    AppleDARTState *dart = mapper->common.dart;

    if (dart->hvf_mirror_sid >= 0 && dart->hvf_mirror_size != 0) {
        apple_dart_hvf_mirror_walk(mapper);
    }

    timer_mod(dart->hvf_mirror_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  DART_HVF_MIRROR_POLL_MS);
}

static void apple_dart_hvf_mirror_rebuild(AppleDARTMapperInstance *mapper)
{
    AppleDARTState *dart = mapper->common.dart;

    if (dart->hvf_mirror_sid < 0 || dart->hvf_mirror_size == 0) {
        return;
    }

    dart->hvf_mirror_mapper = mapper;
    apple_dart_hvf_mirror_walk(mapper);

    if (dart->hvf_mirror_timer == NULL) {
        dart->hvf_mirror_timer =
            timer_new_ms(QEMU_CLOCK_VIRTUAL, apple_dart_hvf_mirror_tick,
                         mapper);
        timer_mod(dart->hvf_mirror_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      DART_HVF_MIRROR_POLL_MS);
    }
}

/*
 * Refresh every mirrored stream right now.
 *
 * The 50 ms poll is a backstop, not the primary trigger: the AP maps a buffer
 * and sends the message that refers to it immediately afterwards, so polling
 * loses the race. Measured on the keystore endpoint, the first six commands
 * after it is enabled are answered `-13` and the seventh succeeds -- the only
 * thing that changes in between is the mirror catching up and publishing
 * iova 0x64000. Refreshing on the way in closes that window.
 */
void apple_dart_hvf_mirror_refresh_all(void)
{
    GSList *darts = apple_dart_get_device_list();
    GSList *iter;

    for (iter = darts; iter != NULL; iter = iter->next) {
        AppleDARTState *dart = APPLE_DART(iter->data);

        if (dart->hvf_mirror_size != 0 && dart->hvf_mirror_mapper != NULL) {
            apple_dart_hvf_mirror_walk(dart->hvf_mirror_mapper);
        }
    }
    g_slist_free(darts);
}

void apple_dart_set_hvf_mirror(AppleDARTState *dart, uint32_t sid,
                               uint64_t size)
{
    /*
     * Only needed under HVF. Under TCG the SEP core's own AddressSpace already
     * routes these accesses through the DART, and mirroring would additionally
     * shadow the same addresses for every other CPU.
     */
    if (!hvf_enabled()) {
        return;
    }

    dart->hvf_mirror_sid = (int32_t)sid;
    dart->hvf_mirror_size = size;
}

static IOMMUTLBEntry apple_dart_mapper_translate(IOMMUMemoryRegion *mr,
                                                 hwaddr addr,
                                                 IOMMUAccessFlags flag,
                                                 int iommu_idx)
{
    AppleDARTIOMMUMemoryRegion *iommu =
        container_of(mr, AppleDARTIOMMUMemoryRegion, iommu);
    AppleDARTMapperInstance *mapper = iommu->mapper;
    AppleDARTState *dart = mapper->common.dart;
    uint32_t sid = iommu->sid;
    uint64_t iova;

    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr,
        .addr_mask = dart->page_bits,
        .perm = IOMMU_NONE,
    };

    if (REG_FIELD_EX32(qatomic_read(&mapper->regs.tlb_op), DART_TLB_OP, BUSY)) {
        return entry;
    }

    QEMU_LOCK_GUARD(&mapper->common.mutex);

    sid = mapper->regs.sid_remap[sid];

    /*
     * Translation disabled, or FULL_BYPASS set, means the stream bypasses the
     * DART. This returns a *failed* translation (perm = IOMMU_NONE).
     *
     * Tried and reverted (twice): returning an identity mapping with IOMMU_RW.
     * It looks right -- SEP's own 0-32 MiB is backed by the "SEPFW_" RAM at
     * system address 0 and is reached through this DART -- but it is wrong in
     * practice. SEPROM and SEPOS boot fine *while every translation fails*,
     * and turning pass-through on makes SEPOS fail at boot stage 1. So they do
     * not reach their firmware through this DART at all; feeding them real
     * data at those addresses changes what they see and breaks the boot.
     *
     * (The first attempt also hung the boot outright, via a separate
     * self-loop: sep.c aliased `ool_mr` back into system memory at
     * s->shmbuf_base, which is 0 unless SEP_ENABLE_TRACE_BUFFER is set.)
     */
    if (REG_FIELD_EX32(mapper->regs.sid_config[sid], DART_SID_CONFIG,
                       TRANSLATION_ENABLE) == 0 ||
        REG_FIELD_EX32(mapper->regs.sid_config[sid], DART_SID_CONFIG,
                       FULL_BYPASS) != 0) {
        goto end;
    }

    iova = addr >> dart->page_shift;

    uint32_t status = apple_dart_mapper_ptw(mapper, sid, iova, &entry);
    if (status != 0) {
        mapper->regs.error_address = addr;
        mapper->regs.error_status =
            REG_FIELD_DP32(mapper->regs.error_status | status,
                           DART_ERROR_STATUS, SID, iommu->sid);
        apple_dart_raise_irq(dart);
        goto end;
    }

    entry.translated_addr |= addr & entry.addr_mask;

    if ((flag & IOMMU_WO) != 0 && (entry.perm & IOMMU_WO) == 0) {
        mapper->regs.error_address = addr;
        mapper->regs.error_status = REG_FIELD_DP32(
            REG_FIELD_DP32(REG_FIELD_DP32(mapper->regs.error_status,
                                          DART_ERROR_STATUS, FLAG, 1),
                           DART_ERROR_STATUS, WRITE_PROT, 1),
            DART_ERROR_STATUS, SID, iommu->sid);
        apple_dart_raise_irq(dart);
    }

    if ((flag & IOMMU_RO) != 0 && (entry.perm & IOMMU_RO) == 0) {
        mapper->regs.error_address = addr;
        mapper->regs.error_status = REG_FIELD_DP32(
            REG_FIELD_DP32(REG_FIELD_DP32(mapper->regs.error_status,
                                          DART_ERROR_STATUS, FLAG, 1),
                           DART_ERROR_STATUS, WRITE_PROT, 1),
            DART_ERROR_STATUS, SID, iommu->sid);
        apple_dart_raise_irq(dart);
    }

end:
    /*
     * Targeted diagnostic: the SEP reaches the AP's out-of-line message buffers
     * through this DART, so a failed translation there is invisible except as
     * zero reads. INFERNO_DART_SEP_TRACE=1 logs just that DART; the unfiltered
     * DPRINTF below covers every DART and is far too noisy to leave on.
     */
    if (getenv("INFERNO_DART_SEP_TRACE") != NULL) {
        const char *dart_id = mapper->common.dart->parent_obj.parent_obj.id;

        if (dart_id != NULL && strstr(dart_id, "sep") != NULL) {
            fprintf(stderr,
                    "DART_SEP: sid %u iova 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx
                    " %c%c\n",
                    iommu->sid, entry.iova, entry.translated_addr,
                    (entry.perm & IOMMU_RO) ? 'r' : '-',
                    (entry.perm & IOMMU_WO) ? 'w' : '-');
        }
    }

    DPRINTF("%s[%d]: (%s) SID %u: 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx
            " (%c%c)\n",
            mapper->common.dart->parent_obj.parent_obj.id, mapper->common.id,
            dart_instance_name[mapper->common.type], iommu->sid, entry.iova,
            entry.translated_addr, (entry.perm & IOMMU_RO) ? 'r' : '-',
            (entry.perm & IOMMU_WO) ? 'w' : '-');
    return entry;
}

static void apple_dart_reset(DeviceState *dev)
{
    AppleDARTState *dart = APPLE_DART(dev);
    AppleDARTMapperInstance *mapper;
    uint32_t i;
    uint32_t j;

    for (i = 0; i < dart->num_instances; i++) {
        switch (dart->instances[i]->type) {
        case DART_DART: {
            mapper = container_of(dart->instances[i], AppleDARTMapperInstance,
                                  common);

            QEMU_LOCK_GUARD(&mapper->common.mutex);
            mapper->regs = (AppleDARTDARTRegs){ 0 };

            mapper->regs.params1 =
                REG_FIELD_DP32(0, DART_PARAMS1, PAGE_SHIFT, dart->page_shift);
            // TODO: added hack against panic
            mapper->regs.params1 = REG_FIELD_DP32(
                mapper->regs.params1, DART_PARAMS1, ACCESS_REGION_PROTECTION,
                (dart->dart_options & BIT(1)) != 0);

            mapper->regs.params2 =
                REG_FIELD_DP32(0, DART_PARAMS2, BYPASS_SUPPORT, 1);

            for (j = 0; j < DART_MAX_STREAMS; j++) {
                mapper->regs.sid_remap[j] = j;
            }
            break;
        }
        default:
            break;
        }
    }
}

static void apple_dart_realize(DeviceState *dev, Error **errp)
{
}

IOMMUMemoryRegion *apple_dart_iommu_mr(AppleDARTState *dart, uint32_t sid)
{
    AppleDARTInstance *instance;
    AppleDARTMapperInstance *mapper;
    uint32_t i;

    if (dart->sid_mask & BIT_ULL(sid)) {
        for (i = 0; i < dart->num_instances; i++) {
            instance = dart->instances[i];
            if (instance->type != DART_DART) {
                continue;
            }

            mapper = container_of(instance, AppleDARTMapperInstance, common);
            return &mapper->iommus[sid]->iommu;
        }
    }
    return NULL;
}

IOMMUMemoryRegion *apple_dart_instance_iommu_mr(AppleDARTState *dart,
                                                uint32_t instance, uint32_t sid)
{
    AppleDARTInstance *o;
    AppleDARTMapperInstance *mapper;

    if (instance >= dart->num_instances ||
        (dart->sid_mask & BIT_ULL(sid)) == 0) {
        return NULL;
    }

    o = dart->instances[instance];
    if (o->type != DART_DART) {
        return NULL;
    }

    mapper = container_of(o, AppleDARTMapperInstance, common);
    return &mapper->iommus[sid]->iommu;
}

AppleDARTState *apple_dart_from_node(AppleDTNode *node)
{
    DeviceState *dev;
    AppleDARTState *dart;
    SysBusDevice *sbd;
    AppleDTProp *prop;
    uint64_t *reg;
    uint32_t *instance_data;
    int i;

    dev = qdev_new(TYPE_APPLE_DART);
    dart = APPLE_DART(dev);
    sbd = SYS_BUS_DEVICE(dev);

    dev->id = apple_dt_get_prop_strdup(node, "name", &error_fatal);

    dart->page_size =
        apple_dt_get_prop_u32_or(node, "page-size", 0x1000, &error_fatal);
    dart->page_shift = 31 - clz32(dart->page_size);
    dart->page_bits = dart->page_size - 1;
    dart->page_mask = ~dart->page_bits;

    switch (dart->page_shift) {
    case 12:
        dart->l_mask[0] = 0xC0000;
        dart->l_mask[1] = 0x3FE00;
        dart->l_mask[2] = 0x1FF;
        dart->l_shift[0] = 0x12;
        dart->l_shift[1] = 9;
        dart->l_shift[2] = 0;
        break;
    case 14:
        dart->l_mask[0] = 0xC00000;
        dart->l_mask[1] = 0x3FF800;
        dart->l_mask[2] = 0x7FF;
        dart->l_shift[0] = 0x16;
        dart->l_shift[1] = 11;
        dart->l_shift[2] = 0;
        break;
    default:
        assert_not_reached();
    }

    // NOTE: there can be up to 64 SIDs. Not on the currently-emulated hardware,
    // but other ones.
    dart->sid_mask =
        apple_dt_get_prop_u32_or(node, "sids", 0xFFFF, &error_fatal);
    dart->dart_options =
        apple_dt_get_prop_u32_or(node, "dart-options", 0, &error_fatal);

    prop = apple_dt_get_prop(node, "instance");
    if (prop == NULL) {
        if (apple_dt_get_prop_u32_or(node, "smmu-present", 0, &error_fatal) ==
            1) {
            instance_data = (uint32_t *)"TRADDART\0\0\0\0UMMSSMMU\0\0\0";
        } else {
            instance_data = (uint32_t *)"TRADDART\0\0\0";
        }
    } else {
        assert_cmpuint(prop->len % 12, ==, 0);
        instance_data = (uint32_t *)prop->data;
    }

    prop = apple_dt_get_prop(node, "reg");
    assert_nonnull(prop);

    reg = (uint64_t *)prop->data;

    dart->num_instances = prop->len / 16;
    dart->instances = g_new(AppleDARTInstance *, dart->num_instances);
    for (i = 0; i < dart->num_instances; i++) {
        AppleDARTInstance *instance;

        switch (ldl_le_p(instance_data)) {
        case 'DART': {
            AppleDARTMapperInstance *mapper =
                g_new0(AppleDARTMapperInstance, 1);
            instance = &mapper->common;
            instance->type = DART_DART;
            memory_region_init_io(&instance->iomem, OBJECT(dev),
                                  &apple_dart_mapper_reg_ops, instance,
                                  TYPE_APPLE_DART ".reg", reg[(i * 2) + 1]);
            for (uint32_t sid = 0; sid < DART_MAX_STREAMS; sid++) {
                if (!(dart->sid_mask & BIT_ULL(sid))) {
                    continue;
                }

                char *name = g_strdup_printf("dart-%s-%u-%u",
                                             dart->parent_obj.parent_obj.id,
                                             instance->id, sid);
                mapper->iommus[sid] = g_new0(AppleDARTIOMMUMemoryRegion, 1);
                mapper->iommus[sid]->sid = sid;
                mapper->iommus[sid]->mapper = mapper;
                memory_region_init_iommu(
                    mapper->iommus[sid], sizeof(AppleDARTIOMMUMemoryRegion),
                    TYPE_APPLE_DART_IOMMU_MEMORY_REGION, OBJECT(dart), name,
                    1ULL << DART_MAX_VA_BITS);
                g_free(name);
            }
            break;
        }
        case 'SMMU':
            instance = g_new0(AppleDARTInstance, 1);
            instance->type = DART_SMMU;
            goto common;
        case 'DAPF':
            instance = g_new0(AppleDARTInstance, 1);
            instance->type = DART_DAPF;
            goto common;
        default:
            instance = g_new0(AppleDARTInstance, 1);
            instance->type = DART_UNKNOWN;
        common:
            memory_region_init_io(&instance->iomem, OBJECT(dev),
                                  &apple_dart_dummy_reg_ops, instance,
                                  TYPE_APPLE_DART ".reg", reg[(i * 2) + 1]);
            break;
        }
        qemu_mutex_init(&instance->mutex);
        instance->id = i;
        instance->dart = dart;
        dart->instances[i] = instance;
        sysbus_init_mmio(sbd, &instance->iomem);
        DPRINTF("%s: DART %s instance %d: %s\n", __func__,
                instance->dart->parent_obj.parent_obj.id, i,
                dart_instance_name[instance->type]);
        instance_data += 3;
    }

    sysbus_init_irq(sbd, &dart->irq);

    return dart;
}

static void apple_dart_dump_pt(Monitor *mon, AppleDARTInstance *instance,
                               hwaddr iova, const uint64_t *entries, int level,
                               uint64_t pte)
{
    AppleDARTState *dart = instance->dart;
    if (level == 3) {
        monitor_printf(mon,
                       "\t\t\t0x" HWADDR_FMT_plx " ... 0x" HWADDR_FMT_plx
                       " -> 0x%llx %c%c\n",
                       iova << dart->page_shift, (iova + 1) << dart->page_shift,
                       pte & dart->page_mask & DART_PTE_ADDR_MASK,
                       REG_FIELD_EX32(pte, DART_PTE, NO_READ) ? '-' : 'r',
                       REG_FIELD_EX32(pte, DART_PTE, NO_WRITE) ? '-' : 'w');
        return;
    }

    for (uint64_t i = 0; i <= (dart->l_mask[level] >> dart->l_shift[level]);
         i++) {
        uint64_t pte2 = entries[i];

        if ((pte2 & DART_PTE_VALID) ||
            ((level == 0) && REG_FIELD_EX32(pte2, DART_TTBR, VALID))) {
            uint64_t pa = pte2 & dart->page_mask & DART_PTE_ADDR_MASK;
            if (level == 0) {
                pa = (pte2 & DART_TTBR_MASK) << DART_TTBR_SHIFT;
            }
            uint64_t next_n_entries = 0;
            if (level < 2) {
                next_n_entries =
                    (dart->l_mask[level + 1] >> dart->l_shift[level + 1]) + 1;
            }
            g_autofree uint64_t *next = g_malloc0(8 * next_n_entries);
            if (dma_memory_read(&address_space_memory, pa, next,
                                8 * next_n_entries,
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                continue;
            }

            apple_dart_dump_pt(mon, instance,
                               iova | (i << dart->l_shift[level]), next,
                               level + 1, pte2);
        }
    }
}

void hmp_info_dart(Monitor *mon, const QDict *qdict)
{
    const char *name = qdict_get_try_str(qdict, "name");
    g_autoptr(GSList) device_list = apple_dart_get_device_list();
    AppleDARTState *dart = NULL;

    if (name == NULL) {
        for (GSList *ele = device_list; ele; ele = ele->next) {
            DeviceState *dev = ele->data;
            dart = APPLE_DART(dev);
            monitor_printf(mon, "%s\tPage size: %d\t%d Instances\n", dev->id,
                           dart->page_size, dart->num_instances);
        }
        return;
    }

    for (GSList *ele = device_list; ele; ele = ele->next) {
        DeviceState *dev = ele->data;
        if (!strcmp(dev->id, name)) {
            dart = APPLE_DART(dev);
            break;
        }
    }

    if (dart == NULL) {
        monitor_printf(mon, "Cannot find dart %s\n", name);
        return;
    }

    for (uint32_t i = 0; i < dart->num_instances; i++) {
        AppleDARTInstance *instance = dart->instances[i];
        monitor_printf(mon, "\tInstance %d: type: %s\n", i,
                       dart_instance_name[instance->type]);
        if (instance->type != DART_DART) {
            continue;
        }
        AppleDARTMapperInstance *mapper =
            container_of(instance, AppleDARTMapperInstance, common);

        for (int sid = 0; sid < DART_MAX_STREAMS; sid++) {
            if (dart->sid_mask & BIT_ULL(sid)) {
                uint8_t remap = mapper->regs.sid_remap[sid];
                if (sid != remap) {
                    monitor_printf(mon, "\t\tSID %d: Remapped to %d\n", sid,
                                   remap);
                    continue;
                }
                if (REG_FIELD_EX32(mapper->regs.sid_config[sid],
                                   DART_SID_CONFIG, TRANSLATION_ENABLE) == 0) {
                    monitor_printf(mon, "\t\tSID %d: Translation disabled\n",
                                   sid);
                    continue;
                }

                if (REG_FIELD_EX32(mapper->regs.sid_config[sid],
                                   DART_SID_CONFIG, FULL_BYPASS) != 0) {
                    monitor_printf(mon, "\t\tSID %d: Translation bypassed\n",
                                   sid);
                    continue;
                }
                monitor_printf(mon, "\t\tSID %d:\n", sid);
                const uint64_t l0_entries[] = { mapper->regs.ttbr[sid][0],
                                                mapper->regs.ttbr[sid][1],
                                                mapper->regs.ttbr[sid][2],
                                                mapper->regs.ttbr[sid][3] };
                apple_dart_dump_pt(mon, instance, 0, l0_entries, 0, 0);
            }
        }
    }
}

// static const VMStateDescription vmstate_apple_dart_instance = {
//     .name = "AppleDARTInstance",
//     .version_id = 1,
//     .minimum_version_id = 1,
//     .fields =
//         (const VMStateField[]){
//             VMSTATE_UINT32_ARRAY(base_reg, AppleDARTInstance,
//                                  0x4000 / sizeof(uint32_t)),
//             VMSTATE_END_OF_LIST(),
//         }
// };
//
// static const VMStateDescription vmstate_apple_dart = {
//     .name = "AppleDARTState",
//     .version_id = 1,
//     .minimum_version_id = 1,
//     .priority = MIG_PRI_IOMMU,
//     .fields =
//         (const VMStateField[]){
//             VMSTATE_STRUCT_ARRAY(instances, AppleDARTState,
//             DART_MAX_INSTANCE,
//                                  1, vmstate_apple_dart_instance,
//                                  AppleDARTInstance),
//             VMSTATE_END_OF_LIST(),
//         }
// };

static void apple_dart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = apple_dart_realize;
    device_class_set_legacy_reset(dc, apple_dart_reset);
    dc->desc = "Apple DART IOMMU";
    // dc->vmsd = &vmstate_apple_dart;
}

static void apple_dart_iommu_memory_region_class_init(ObjectClass *klass,
                                                      const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = apple_dart_mapper_translate;
}

static const TypeInfo apple_dart_info = {
    .name = TYPE_APPLE_DART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleDARTState),
    .class_init = apple_dart_class_init,
};

static const TypeInfo apple_dart_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_APPLE_DART_IOMMU_MEMORY_REGION,
    .class_init = apple_dart_iommu_memory_region_class_init,
};

static void apple_dart_register_types(void)
{
    type_register_static(&apple_dart_info);
    type_register_static(&apple_dart_iommu_memory_region_info);
}

type_init(apple_dart_register_types);
