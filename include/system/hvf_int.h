/*
 * QEMU Hypervisor.framework (HVF) support
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* header to be included in HVF-specific code */

#ifndef HVF_INT_H
#define HVF_INT_H

#include "qemu/queue.h"
#include "exec/vaddr.h"
#include "qom/object.h"
#include "accel/accel-ops.h"

#ifdef __aarch64__
#include <Hypervisor/Hypervisor.h>
typedef hv_vcpu_t hvf_vcpuid;
#else
#include <Hypervisor/hv.h>
typedef hv_vcpuid_t hvf_vcpuid;
#endif

/* hvf_slot flags */
#define HVF_SLOT_LOG (1 << 0)

typedef struct hvf_slot {
    uint64_t start;
    uint64_t size;
    uint8_t *mem;
    int slot_id;
    uint32_t flags;
    MemoryRegion *region;
} hvf_slot;

typedef struct hvf_vcpu_caps {
    uint64_t vmx_cap_pinbased;
    uint64_t vmx_cap_procbased;
    uint64_t vmx_cap_procbased2;
    uint64_t vmx_cap_entry;
    uint64_t vmx_cap_exit;
    uint64_t vmx_cap_preemption_timer;
} hvf_vcpu_caps;

/*
 * Upstream fixes this at 32, which the t8030 machine nearly exhausts on its own
 * (DRAM, SROM, SRAM, SEPROM, DRAM_30/34, the SEP_UNKN* banks, SEPFW_, ...).
 * The SEP DART mirror (hw/arm/apple-silicon/dart.c) then needs a handful more,
 * and running out is fatal -- hvf_set_phys_mem() reports "No free slots" and
 * QEMU exits. Slots are cheap; give the mirror room to breathe.
 */
#define HVF_NUM_SLOTS 256

struct HVFState {
    AccelState parent_obj;

    hvf_slot slots[HVF_NUM_SLOTS];
    int num_slots;

    hvf_vcpu_caps *hvf_caps;
    uint64_t vtimer_offset;
    QTAILQ_HEAD(, hvf_sw_breakpoint) hvf_sw_breakpoints;
};
extern HVFState *hvf_state;

struct AccelCPUState {
    hvf_vcpuid fd;
#ifdef __aarch64__
    hv_vcpu_exit_t *exit;
    bool vtimer_masked;
    sigset_t unblock_ipi_mask;
    bool guest_debug_enabled;
    /*
     * Apple GXF emulation: while the guest is in GL1, the GL1 register
     * bank lives in the real EL1 registers and the EL1 bank is stashed
     * here. See target/arm/hvf/hvf.c.
     */
    struct {
        uint64_t vbar;
        uint64_t tpidr;
        uint64_t spsr;
        uint64_t elr;
        uint64_t esr;
        uint64_t far;
        uint64_t sp_el1;
    } gxf_el1_saved;
    /* INFERNO_HVF_HANG_WATCH bookkeeping; see hvf_hang_tick(). */
    uint64_t hang_last_pc;
    int hang_ticks;
    uint64_t gxf_irq_deferred;
    bool gxf_vtimer_masked;
    uint64_t gxf_vbar_fixed;
    uint64_t gxf_sp_fixed;
    uint64_t gxf_enters;
    uint64_t gxf_sprr_seen;
#define HVF_EV_RING 48
    struct {
        uint64_t pc;
        uint64_t aux;
        uint32_t rep;
        uint8_t kind;
    } ev[HVF_EV_RING];
    uint64_t ev_head;
#endif
};

void assert_hvf_ok_impl(hv_return_t ret, const char *file, unsigned int line,
                        const char *exp);
#define assert_hvf_ok(EX) assert_hvf_ok_impl((EX), __FILE__, __LINE__, #EX)
const char *hvf_return_string(hv_return_t ret);
int hvf_arch_init(void);
hv_return_t hvf_arch_vm_create(MachineState *ms, uint32_t pa_range);
hvf_slot *hvf_find_overlap_slot(uint64_t, uint64_t);
void hvf_kick_vcpu_thread(CPUState *cpu);

/* Must be called by the owning thread */
int hvf_arch_init_vcpu(CPUState *cpu);
/* Must be called by the owning thread */
void hvf_arch_vcpu_destroy(CPUState *cpu);
/* Must be called by the owning thread */
int hvf_arch_vcpu_exec(CPUState *);
/* Must be called by the owning thread */
int hvf_arch_put_registers(CPUState *);
/* Must be called by the owning thread */
int hvf_arch_get_registers(CPUState *);
/* Must be called by the owning thread */
void hvf_arch_update_guest_debug(CPUState *cpu);

struct hvf_sw_breakpoint {
    vaddr pc;
    vaddr saved_insn;
    int use_count;
    QTAILQ_ENTRY(hvf_sw_breakpoint) entry;
};

struct hvf_sw_breakpoint *hvf_find_sw_breakpoint(CPUState *cpu,
                                                 vaddr pc);
int hvf_sw_breakpoints_active(CPUState *cpu);

int hvf_arch_insert_sw_breakpoint(CPUState *cpu, struct hvf_sw_breakpoint *bp);
int hvf_arch_remove_sw_breakpoint(CPUState *cpu, struct hvf_sw_breakpoint *bp);
int hvf_arch_insert_hw_breakpoint(vaddr addr, vaddr len, int type);
int hvf_arch_remove_hw_breakpoint(vaddr addr, vaddr len, int type);
void hvf_arch_remove_all_hw_breakpoints(void);

/*
 * hvf_update_guest_debug:
 * @cs: CPUState for the CPU to update
 *
 * Update guest to enable or disable debugging. Per-arch specifics will be
 * handled by calling down to hvf_arch_update_guest_debug.
 */
int hvf_update_guest_debug(CPUState *cpu);

/*
 * Return whether the guest supports debugging.
 */
bool hvf_arch_supports_guest_debug(void);

#endif
