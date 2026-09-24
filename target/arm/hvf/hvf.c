/*
 * QEMU Hypervisor.framework support for Apple Silicon

 * Copyright 2020 Alexander Graf <agraf@csgraf.de>
 * Copyright 2020 Google LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"

#include "system/runstate.h"
#include "system/hvf.h"
#include "system/hvf_int.h"
#include "system/hw_accel.h"
#include "hvf_arm.h"
#include "cpregs.h"
#include "cpu-sysregs.h"

#include <mach/mach_time.h>

#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "qemu/main-loop.h"
#include "system/cpus.h"
#include "arm-powerctl.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"
#include "target/arm/multiprocessing.h"
#include "target/arm/gtimer.h"
#include "target/arm/trace.h"
#include "trace.h"
#include "migration/vmstate.h"

#include "gdbstub/enums.h"

#include "emulate/aarch64.h"
#include "hw/arm/apple-silicon/gxf-hvf.h"

static void hvf_save_sp(CPUARMState *env);
static void hvf_restore_sp(CPUARMState *env);

#define MDSCR_EL1_SS_SHIFT  0
#define MDSCR_EL1_MDE_SHIFT 15

static const uint16_t dbgbcr_regs[] = {
    HV_SYS_REG_DBGBCR0_EL1,
    HV_SYS_REG_DBGBCR1_EL1,
    HV_SYS_REG_DBGBCR2_EL1,
    HV_SYS_REG_DBGBCR3_EL1,
    HV_SYS_REG_DBGBCR4_EL1,
    HV_SYS_REG_DBGBCR5_EL1,
    HV_SYS_REG_DBGBCR6_EL1,
    HV_SYS_REG_DBGBCR7_EL1,
    HV_SYS_REG_DBGBCR8_EL1,
    HV_SYS_REG_DBGBCR9_EL1,
    HV_SYS_REG_DBGBCR10_EL1,
    HV_SYS_REG_DBGBCR11_EL1,
    HV_SYS_REG_DBGBCR12_EL1,
    HV_SYS_REG_DBGBCR13_EL1,
    HV_SYS_REG_DBGBCR14_EL1,
    HV_SYS_REG_DBGBCR15_EL1,
};

static const uint16_t dbgbvr_regs[] = {
    HV_SYS_REG_DBGBVR0_EL1,
    HV_SYS_REG_DBGBVR1_EL1,
    HV_SYS_REG_DBGBVR2_EL1,
    HV_SYS_REG_DBGBVR3_EL1,
    HV_SYS_REG_DBGBVR4_EL1,
    HV_SYS_REG_DBGBVR5_EL1,
    HV_SYS_REG_DBGBVR6_EL1,
    HV_SYS_REG_DBGBVR7_EL1,
    HV_SYS_REG_DBGBVR8_EL1,
    HV_SYS_REG_DBGBVR9_EL1,
    HV_SYS_REG_DBGBVR10_EL1,
    HV_SYS_REG_DBGBVR11_EL1,
    HV_SYS_REG_DBGBVR12_EL1,
    HV_SYS_REG_DBGBVR13_EL1,
    HV_SYS_REG_DBGBVR14_EL1,
    HV_SYS_REG_DBGBVR15_EL1,
};

static const uint16_t dbgwcr_regs[] = {
    HV_SYS_REG_DBGWCR0_EL1,
    HV_SYS_REG_DBGWCR1_EL1,
    HV_SYS_REG_DBGWCR2_EL1,
    HV_SYS_REG_DBGWCR3_EL1,
    HV_SYS_REG_DBGWCR4_EL1,
    HV_SYS_REG_DBGWCR5_EL1,
    HV_SYS_REG_DBGWCR6_EL1,
    HV_SYS_REG_DBGWCR7_EL1,
    HV_SYS_REG_DBGWCR8_EL1,
    HV_SYS_REG_DBGWCR9_EL1,
    HV_SYS_REG_DBGWCR10_EL1,
    HV_SYS_REG_DBGWCR11_EL1,
    HV_SYS_REG_DBGWCR12_EL1,
    HV_SYS_REG_DBGWCR13_EL1,
    HV_SYS_REG_DBGWCR14_EL1,
    HV_SYS_REG_DBGWCR15_EL1,
};

static const uint16_t dbgwvr_regs[] = {
    HV_SYS_REG_DBGWVR0_EL1,
    HV_SYS_REG_DBGWVR1_EL1,
    HV_SYS_REG_DBGWVR2_EL1,
    HV_SYS_REG_DBGWVR3_EL1,
    HV_SYS_REG_DBGWVR4_EL1,
    HV_SYS_REG_DBGWVR5_EL1,
    HV_SYS_REG_DBGWVR6_EL1,
    HV_SYS_REG_DBGWVR7_EL1,
    HV_SYS_REG_DBGWVR8_EL1,
    HV_SYS_REG_DBGWVR9_EL1,
    HV_SYS_REG_DBGWVR10_EL1,
    HV_SYS_REG_DBGWVR11_EL1,
    HV_SYS_REG_DBGWVR12_EL1,
    HV_SYS_REG_DBGWVR13_EL1,
    HV_SYS_REG_DBGWVR14_EL1,
    HV_SYS_REG_DBGWVR15_EL1,
};

static inline int hvf_arm_num_brps(hv_vcpu_config_t config)
{
    uint64_t val;
    hv_return_t ret;
    ret = hv_vcpu_config_get_feature_reg(config, HV_FEATURE_REG_ID_AA64DFR0_EL1,
                                         &val);
    assert_hvf_ok(ret);
    return REG_FIELD_EX64(val, ID_AA64DFR0, BRPS) + 1;
}

static inline int hvf_arm_num_wrps(hv_vcpu_config_t config)
{
    uint64_t val;
    hv_return_t ret;
    ret = hv_vcpu_config_get_feature_reg(config, HV_FEATURE_REG_ID_AA64DFR0_EL1,
                                         &val);
    assert_hvf_ok(ret);
    return REG_FIELD_EX64(val, ID_AA64DFR0, WRPS) + 1;
}

void hvf_arm_init_debug(void)
{
    hv_vcpu_config_t config;
    config = hv_vcpu_config_create();

    max_hw_bps = hvf_arm_num_brps(config);
    hw_breakpoints =
        g_array_sized_new(true, true, sizeof(HWBreakpoint), max_hw_bps);

    max_hw_wps = hvf_arm_num_wrps(config);
    hw_watchpoints =
        g_array_sized_new(true, true, sizeof(HWWatchpoint), max_hw_wps);

    os_release(config);
}

#define SYSREG_OP0_SHIFT      20
#define SYSREG_OP0_MASK       0x3
#define SYSREG_OP0(sysreg)    ((sysreg >> SYSREG_OP0_SHIFT) & SYSREG_OP0_MASK)
#define SYSREG_OP1_SHIFT      14
#define SYSREG_OP1_MASK       0x7
#define SYSREG_OP1(sysreg)    ((sysreg >> SYSREG_OP1_SHIFT) & SYSREG_OP1_MASK)
#define SYSREG_CRN_SHIFT      10
#define SYSREG_CRN_MASK       0xf
#define SYSREG_CRN(sysreg)    ((sysreg >> SYSREG_CRN_SHIFT) & SYSREG_CRN_MASK)
#define SYSREG_CRM_SHIFT      1
#define SYSREG_CRM_MASK       0xf
#define SYSREG_CRM(sysreg)    ((sysreg >> SYSREG_CRM_SHIFT) & SYSREG_CRM_MASK)
#define SYSREG_OP2_SHIFT      17
#define SYSREG_OP2_MASK       0x7
#define SYSREG_OP2(sysreg)    ((sysreg >> SYSREG_OP2_SHIFT) & SYSREG_OP2_MASK)

#define SYSREG(op0, op1, crn, crm, op2) \
    ((op0 << SYSREG_OP0_SHIFT) | \
     (op1 << SYSREG_OP1_SHIFT) | \
     (crn << SYSREG_CRN_SHIFT) | \
     (crm << SYSREG_CRM_SHIFT) | \
     (op2 << SYSREG_OP2_SHIFT))
#define SYSREG_MASK \
    SYSREG(SYSREG_OP0_MASK, \
           SYSREG_OP1_MASK, \
           SYSREG_CRN_MASK, \
           SYSREG_CRM_MASK, \
           SYSREG_OP2_MASK)
#define SYSREG_OSLAR_EL1      SYSREG(2, 0, 1, 0, 4)
#define SYSREG_OSLSR_EL1      SYSREG(2, 0, 1, 1, 4)
#define SYSREG_OSDLR_EL1      SYSREG(2, 0, 1, 3, 4)
#define SYSREG_LORC_EL1       SYSREG(3, 0, 10, 4, 3)
#define SYSREG_CNTPCT_EL0     SYSREG(3, 3, 14, 0, 1)
#define SYSREG_CNTP_CTL_EL0   SYSREG(3, 3, 14, 2, 1)
#define SYSREG_PMCR_EL0       SYSREG(3, 3, 9, 12, 0)
#define SYSREG_PMUSERENR_EL0  SYSREG(3, 3, 9, 14, 0)
#define SYSREG_PMCNTENSET_EL0 SYSREG(3, 3, 9, 12, 1)
#define SYSREG_PMCNTENCLR_EL0 SYSREG(3, 3, 9, 12, 2)
#define SYSREG_PMINTENCLR_EL1 SYSREG(3, 0, 9, 14, 2)
#define SYSREG_PMOVSCLR_EL0   SYSREG(3, 3, 9, 12, 3)
#define SYSREG_PMSWINC_EL0    SYSREG(3, 3, 9, 12, 4)
#define SYSREG_PMSELR_EL0     SYSREG(3, 3, 9, 12, 5)
#define SYSREG_PMCEID0_EL0    SYSREG(3, 3, 9, 12, 6)
#define SYSREG_PMCEID1_EL0    SYSREG(3, 3, 9, 12, 7)
#define SYSREG_PMCCNTR_EL0    SYSREG(3, 3, 9, 13, 0)
#define SYSREG_PMCCFILTR_EL0  SYSREG(3, 3, 14, 15, 7)

#define SYSREG_ICC_AP0R0_EL1     SYSREG(3, 0, 12, 8, 4)
#define SYSREG_ICC_AP0R1_EL1     SYSREG(3, 0, 12, 8, 5)
#define SYSREG_ICC_AP0R2_EL1     SYSREG(3, 0, 12, 8, 6)
#define SYSREG_ICC_AP0R3_EL1     SYSREG(3, 0, 12, 8, 7)
#define SYSREG_ICC_AP1R0_EL1     SYSREG(3, 0, 12, 9, 0)
#define SYSREG_ICC_AP1R1_EL1     SYSREG(3, 0, 12, 9, 1)
#define SYSREG_ICC_AP1R2_EL1     SYSREG(3, 0, 12, 9, 2)
#define SYSREG_ICC_AP1R3_EL1     SYSREG(3, 0, 12, 9, 3)
#define SYSREG_ICC_ASGI1R_EL1    SYSREG(3, 0, 12, 11, 6)
#define SYSREG_ICC_BPR0_EL1      SYSREG(3, 0, 12, 8, 3)
#define SYSREG_ICC_BPR1_EL1      SYSREG(3, 0, 12, 12, 3)
#define SYSREG_ICC_CTLR_EL1      SYSREG(3, 0, 12, 12, 4)
#define SYSREG_ICC_DIR_EL1       SYSREG(3, 0, 12, 11, 1)
#define SYSREG_ICC_EOIR0_EL1     SYSREG(3, 0, 12, 8, 1)
#define SYSREG_ICC_EOIR1_EL1     SYSREG(3, 0, 12, 12, 1)
#define SYSREG_ICC_HPPIR0_EL1    SYSREG(3, 0, 12, 8, 2)
#define SYSREG_ICC_HPPIR1_EL1    SYSREG(3, 0, 12, 12, 2)
#define SYSREG_ICC_IAR0_EL1      SYSREG(3, 0, 12, 8, 0)
#define SYSREG_ICC_IAR1_EL1      SYSREG(3, 0, 12, 12, 0)
#define SYSREG_ICC_IGRPEN0_EL1   SYSREG(3, 0, 12, 12, 6)
#define SYSREG_ICC_IGRPEN1_EL1   SYSREG(3, 0, 12, 12, 7)
#define SYSREG_ICC_PMR_EL1       SYSREG(3, 0, 4, 6, 0)
#define SYSREG_ICC_RPR_EL1       SYSREG(3, 0, 12, 11, 3)
#define SYSREG_ICC_SGI0R_EL1     SYSREG(3, 0, 12, 11, 7)
#define SYSREG_ICC_SGI1R_EL1     SYSREG(3, 0, 12, 11, 5)
#define SYSREG_ICC_SRE_EL1       SYSREG(3, 0, 12, 12, 5)

#define SYSREG_MDSCR_EL1      SYSREG(2, 0, 0, 2, 2)
#define SYSREG_DBGBVR0_EL1    SYSREG(2, 0, 0, 0, 4)
#define SYSREG_DBGBCR0_EL1    SYSREG(2, 0, 0, 0, 5)
#define SYSREG_DBGWVR0_EL1    SYSREG(2, 0, 0, 0, 6)
#define SYSREG_DBGWCR0_EL1    SYSREG(2, 0, 0, 0, 7)
#define SYSREG_DBGBVR1_EL1    SYSREG(2, 0, 0, 1, 4)
#define SYSREG_DBGBCR1_EL1    SYSREG(2, 0, 0, 1, 5)
#define SYSREG_DBGWVR1_EL1    SYSREG(2, 0, 0, 1, 6)
#define SYSREG_DBGWCR1_EL1    SYSREG(2, 0, 0, 1, 7)
#define SYSREG_DBGBVR2_EL1    SYSREG(2, 0, 0, 2, 4)
#define SYSREG_DBGBCR2_EL1    SYSREG(2, 0, 0, 2, 5)
#define SYSREG_DBGWVR2_EL1    SYSREG(2, 0, 0, 2, 6)
#define SYSREG_DBGWCR2_EL1    SYSREG(2, 0, 0, 2, 7)
#define SYSREG_DBGBVR3_EL1    SYSREG(2, 0, 0, 3, 4)
#define SYSREG_DBGBCR3_EL1    SYSREG(2, 0, 0, 3, 5)
#define SYSREG_DBGWVR3_EL1    SYSREG(2, 0, 0, 3, 6)
#define SYSREG_DBGWCR3_EL1    SYSREG(2, 0, 0, 3, 7)
#define SYSREG_DBGBVR4_EL1    SYSREG(2, 0, 0, 4, 4)
#define SYSREG_DBGBCR4_EL1    SYSREG(2, 0, 0, 4, 5)
#define SYSREG_DBGWVR4_EL1    SYSREG(2, 0, 0, 4, 6)
#define SYSREG_DBGWCR4_EL1    SYSREG(2, 0, 0, 4, 7)
#define SYSREG_DBGBVR5_EL1    SYSREG(2, 0, 0, 5, 4)
#define SYSREG_DBGBCR5_EL1    SYSREG(2, 0, 0, 5, 5)
#define SYSREG_DBGWVR5_EL1    SYSREG(2, 0, 0, 5, 6)
#define SYSREG_DBGWCR5_EL1    SYSREG(2, 0, 0, 5, 7)
#define SYSREG_DBGBVR6_EL1    SYSREG(2, 0, 0, 6, 4)
#define SYSREG_DBGBCR6_EL1    SYSREG(2, 0, 0, 6, 5)
#define SYSREG_DBGWVR6_EL1    SYSREG(2, 0, 0, 6, 6)
#define SYSREG_DBGWCR6_EL1    SYSREG(2, 0, 0, 6, 7)
#define SYSREG_DBGBVR7_EL1    SYSREG(2, 0, 0, 7, 4)
#define SYSREG_DBGBCR7_EL1    SYSREG(2, 0, 0, 7, 5)
#define SYSREG_DBGWVR7_EL1    SYSREG(2, 0, 0, 7, 6)
#define SYSREG_DBGWCR7_EL1    SYSREG(2, 0, 0, 7, 7)
#define SYSREG_DBGBVR8_EL1    SYSREG(2, 0, 0, 8, 4)
#define SYSREG_DBGBCR8_EL1    SYSREG(2, 0, 0, 8, 5)
#define SYSREG_DBGWVR8_EL1    SYSREG(2, 0, 0, 8, 6)
#define SYSREG_DBGWCR8_EL1    SYSREG(2, 0, 0, 8, 7)
#define SYSREG_DBGBVR9_EL1    SYSREG(2, 0, 0, 9, 4)
#define SYSREG_DBGBCR9_EL1    SYSREG(2, 0, 0, 9, 5)
#define SYSREG_DBGWVR9_EL1    SYSREG(2, 0, 0, 9, 6)
#define SYSREG_DBGWCR9_EL1    SYSREG(2, 0, 0, 9, 7)
#define SYSREG_DBGBVR10_EL1   SYSREG(2, 0, 0, 10, 4)
#define SYSREG_DBGBCR10_EL1   SYSREG(2, 0, 0, 10, 5)
#define SYSREG_DBGWVR10_EL1   SYSREG(2, 0, 0, 10, 6)
#define SYSREG_DBGWCR10_EL1   SYSREG(2, 0, 0, 10, 7)
#define SYSREG_DBGBVR11_EL1   SYSREG(2, 0, 0, 11, 4)
#define SYSREG_DBGBCR11_EL1   SYSREG(2, 0, 0, 11, 5)
#define SYSREG_DBGWVR11_EL1   SYSREG(2, 0, 0, 11, 6)
#define SYSREG_DBGWCR11_EL1   SYSREG(2, 0, 0, 11, 7)
#define SYSREG_DBGBVR12_EL1   SYSREG(2, 0, 0, 12, 4)
#define SYSREG_DBGBCR12_EL1   SYSREG(2, 0, 0, 12, 5)
#define SYSREG_DBGWVR12_EL1   SYSREG(2, 0, 0, 12, 6)
#define SYSREG_DBGWCR12_EL1   SYSREG(2, 0, 0, 12, 7)
#define SYSREG_DBGBVR13_EL1   SYSREG(2, 0, 0, 13, 4)
#define SYSREG_DBGBCR13_EL1   SYSREG(2, 0, 0, 13, 5)
#define SYSREG_DBGWVR13_EL1   SYSREG(2, 0, 0, 13, 6)
#define SYSREG_DBGWCR13_EL1   SYSREG(2, 0, 0, 13, 7)
#define SYSREG_DBGBVR14_EL1   SYSREG(2, 0, 0, 14, 4)
#define SYSREG_DBGBCR14_EL1   SYSREG(2, 0, 0, 14, 5)
#define SYSREG_DBGWVR14_EL1   SYSREG(2, 0, 0, 14, 6)
#define SYSREG_DBGWCR14_EL1   SYSREG(2, 0, 0, 14, 7)
#define SYSREG_DBGBVR15_EL1   SYSREG(2, 0, 0, 15, 4)
#define SYSREG_DBGBCR15_EL1   SYSREG(2, 0, 0, 15, 5)
#define SYSREG_DBGWVR15_EL1   SYSREG(2, 0, 0, 15, 6)
#define SYSREG_DBGWCR15_EL1   SYSREG(2, 0, 0, 15, 7)

#define WFX_IS_WFE (1 << 0)

#define TMR_CTL_ENABLE  (1 << 0)
#define TMR_CTL_IMASK   (1 << 1)
#define TMR_CTL_ISTATUS (1 << 2)

static void hvf_wfi(CPUState *cpu);

static uint32_t chosen_ipa_bit_size;

typedef struct HVFVTimer {
    /* Vtimer value during migration and paused state */
    uint64_t vtimer_val;
} HVFVTimer;

static HVFVTimer vtimer;

typedef struct ARMHostCPUFeatures {
    ARMISARegisters isar;
    uint64_t features;
    uint64_t midr;
    uint32_t reset_sctlr;
    const char *dtb_compatible;
} ARMHostCPUFeatures;

static ARMHostCPUFeatures arm_host_cpu_features;

struct hvf_reg_match {
    int reg;
    uint64_t offset;
};

static const struct hvf_reg_match hvf_reg_match[] = {
    { HV_REG_X0,   offsetof(CPUARMState, xregs[0]) },
    { HV_REG_X1,   offsetof(CPUARMState, xregs[1]) },
    { HV_REG_X2,   offsetof(CPUARMState, xregs[2]) },
    { HV_REG_X3,   offsetof(CPUARMState, xregs[3]) },
    { HV_REG_X4,   offsetof(CPUARMState, xregs[4]) },
    { HV_REG_X5,   offsetof(CPUARMState, xregs[5]) },
    { HV_REG_X6,   offsetof(CPUARMState, xregs[6]) },
    { HV_REG_X7,   offsetof(CPUARMState, xregs[7]) },
    { HV_REG_X8,   offsetof(CPUARMState, xregs[8]) },
    { HV_REG_X9,   offsetof(CPUARMState, xregs[9]) },
    { HV_REG_X10,  offsetof(CPUARMState, xregs[10]) },
    { HV_REG_X11,  offsetof(CPUARMState, xregs[11]) },
    { HV_REG_X12,  offsetof(CPUARMState, xregs[12]) },
    { HV_REG_X13,  offsetof(CPUARMState, xregs[13]) },
    { HV_REG_X14,  offsetof(CPUARMState, xregs[14]) },
    { HV_REG_X15,  offsetof(CPUARMState, xregs[15]) },
    { HV_REG_X16,  offsetof(CPUARMState, xregs[16]) },
    { HV_REG_X17,  offsetof(CPUARMState, xregs[17]) },
    { HV_REG_X18,  offsetof(CPUARMState, xregs[18]) },
    { HV_REG_X19,  offsetof(CPUARMState, xregs[19]) },
    { HV_REG_X20,  offsetof(CPUARMState, xregs[20]) },
    { HV_REG_X21,  offsetof(CPUARMState, xregs[21]) },
    { HV_REG_X22,  offsetof(CPUARMState, xregs[22]) },
    { HV_REG_X23,  offsetof(CPUARMState, xregs[23]) },
    { HV_REG_X24,  offsetof(CPUARMState, xregs[24]) },
    { HV_REG_X25,  offsetof(CPUARMState, xregs[25]) },
    { HV_REG_X26,  offsetof(CPUARMState, xregs[26]) },
    { HV_REG_X27,  offsetof(CPUARMState, xregs[27]) },
    { HV_REG_X28,  offsetof(CPUARMState, xregs[28]) },
    { HV_REG_X29,  offsetof(CPUARMState, xregs[29]) },
    { HV_REG_X30,  offsetof(CPUARMState, xregs[30]) },
    { HV_REG_PC,   offsetof(CPUARMState, pc) },
};

static const struct hvf_reg_match hvf_fpreg_match[] = {
    { HV_SIMD_FP_REG_Q0,  offsetof(CPUARMState, vfp.zregs[0]) },
    { HV_SIMD_FP_REG_Q1,  offsetof(CPUARMState, vfp.zregs[1]) },
    { HV_SIMD_FP_REG_Q2,  offsetof(CPUARMState, vfp.zregs[2]) },
    { HV_SIMD_FP_REG_Q3,  offsetof(CPUARMState, vfp.zregs[3]) },
    { HV_SIMD_FP_REG_Q4,  offsetof(CPUARMState, vfp.zregs[4]) },
    { HV_SIMD_FP_REG_Q5,  offsetof(CPUARMState, vfp.zregs[5]) },
    { HV_SIMD_FP_REG_Q6,  offsetof(CPUARMState, vfp.zregs[6]) },
    { HV_SIMD_FP_REG_Q7,  offsetof(CPUARMState, vfp.zregs[7]) },
    { HV_SIMD_FP_REG_Q8,  offsetof(CPUARMState, vfp.zregs[8]) },
    { HV_SIMD_FP_REG_Q9,  offsetof(CPUARMState, vfp.zregs[9]) },
    { HV_SIMD_FP_REG_Q10, offsetof(CPUARMState, vfp.zregs[10]) },
    { HV_SIMD_FP_REG_Q11, offsetof(CPUARMState, vfp.zregs[11]) },
    { HV_SIMD_FP_REG_Q12, offsetof(CPUARMState, vfp.zregs[12]) },
    { HV_SIMD_FP_REG_Q13, offsetof(CPUARMState, vfp.zregs[13]) },
    { HV_SIMD_FP_REG_Q14, offsetof(CPUARMState, vfp.zregs[14]) },
    { HV_SIMD_FP_REG_Q15, offsetof(CPUARMState, vfp.zregs[15]) },
    { HV_SIMD_FP_REG_Q16, offsetof(CPUARMState, vfp.zregs[16]) },
    { HV_SIMD_FP_REG_Q17, offsetof(CPUARMState, vfp.zregs[17]) },
    { HV_SIMD_FP_REG_Q18, offsetof(CPUARMState, vfp.zregs[18]) },
    { HV_SIMD_FP_REG_Q19, offsetof(CPUARMState, vfp.zregs[19]) },
    { HV_SIMD_FP_REG_Q20, offsetof(CPUARMState, vfp.zregs[20]) },
    { HV_SIMD_FP_REG_Q21, offsetof(CPUARMState, vfp.zregs[21]) },
    { HV_SIMD_FP_REG_Q22, offsetof(CPUARMState, vfp.zregs[22]) },
    { HV_SIMD_FP_REG_Q23, offsetof(CPUARMState, vfp.zregs[23]) },
    { HV_SIMD_FP_REG_Q24, offsetof(CPUARMState, vfp.zregs[24]) },
    { HV_SIMD_FP_REG_Q25, offsetof(CPUARMState, vfp.zregs[25]) },
    { HV_SIMD_FP_REG_Q26, offsetof(CPUARMState, vfp.zregs[26]) },
    { HV_SIMD_FP_REG_Q27, offsetof(CPUARMState, vfp.zregs[27]) },
    { HV_SIMD_FP_REG_Q28, offsetof(CPUARMState, vfp.zregs[28]) },
    { HV_SIMD_FP_REG_Q29, offsetof(CPUARMState, vfp.zregs[29]) },
    { HV_SIMD_FP_REG_Q30, offsetof(CPUARMState, vfp.zregs[30]) },
    { HV_SIMD_FP_REG_Q31, offsetof(CPUARMState, vfp.zregs[31]) },
};

/*
 * QEMU uses KVM system register ids in the migration format.
 * Conveniently, HVF uses the same encoding of the op* and cr* parameters
 * within the low 16 bits of the ids.  Thus conversion between the
 * formats is trivial.
 */

#define KVMID_TO_HVF(KVM)  ((KVM) & 0xffff)
#define HVF_TO_KVMID(HVF)  \
    (CP_REG_ARM64 | CP_REG_SIZE_U64 | CP_REG_ARM64_SYSREG | (HVF))

/* Verify this at compile-time. */

#define DEF_SYSREG(HVF_ID, ...) \
  QEMU_BUILD_BUG_ON(HVF_ID != KVMID_TO_HVF(KVMID_AA64_SYS_REG64(__VA_ARGS__)));

#include "sysreg.c.inc"

#undef DEF_SYSREG

#define DEF_SYSREG(HVF_ID, op0, op1, crn, crm, op2)  HVF_ID,

static const hv_sys_reg_t hvf_sreg_list[] = {
#include "sysreg.c.inc"
};

#undef DEF_SYSREG

int hvf_arch_get_registers(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    hv_return_t ret;
    uint64_t val;
    hv_simd_fp_uchar16_t fpval;
    int i, n;

    for (i = 0; i < ARRAY_SIZE(hvf_reg_match); i++) {
        ret = hv_vcpu_get_reg(cpu->accel->fd, hvf_reg_match[i].reg, &val);
        *(uint64_t *)((void *)env + hvf_reg_match[i].offset) = val;
        assert_hvf_ok(ret);
    }

    for (i = 0; i < ARRAY_SIZE(hvf_fpreg_match); i++) {
        ret = hv_vcpu_get_simd_fp_reg(cpu->accel->fd, hvf_fpreg_match[i].reg,
                                      &fpval);
        memcpy((void *)env + hvf_fpreg_match[i].offset, &fpval, sizeof(fpval));
        assert_hvf_ok(ret);
    }

    val = 0;
    ret = hv_vcpu_get_reg(cpu->accel->fd, HV_REG_FPCR, &val);
    assert_hvf_ok(ret);
    vfp_set_fpcr(env, val);

    val = 0;
    ret = hv_vcpu_get_reg(cpu->accel->fd, HV_REG_FPSR, &val);
    assert_hvf_ok(ret);
    vfp_set_fpsr(env, val);

    ret = hv_vcpu_get_reg(cpu->accel->fd, HV_REG_CPSR, &val);
    assert_hvf_ok(ret);
    pstate_write(env, val);

    for (i = 0, n = arm_cpu->cpreg_array_len; i < n; i++) {
        uint64_t kvm_id = arm_cpu->cpreg_indexes[i];
        int hvf_id = KVMID_TO_HVF(kvm_id);

        if (cpu->accel->guest_debug_enabled) {
            /* Handle debug registers */
            switch (hvf_id) {
            case HV_SYS_REG_DBGBVR0_EL1:
            case HV_SYS_REG_DBGBCR0_EL1:
            case HV_SYS_REG_DBGWVR0_EL1:
            case HV_SYS_REG_DBGWCR0_EL1:
            case HV_SYS_REG_DBGBVR1_EL1:
            case HV_SYS_REG_DBGBCR1_EL1:
            case HV_SYS_REG_DBGWVR1_EL1:
            case HV_SYS_REG_DBGWCR1_EL1:
            case HV_SYS_REG_DBGBVR2_EL1:
            case HV_SYS_REG_DBGBCR2_EL1:
            case HV_SYS_REG_DBGWVR2_EL1:
            case HV_SYS_REG_DBGWCR2_EL1:
            case HV_SYS_REG_DBGBVR3_EL1:
            case HV_SYS_REG_DBGBCR3_EL1:
            case HV_SYS_REG_DBGWVR3_EL1:
            case HV_SYS_REG_DBGWCR3_EL1:
            case HV_SYS_REG_DBGBVR4_EL1:
            case HV_SYS_REG_DBGBCR4_EL1:
            case HV_SYS_REG_DBGWVR4_EL1:
            case HV_SYS_REG_DBGWCR4_EL1:
            case HV_SYS_REG_DBGBVR5_EL1:
            case HV_SYS_REG_DBGBCR5_EL1:
            case HV_SYS_REG_DBGWVR5_EL1:
            case HV_SYS_REG_DBGWCR5_EL1:
            case HV_SYS_REG_DBGBVR6_EL1:
            case HV_SYS_REG_DBGBCR6_EL1:
            case HV_SYS_REG_DBGWVR6_EL1:
            case HV_SYS_REG_DBGWCR6_EL1:
            case HV_SYS_REG_DBGBVR7_EL1:
            case HV_SYS_REG_DBGBCR7_EL1:
            case HV_SYS_REG_DBGWVR7_EL1:
            case HV_SYS_REG_DBGWCR7_EL1:
            case HV_SYS_REG_DBGBVR8_EL1:
            case HV_SYS_REG_DBGBCR8_EL1:
            case HV_SYS_REG_DBGWVR8_EL1:
            case HV_SYS_REG_DBGWCR8_EL1:
            case HV_SYS_REG_DBGBVR9_EL1:
            case HV_SYS_REG_DBGBCR9_EL1:
            case HV_SYS_REG_DBGWVR9_EL1:
            case HV_SYS_REG_DBGWCR9_EL1:
            case HV_SYS_REG_DBGBVR10_EL1:
            case HV_SYS_REG_DBGBCR10_EL1:
            case HV_SYS_REG_DBGWVR10_EL1:
            case HV_SYS_REG_DBGWCR10_EL1:
            case HV_SYS_REG_DBGBVR11_EL1:
            case HV_SYS_REG_DBGBCR11_EL1:
            case HV_SYS_REG_DBGWVR11_EL1:
            case HV_SYS_REG_DBGWCR11_EL1:
            case HV_SYS_REG_DBGBVR12_EL1:
            case HV_SYS_REG_DBGBCR12_EL1:
            case HV_SYS_REG_DBGWVR12_EL1:
            case HV_SYS_REG_DBGWCR12_EL1:
            case HV_SYS_REG_DBGBVR13_EL1:
            case HV_SYS_REG_DBGBCR13_EL1:
            case HV_SYS_REG_DBGWVR13_EL1:
            case HV_SYS_REG_DBGWCR13_EL1:
            case HV_SYS_REG_DBGBVR14_EL1:
            case HV_SYS_REG_DBGBCR14_EL1:
            case HV_SYS_REG_DBGWVR14_EL1:
            case HV_SYS_REG_DBGWCR14_EL1:
            case HV_SYS_REG_DBGBVR15_EL1:
            case HV_SYS_REG_DBGBCR15_EL1:
            case HV_SYS_REG_DBGWVR15_EL1:
            case HV_SYS_REG_DBGWCR15_EL1: {
                /*
                 * If the guest is being debugged, the vCPU's debug registers
                 * are holding the gdbstub's view of the registers (set in
                 * hvf_arch_update_guest_debug()).
                 * Since the environment is used to store only the guest's view
                 * of the registers, don't update it with the values from the
                 * vCPU but simply keep the values from the previous
                 * environment.
                 */
                uint32_t key = kvm_to_cpreg_id(kvm_id);
                const ARMCPRegInfo *ri =
                    ARMCPRegTable_cget(arm_cpu->cp_regs, key);

                val = read_raw_cp_reg(env, ri);

                arm_cpu->cpreg_values[i] = val;
                continue;
            }
            }
        }

        ret = hv_vcpu_get_sys_reg(cpu->accel->fd, hvf_id, &val);
        assert_hvf_ok(ret);

        arm_cpu->cpreg_values[i] = val;
    }
    assert(write_list_to_cpustate(arm_cpu));

    hvf_restore_sp(env);

    return 0;
}

int hvf_arch_put_registers(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    hv_return_t ret;
    uint64_t val;
    hv_simd_fp_uchar16_t fpval;
    int i, n;
    bool b;

    for (i = 0; i < ARRAY_SIZE(hvf_reg_match); i++) {
        val = *(uint64_t *)((void *)env + hvf_reg_match[i].offset);
        ret = hv_vcpu_set_reg(cpu->accel->fd, hvf_reg_match[i].reg, val);
        assert_hvf_ok(ret);
    }

    for (i = 0; i < ARRAY_SIZE(hvf_fpreg_match); i++) {
        memcpy(&fpval, (void *)env + hvf_fpreg_match[i].offset, sizeof(fpval));
        ret = hv_vcpu_set_simd_fp_reg(cpu->accel->fd, hvf_fpreg_match[i].reg,
                                      fpval);
        assert_hvf_ok(ret);
    }

    ret = hv_vcpu_set_reg(cpu->accel->fd, HV_REG_FPCR, vfp_get_fpcr(env));
    assert_hvf_ok(ret);

    ret = hv_vcpu_set_reg(cpu->accel->fd, HV_REG_FPSR, vfp_get_fpsr(env));
    assert_hvf_ok(ret);

    ret = hv_vcpu_set_reg(cpu->accel->fd, HV_REG_CPSR, pstate_read(env));
    assert_hvf_ok(ret);

    hvf_save_sp(env);

    assert(write_cpustate_to_list(arm_cpu, false));
    for (i = 0, n = arm_cpu->cpreg_array_len; i < n; i++) {
        uint64_t kvm_id = arm_cpu->cpreg_indexes[i];
        int hvf_id = KVMID_TO_HVF(kvm_id);

        if (cpu->accel->guest_debug_enabled) {
            /* Handle debug registers */
            switch (hvf_id) {
            case HV_SYS_REG_DBGBVR0_EL1:
            case HV_SYS_REG_DBGBCR0_EL1:
            case HV_SYS_REG_DBGWVR0_EL1:
            case HV_SYS_REG_DBGWCR0_EL1:
            case HV_SYS_REG_DBGBVR1_EL1:
            case HV_SYS_REG_DBGBCR1_EL1:
            case HV_SYS_REG_DBGWVR1_EL1:
            case HV_SYS_REG_DBGWCR1_EL1:
            case HV_SYS_REG_DBGBVR2_EL1:
            case HV_SYS_REG_DBGBCR2_EL1:
            case HV_SYS_REG_DBGWVR2_EL1:
            case HV_SYS_REG_DBGWCR2_EL1:
            case HV_SYS_REG_DBGBVR3_EL1:
            case HV_SYS_REG_DBGBCR3_EL1:
            case HV_SYS_REG_DBGWVR3_EL1:
            case HV_SYS_REG_DBGWCR3_EL1:
            case HV_SYS_REG_DBGBVR4_EL1:
            case HV_SYS_REG_DBGBCR4_EL1:
            case HV_SYS_REG_DBGWVR4_EL1:
            case HV_SYS_REG_DBGWCR4_EL1:
            case HV_SYS_REG_DBGBVR5_EL1:
            case HV_SYS_REG_DBGBCR5_EL1:
            case HV_SYS_REG_DBGWVR5_EL1:
            case HV_SYS_REG_DBGWCR5_EL1:
            case HV_SYS_REG_DBGBVR6_EL1:
            case HV_SYS_REG_DBGBCR6_EL1:
            case HV_SYS_REG_DBGWVR6_EL1:
            case HV_SYS_REG_DBGWCR6_EL1:
            case HV_SYS_REG_DBGBVR7_EL1:
            case HV_SYS_REG_DBGBCR7_EL1:
            case HV_SYS_REG_DBGWVR7_EL1:
            case HV_SYS_REG_DBGWCR7_EL1:
            case HV_SYS_REG_DBGBVR8_EL1:
            case HV_SYS_REG_DBGBCR8_EL1:
            case HV_SYS_REG_DBGWVR8_EL1:
            case HV_SYS_REG_DBGWCR8_EL1:
            case HV_SYS_REG_DBGBVR9_EL1:
            case HV_SYS_REG_DBGBCR9_EL1:
            case HV_SYS_REG_DBGWVR9_EL1:
            case HV_SYS_REG_DBGWCR9_EL1:
            case HV_SYS_REG_DBGBVR10_EL1:
            case HV_SYS_REG_DBGBCR10_EL1:
            case HV_SYS_REG_DBGWVR10_EL1:
            case HV_SYS_REG_DBGWCR10_EL1:
            case HV_SYS_REG_DBGBVR11_EL1:
            case HV_SYS_REG_DBGBCR11_EL1:
            case HV_SYS_REG_DBGWVR11_EL1:
            case HV_SYS_REG_DBGWCR11_EL1:
            case HV_SYS_REG_DBGBVR12_EL1:
            case HV_SYS_REG_DBGBCR12_EL1:
            case HV_SYS_REG_DBGWVR12_EL1:
            case HV_SYS_REG_DBGWCR12_EL1:
            case HV_SYS_REG_DBGBVR13_EL1:
            case HV_SYS_REG_DBGBCR13_EL1:
            case HV_SYS_REG_DBGWVR13_EL1:
            case HV_SYS_REG_DBGWCR13_EL1:
            case HV_SYS_REG_DBGBVR14_EL1:
            case HV_SYS_REG_DBGBCR14_EL1:
            case HV_SYS_REG_DBGWVR14_EL1:
            case HV_SYS_REG_DBGWCR14_EL1:
            case HV_SYS_REG_DBGBVR15_EL1:
            case HV_SYS_REG_DBGBCR15_EL1:
            case HV_SYS_REG_DBGWVR15_EL1:
            case HV_SYS_REG_DBGWCR15_EL1:
                /*
                 * If the guest is being debugged, the vCPU's debug registers
                 * are already holding the gdbstub's view of the registers (set
                 * in hvf_arch_update_guest_debug()).
                 */
                continue;
            }
        }

        val = arm_cpu->cpreg_values[i];
        ret = hv_vcpu_set_sys_reg(cpu->accel->fd, hvf_id, val);
        assert_hvf_ok(ret);
    }

    ret = hv_vcpu_set_vtimer_offset(cpu->accel->fd, hvf_state->vtimer_offset);
    assert_hvf_ok(ret);

    return 0;
}

/* Must be called by the owning thread */
static void flush_cpu_state(CPUState *cpu)
{
    if (cpu->vcpu_dirty) {
        hvf_arch_put_registers(cpu);
        cpu->vcpu_dirty = false;
    }
}

/* Must be called by the owning thread */
static void hvf_set_reg(CPUState *cpu, int rt, uint64_t val)
{
    hv_return_t r;

    flush_cpu_state(cpu);

    if (rt < 31) {
        r = hv_vcpu_set_reg(cpu->accel->fd, HV_REG_X0 + rt, val);
        assert_hvf_ok(r);
    }
}

/* Must be called by the owning thread */
static uint64_t hvf_get_reg(CPUState *cpu, int rt)
{
    uint64_t val = 0;
    hv_return_t r;

    flush_cpu_state(cpu);

    if (rt < 31) {
        r = hv_vcpu_get_reg(cpu->accel->fd, HV_REG_X0 + rt, &val);
        assert_hvf_ok(r);
    }

    return val;
}

static void clamp_id_aa64mmfr0_parange_to_ipa_size(ARMISARegisters *isar)
{
    uint32_t ipa_size = chosen_ipa_bit_size ?
            chosen_ipa_bit_size : hvf_arm_get_max_ipa_bit_size();
    uint64_t id_aa64mmfr0;

    /* Clamp down the PARange to the IPA size the kernel supports. */
    uint8_t index = round_down_to_parange_index(ipa_size);
    id_aa64mmfr0 = GET_IDREG(isar, ID_AA64MMFR0);
    id_aa64mmfr0 = (id_aa64mmfr0 & ~R_ID_AA64MMFR0_PARANGE_MASK) | index;
    SET_IDREG(isar, ID_AA64MMFR0, id_aa64mmfr0);
}

static bool hvf_arm_get_host_cpu_features(ARMHostCPUFeatures *ahcf)
{
    ARMISARegisters host_isar = {};
    static const struct isar_regs {
        hv_feature_reg_t reg;
        ARMIDRegisterIdx index;
    } regs[] = {
        { HV_FEATURE_REG_ID_AA64PFR0_EL1, ID_AA64PFR0_EL1_IDX },
        { HV_FEATURE_REG_ID_AA64PFR1_EL1, ID_AA64PFR1_EL1_IDX },
        /* Add ID_AA64PFR2_EL1 here when HVF supports it */
        { HV_FEATURE_REG_ID_AA64DFR0_EL1, ID_AA64DFR0_EL1_IDX },
        { HV_FEATURE_REG_ID_AA64DFR1_EL1, ID_AA64DFR1_EL1_IDX },
        { HV_FEATURE_REG_ID_AA64ISAR0_EL1, ID_AA64ISAR0_EL1_IDX },
        { HV_FEATURE_REG_ID_AA64ISAR1_EL1, ID_AA64ISAR1_EL1_IDX },
        /* Add ID_AA64ISAR2_EL1 here when HVF supports it */
        { HV_FEATURE_REG_ID_AA64MMFR0_EL1, ID_AA64MMFR0_EL1_IDX },
        { HV_FEATURE_REG_ID_AA64MMFR1_EL1, ID_AA64MMFR1_EL1_IDX },
        { HV_FEATURE_REG_ID_AA64MMFR2_EL1, ID_AA64MMFR2_EL1_IDX },
        /* Add ID_AA64MMFR3_EL1 here when HVF supports it */
    };
    hv_return_t r = HV_SUCCESS;
    hv_vcpu_config_t config = hv_vcpu_config_create();
    uint64_t t;
    int i;

    ahcf->dtb_compatible = "arm,armv8";
    ahcf->features = (1ULL << ARM_FEATURE_V8) |
                     (1ULL << ARM_FEATURE_NEON) |
                     (1ULL << ARM_FEATURE_AARCH64) |
                     (1ULL << ARM_FEATURE_PMU) |
                     (1ULL << ARM_FEATURE_GENERIC_TIMER);

    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        r |= hv_vcpu_config_get_feature_reg(config, regs[i].reg,
                                            &host_isar.idregs[regs[i].index]);
    }
    os_release(config);

    /*
     * Hardcode MIDR because Apple deliberately doesn't expose a divergent
     * MIDR across systems.
     */
    t = REG_FIELD_DP64(0, MIDR_EL1, IMPLEMENTER, 0x61); /* Apple */
    t = REG_FIELD_DP64(t, MIDR_EL1, ARCHITECTURE, 0xf); /* v7 or later */
    t = REG_FIELD_DP64(t, MIDR_EL1, PARTNUM, 0);
    t = REG_FIELD_DP64(t, MIDR_EL1, VARIANT, 0);
    t = REG_FIELD_DP64(t, MIDR_EL1, REVISION, 0);
    ahcf->midr = t;

    clamp_id_aa64mmfr0_parange_to_ipa_size(&host_isar);

    /*
     * Disable SME, which is not properly handled by QEMU hvf yet.
     * To allow this through we would need to:
     * - make sure that the SME state is correctly handled in the
     *   get_registers/put_registers functions
     * - get the SME-specific CPU properties to work with accelerators
     *   other than TCG
     * - fix any assumptions we made that SME implies SVE (since
     *   on the M4 there is SME but not SVE)
     */
    SET_IDREG(&host_isar, ID_AA64PFR1,
              GET_IDREG(&host_isar, ID_AA64PFR1) & ~R_ID_AA64PFR1_SME_MASK);

    ahcf->isar = host_isar;

    /*
     * A scratch vCPU returns SCTLR 0, so let's fill our default with the M1
     * boot SCTLR from https://github.com/AsahiLinux/m1n1/issues/97
     */
    ahcf->reset_sctlr = 0x30100180;
    /*
     * SPAN is disabled by default when SCTLR.SPAN=1. To improve compatibility,
     * let's disable it on boot and then allow guest software to turn it on by
     * setting it to 0.
     */
    ahcf->reset_sctlr |= 0x00800000;

    /* Make sure we don't advertise AArch32 support for EL0/EL1 */
    if ((GET_IDREG(&host_isar, ID_AA64PFR0) & 0xff) != 0x11) {
        return false;
    }

    return r == HV_SUCCESS;
}

uint32_t hvf_arm_get_default_ipa_bit_size(void)
{
    uint32_t default_ipa_size;
    hv_return_t ret = hv_vm_config_get_default_ipa_size(&default_ipa_size);
    assert_hvf_ok(ret);

    return default_ipa_size;
}

uint32_t hvf_arm_get_max_ipa_bit_size(void)
{
    uint32_t max_ipa_size;
    hv_return_t ret = hv_vm_config_get_max_ipa_size(&max_ipa_size);
    assert_hvf_ok(ret);

    /*
     * We clamp any IPA size we want to back the VM with to a valid PARange
     * value so the guest doesn't try and map memory outside of the valid range.
     * This logic just clamps the passed in IPA bit size to the first valid
     * PARange value <= to it.
     */
    return round_down_to_parange_bit_size(max_ipa_size);
}

void hvf_arm_set_cpu_features_from_host(ARMCPU *cpu)
{
    if (!arm_host_cpu_features.dtb_compatible) {
        if (!hvf_enabled() ||
            !hvf_arm_get_host_cpu_features(&arm_host_cpu_features)) {
            /*
             * We can't report this error yet, so flag that we need to
             * in arm_cpu_realizefn().
             */
            cpu->host_cpu_probe_failed = true;
            return;
        }
    }

    cpu->dtb_compatible = arm_host_cpu_features.dtb_compatible;
    cpu->isar = arm_host_cpu_features.isar;
    cpu->env.features = arm_host_cpu_features.features;
    cpu->midr = arm_host_cpu_features.midr;
    cpu->reset_sctlr = arm_host_cpu_features.reset_sctlr;
}

void hvf_arch_vcpu_destroy(CPUState *cpu)
{
    hv_return_t ret;

    ret = hv_vcpu_destroy(cpu->accel->fd);
    assert_hvf_ok(ret);
}

hv_return_t hvf_arch_vm_create(MachineState *ms, uint32_t pa_range)
{
    hv_return_t ret;
    hv_vm_config_t config = hv_vm_config_create();

    ret = hv_vm_config_set_ipa_size(config, pa_range);
    if (ret != HV_SUCCESS) {
        goto cleanup;
    }
    chosen_ipa_bit_size = pa_range;

    ret = hv_vm_create(config);

cleanup:
    os_release(config);

    return ret;
}

int hvf_arch_init_vcpu(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    uint32_t sregs_match_len = ARRAY_SIZE(hvf_sreg_list);
    uint32_t sregs_cnt = 0;
    uint64_t pfr;
    hv_return_t ret;
    int i;

    env->aarch64 = true;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(arm_cpu->gt_cntfrq_hz));

    /* Allocate enough space for our sysreg sync */
    arm_cpu->cpreg_indexes = g_renew(uint64_t, arm_cpu->cpreg_indexes,
                                     sregs_match_len);
    arm_cpu->cpreg_values = g_renew(uint64_t, arm_cpu->cpreg_values,
                                    sregs_match_len);
    arm_cpu->cpreg_vmstate_indexes = g_renew(uint64_t,
                                             arm_cpu->cpreg_vmstate_indexes,
                                             sregs_match_len);
    arm_cpu->cpreg_vmstate_values = g_renew(uint64_t,
                                            arm_cpu->cpreg_vmstate_values,
                                            sregs_match_len);

    memset(arm_cpu->cpreg_values, 0, sregs_match_len * sizeof(uint64_t));

    /* Populate cp list for all known sysregs */
    for (i = 0; i < sregs_match_len; i++) {
        hv_sys_reg_t hvf_id = hvf_sreg_list[i];
        uint64_t kvm_id = HVF_TO_KVMID(hvf_id);
        uint32_t key = kvm_to_cpreg_id(kvm_id);
        const ARMCPRegInfo *ri = ARMCPRegTable_cget(arm_cpu->cp_regs, key);

        if (ri) {
            assert(!(ri->type & ARM_CP_NO_RAW));
            arm_cpu->cpreg_indexes[sregs_cnt++] = kvm_id;
        }
    }
    arm_cpu->cpreg_array_len = sregs_cnt;
    arm_cpu->cpreg_vmstate_array_len = sregs_cnt;

    /* cpreg tuples must be in strictly ascending order */
    qsort(arm_cpu->cpreg_indexes, sregs_cnt, sizeof(uint64_t), compare_u64);

    assert(write_cpustate_to_list(arm_cpu, false));

    /* Set CP_NO_RAW system registers on init */
    ret = hv_vcpu_set_sys_reg(cpu->accel->fd, HV_SYS_REG_MIDR_EL1,
                              arm_cpu->midr);
    assert_hvf_ok(ret);

    ret = hv_vcpu_set_sys_reg(cpu->accel->fd, HV_SYS_REG_MPIDR_EL1,
                              arm_cpu->mp_affinity);
    assert_hvf_ok(ret);

    ret = hv_vcpu_get_sys_reg(cpu->accel->fd, HV_SYS_REG_ID_AA64PFR0_EL1, &pfr);
    assert_hvf_ok(ret);
    pfr |= env->gicv3state ? (1 << 24) : 0;
    ret = hv_vcpu_set_sys_reg(cpu->accel->fd, HV_SYS_REG_ID_AA64PFR0_EL1, pfr);
    assert_hvf_ok(ret);

    /* We're limited to underlying hardware caps, override internal versions */
    ret = hv_vcpu_get_sys_reg(cpu->accel->fd, HV_SYS_REG_ID_AA64MMFR0_EL1,
                              &arm_cpu->isar.idregs[ID_AA64MMFR0_EL1_IDX]);
    assert_hvf_ok(ret);

    clamp_id_aa64mmfr0_parange_to_ipa_size(&arm_cpu->isar);
    ret = hv_vcpu_set_sys_reg(cpu->accel->fd, HV_SYS_REG_ID_AA64MMFR0_EL1,
                              arm_cpu->isar.idregs[ID_AA64MMFR0_EL1_IDX]);
    assert_hvf_ok(ret);

    return 0;
}

void hvf_kick_vcpu_thread(CPUState *cpu)
{
    hv_return_t ret;
    trace_hvf_kick_vcpu_thread(cpu->cpu_index, cpu->stop);
    cpus_kick_thread(cpu);
    ret = hv_vcpus_exit(&cpu->accel->fd, 1);
    assert_hvf_ok(ret);
}

/*
 * SP_EL0/SP_EL1 <-> xregs[31] helpers. The generic aarch64_save_sp() and
 * aarch64_restore_sp() redirect SP_EL1 to gxf.sp_gl[] while guarded (TCG's
 * banking); under HVF the vCPU's SP_EL1 is always the live one, so use
 * plain variants everywhere in this file.
 */
static void hvf_save_sp(CPUARMState *env)
{
    if (env->pstate & PSTATE_SP) {
        env->sp_el[arm_current_el(env)] = env->xregs[31];
    } else {
        env->sp_el[0] = env->xregs[31];
    }
}

static void hvf_restore_sp(CPUARMState *env)
{
    if (env->pstate & PSTATE_SP) {
        env->xregs[31] = env->sp_el[arm_current_el(env)];
    } else {
        env->xregs[31] = env->sp_el[0];
    }
}

/*
 * Apple GXF (Guarded Execution) emulation.
 *
 * Hypervisor.framework does not virtualise GXF. The kernel patcher rewrites
 * GENTER/GEXIT into HVC #GXF_HVC_IMM_GENTER / #GXF_HVC_IMM_GEXIT, and we
 * emulate the GL1 register bank here. While the guest is in GL1, the GL1
 * bank (VBAR/TPIDR/SPSR/ELR/ESR/FAR) is made live in the real EL1 registers
 * so that hardware exception entry/return inside GL1 behaves like on real
 * silicon (vectors through VBAR_GL1, saves into SPSR/ELR/ESR/FAR_GL1). The
 * EL1 bank is stashed in cpu->accel->gxf_el1_saved meanwhile.
 *
 * All of these operate on the QEMU-side env; callers must have done
 * cpu_synchronize_state() and must leave cpu->vcpu_dirty set.
 */
static bool hvf_gxf_defer_irq(void);
static unsigned hvf_sprr_prot_guarded(unsigned nibble);
static unsigned hvf_sprr_prot_plain(unsigned nibble);
static unsigned hvf_sprr_permissive_class(CPUARMState *env, unsigned cls);

/*
 * Per-vCPU event ring, recorded only when INFERNO_HVF_HANG_WATCH is set and
 * dumped by the hang watchdog. A wedge leaves the vCPU spinning inside the
 * guest with no exit, so the only way to tell *which* exception got into
 * guarded mode is to keep a log of what we did just before.
 */
enum {
    HVF_EV_GENTER = 1, HVF_EV_GEXIT, HVF_EV_IRQ_ARM, HVF_EV_FIQ_ARM,
    HVF_EV_WITHDRAW, HVF_EV_VT_MASK, HVF_EV_VT_UNMASK, HVF_EV_VT_ACTIVE,
    HVF_EV_RAISE, HVF_EV_EXIT,
};

static const char *hvf_ev_names[] = {
    "-", "genter", "gexit", "irq-arm", "fiq-arm", "withdraw",
    "vt-mask", "vt-unmask", "vt-active", "raise", "exit",
};

static bool hvf_ev_on;

static void hvf_ev(CPUState *cpu, unsigned kind, uint64_t aux)
{
    AccelCPUState *acc = cpu->accel;
    unsigned i;

    if (!hvf_ev_on) {
        return;
    }
    /*
     * Run-length encode. Once a vCPU is wedged, QEMU is kicked and withdraws
     * the same interrupt at the same PC thousands of times, which otherwise
     * flushes the history that actually explains how it got there.
     */
    if (acc->ev_head) {
        i = (acc->ev_head - 1) % HVF_EV_RING;
        if (acc->ev[i].kind == kind && acc->ev[i].pc == cpu_env(cpu)->pc &&
            acc->ev[i].aux == aux) {
            acc->ev[i].rep++;
            return;
        }
    }
    i = acc->ev_head++ % HVF_EV_RING;
    acc->ev[i].kind = kind;
    acc->ev[i].pc = cpu_env(cpu)->pc;
    acc->ev[i].aux = aux;
    acc->ev[i].rep = 1;
}

static void hvf_ev_dump(CPUState *cpu)
{
    AccelCPUState *acc = cpu->accel;
    unsigned n = MIN(acc->ev_head, HVF_EV_RING);
    unsigned i;

    if (!hvf_ev_on || n == 0) {
        return;
    }
    fprintf(stderr, "  events (oldest first, %u of %" PRIu64 "):\n", n,
            acc->ev_head);
    for (i = 0; i < n; i++) {
        unsigned k = (acc->ev_head - n + i) % HVF_EV_RING;
        unsigned kind = acc->ev[k].kind;

        fprintf(stderr, "    %-9s x%-7u pc=%#018" PRIx64 " aux=%#" PRIx64
                "\n",
                kind < ARRAY_SIZE(hvf_ev_names) ? hvf_ev_names[kind] : "?",
                acc->ev[k].rep, acc->ev[k].pc, acc->ev[k].aux);
    }
}

#define HVF_SPRR_RELAX_INTERVAL 50000

static bool hvf_sprr_relax_all_enabled(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("INFERNO_HVF_SPRR_RELAX_ALL");
        on = e != NULL && atoi(e) != 0;
    }
    return on;
}

/*
 * Walk the guest's TTBR1 stage-1 tables and return the address of the leaf
 * descriptor for `va`, or 0 if the walk does not reach one.
 */
static uint64_t hvf_s1_leaf_addr(CPUARMState *env, uint64_t va, uint64_t *out)
{
    uint64_t tcr = env->cp15.tcr_el[1];
    unsigned tg1 = extract64(tcr, 30, 2);
    unsigned t1sz = extract64(tcr, 16, 6);
    unsigned page_bits, stride, va_bits, bits;
    uint64_t pa, desc = 0, entry = 0;
    int level;

    switch (tg1) {
    case 1: page_bits = 14; break;
    case 2: page_bits = 12; break;
    case 3: page_bits = 16; break;
    default: return 0;
    }
    stride = page_bits - 3;
    va_bits = 64 - t1sz;
    level = 3;
    bits = page_bits;
    while (bits + stride < va_bits) {
        bits += stride;
        level--;
    }

    pa = env->cp15.ttbr1_el[1] & MAKE_64BIT_MASK(1, 47);
    for (; level <= 3; level++) {
        unsigned shift = page_bits + stride * (3 - level);
        unsigned idx_bits = (level == 3) ? stride :
                            MIN(stride, va_bits - shift);

        entry = pa + extract64(va, shift, idx_bits) * 8;
        desc = address_space_ldq(&address_space_memory, entry,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
        if (!(desc & 1)) {
            return 0;
        }
        if (level < 3 && (desc & 3) == 1) {
            break;              /* block */
        }
        if (level < 3) {
            pa = desc & MAKE_64BIT_MASK(page_bits, 48 - page_bits);
        }
    }
    *out = desc;
    return entry;
}

/*
 * Make the GL1 vector page executable outside guarded mode.
 *
 * XNU maps VBAR_GL1's page with AP=0, PXN=0, UXN=0. On Apple silicon that is
 * not read as plain ARM permissions: it is SPRR class 0, and the guest's
 * SPRR_PERM_EL1 gives that class "RX while guarded, R otherwise". The host CPU
 * is never actually in GL1 -- GXF is emulated in software here -- so the page
 * decodes as read-only and *any* exception taken while guarded faults trying
 * to fetch its own vector, then vectors to the same page and faults again. The
 * vCPU spins on that at 100% with no exit to the hypervisor and the boot stops
 * dead. It cost roughly a third of all boots.
 *
 * Re-tag the page as class 0xA (AP=2, UXN=1, PXN=0), which is what ordinary
 * kernel text uses and which SPRR maps to RX in both modes. The page is never
 * written, so nothing is lost. Done once, when the guest installs VBAR_GL1,
 * which is long before anything fetches from it, so no stale TLB entry can
 * shadow the change.
 *
 * Only this one page. Re-tagging further guarded-only pages was measured and
 * is worse, not better: doing the guarded stack too went 15/16 -> 12/17, and
 * relaxing every class 1 (PPL data) mapping went to 0/12, mostly fast kernel
 * panics -- PPL validates its own mappings. Going further means the guarded
 * exception is actually delivered, and then XNU's stack-bounds check in the
 * normal EL1 vector spins on `b.lt .` instead, because the GEXIT hand-off
 * leaves SP_EL1 pointing at the guarded stack. That needs the GXF emulation
 * to model exception entry/return in GL1 properly, which is a bigger job.
 * INFERNO_HVF_SPRR_RELAX_ALL=1 enables the whole-table pass for experiments.
 */
/*
 * Relax every "guarded-only" SPRR class in the kernel's TTBR1 mappings.
 *
 * Apple silicon does not read a PTE's AP/PXN/UXN bits directly: they form a
 * 4-bit index into SPRR_PERM_EL1, and each entry encodes *two* permissions --
 * one for guarded (GL1) execution and one for everything else. XNU gives PPL
 * its isolation that way: class 0 is "RX guarded, R otherwise" (the GL1
 * vectors and PPL text) and class 1 is "RW guarded, R otherwise" (PPL stacks
 * and data).
 *
 * We emulate GXF in software, so the host CPU is never actually in GL1 and
 * always decodes the "otherwise" half. Everything PPL does while QEMU thinks
 * it is guarded therefore faults, and because the fault vectors through
 * VBAR_GL1 -- itself a class 0 page -- the abort re-faults on its own vector
 * and the vCPU spins there at 100% with no exit to the hypervisor. That is
 * what stopped roughly a third of boots dead.
 *
 * Re-tag those two classes to equivalents that grant the same access in both
 * halves: class 0 -> 0xA ("RX" both, what ordinary kernel text uses) and
 * class 1 -> 3 ("RW" both). This gives up PPL's hardware isolation, which
 * buys nothing here -- the kernel patcher already disables hardware TPRO and
 * pmap_cs for the same reason.
 */
/* Bitmap of classes whose non-guarded half no longer covers their guarded one. */
static unsigned hvf_sprr_guarded_only_classes(uint64_t perm)
{
    unsigned mask = 0;
    unsigned i;

    for (i = 0; i < 16; i++) {
        unsigned nibble = (perm >> (4 * i)) & 0xf;
        unsigned want = hvf_sprr_prot_guarded(nibble);

        if ((hvf_sprr_prot_plain(nibble) & want) != want) {
            mask |= 1u << i;
        }
    }
    return mask;
}

static unsigned hvf_sprr_relax_mask;

static bool hvf_sprr_relax_leaf(uint64_t *desc)
{
    unsigned ap = extract64(*desc, 6, 2);
    unsigned pxn = extract64(*desc, 53, 1);
    unsigned uxn = extract64(*desc, 54, 1);
    unsigned idx = (ap << 2) | (uxn << 1) | pxn;
    unsigned repl;

    /*
     * Re-tag only the classes that the kernel's SPRR_PERM_EL1 write just made
     * guarded-only, to the equivalent that grants the same outside GL1.
     */
    if (!(hvf_sprr_relax_mask & (1u << idx))) {
        return false;
    }
    repl = hvf_sprr_permissive_class(&ARM_CPU(first_cpu)->env, idx);
    if (repl == idx) {
        return false;
    }
    *desc = deposit64(*desc, 6, 2, (repl >> 2) & 3);
    *desc = deposit64(*desc, 53, 1, repl & 1);
    *desc = deposit64(*desc, 54, 1, (repl >> 1) & 1);
    return true;
}

static void hvf_sprr_relax_all(CPUState *cpu)
{
    CPUARMState *env = &ARM_CPU(cpu)->env;
    uint64_t tcr = env->cp15.tcr_el[1];
    unsigned tg1 = extract64(tcr, 30, 2);
    unsigned t1sz = extract64(tcr, 16, 6);
    unsigned page_bits, stride, va_bits, bits, top_bits;
    uint64_t l1, changed = 0, leaves = 0;
    int level;
    unsigned i, j, k;

    switch (tg1) {
    case 1: page_bits = 14; break;
    case 2: page_bits = 12; break;
    case 3: page_bits = 16; break;
    default: return;
    }
    stride = page_bits - 3;
    va_bits = 64 - t1sz;
    level = 3;
    bits = page_bits;
    while (bits + stride < va_bits) {
        bits += stride;
        level--;
    }
    if (level != 1) {
        /* Only the 3-level layout the t8030 guest actually uses. */
        return;
    }
    top_bits = va_bits - bits;
    l1 = env->cp15.ttbr1_el[1] & MAKE_64BIT_MASK(1, 47);

    for (i = 0; i < (1u << top_bits); i++) {
        uint64_t d1 = address_space_ldq(&address_space_memory, l1 + i * 8,
                                        MEMTXATTRS_UNSPECIFIED, NULL);
        uint64_t l2;

        if ((d1 & 3) != 3) {
            continue;
        }
        l2 = d1 & MAKE_64BIT_MASK(page_bits, 48 - page_bits);
        for (j = 0; j < (1u << stride); j++) {
            uint64_t d2 = address_space_ldq(&address_space_memory, l2 + j * 8,
                                            MEMTXATTRS_UNSPECIFIED, NULL);
            uint64_t l3;

            if ((d2 & 3) != 3) {
                continue;
            }
            l3 = d2 & MAKE_64BIT_MASK(page_bits, 48 - page_bits);
            for (k = 0; k < (1u << stride); k++) {
                uint64_t addr = l3 + k * 8;
                uint64_t d3 = address_space_ldq(&address_space_memory, addr,
                                                MEMTXATTRS_UNSPECIFIED, NULL);

                if ((d3 & 3) != 3) {
                    continue;
                }
                leaves++;
                if (hvf_sprr_relax_leaf(&d3)) {
                    address_space_stq(&address_space_memory, addr, d3,
                                      MEMTXATTRS_UNSPECIFIED, NULL);
                    changed++;
                }
            }
        }
    }

    if (changed) {
        info_report("GXF/HVF: relaxed %" PRIu64 " of %" PRIu64
                    " guarded-only PPL data mappings", changed, leaves);
    }
}

/*
 * Re-tag one page so that the class it decodes to outside guarded mode grants
 * what it grants inside. Returns true if the descriptor was changed.
 *
 *   class 0 (AP=0,UXN=0,PXN=0): RX guarded, R otherwise  -> class 0xA, RX both
 *   class 1 (AP=0,UXN=0,PXN=1): RW guarded, R otherwise  -> class 3,   RW both
 *
 * Anything else already grants the same in both halves and is left alone --
 * which matters, because XNU's pmap_set_pte_xprr_perm() asserts a PTE's
 * current class before re-permissioning it and panics on a mismatch
 * ("perm=3 does not match expected_perm"). Only pages PPL never re-permissions
 * are safe to touch, which is why this is driven off the GL1 vector page's own
 * references rather than applied to the whole table.
 */
static bool hvf_gxf_retag_page(CPUState *cpu, uint64_t va, const char *what)
{
    CPUARMState *env = &ARM_CPU(cpu)->env;
    uint64_t desc = 0, entry;
    unsigned idx;

    if (va == 0) {
        return false;
    }
    entry = hvf_s1_leaf_addr(env, va, &desc);
    if (entry == 0) {
        return false;
    }
    idx = (extract64(desc, 6, 2) << 2) | (extract64(desc, 54, 1) << 1) |
          extract64(desc, 53, 1);
    if (idx == 0) {
        desc = deposit64(desc, 6, 2, 2);    /* AP  = 2 */
        desc = deposit64(desc, 54, 1, 1);   /* UXN = 1 */
    } else if (idx == 1) {
        desc = deposit64(desc, 54, 1, 1);   /* UXN = 1 */
    } else {
        return false;
    }
    address_space_stq(&address_space_memory, entry, desc,
                      MEMTXATTRS_UNSPECIFIED, NULL);
    info_report("GXF/HVF: re-tagged %s %#" PRIx64 " (class %u, pte@%#" PRIx64
                " -> %#" PRIx64 ")", what, va, idx, entry, desc);
    return true;
}

/*
 * Walk the GL1 vector page's instructions and re-tag every page its PC-relative
 * references reach. The handler reads a per-CPU array through an ADRP/ADD pair
 * before it has left guarded mode, and that array is class 1, so without this
 * the very first load after the exception faults just as the vector fetch did.
 */
static void hvf_gxf_fixup_vector_refs(CPUState *cpu, uint64_t vbar)
{
    uint64_t seen[8] = { 0 };
    unsigned n_seen = 0;
    unsigned i;

    for (i = 0; i < 0x4000 / 4; i++) {
        uint64_t pc = vbar + i * 4;
        uint32_t insn;
        uint64_t target, page;

        if (cpu_memory_rw_debug(cpu, pc, (uint8_t *)&insn, 4, 0)) {
            return;
        }
        insn = le32_to_cpu(insn);
        unsigned rd, j;
        bool dup = false;

        /* ADRP: op=1, 1 0000 -> bits 31 and 28..24 */
        if ((insn & 0x9f000000) != 0x90000000) {
            continue;
        }
        rd = insn & 0x1f;
        target = (pc & ~(uint64_t)0xfff) +
                 (sextract64(((insn >> 5) & 0x7ffff) << 2 |
                             ((insn >> 29) & 3), 0, 21) << 12);

        /* Fold in a following "ADD Xd, Xd, #imm" so the datum is covered. */
        if (i + 1 < 0x4000 / 4) {
            uint32_t next = 0;

            if (cpu_memory_rw_debug(cpu, pc + 4, (uint8_t *)&next, 4, 0)) {
                next = 0;
            }
            next = le32_to_cpu(next);
            if ((next & 0xffc00000) == 0x91000000 &&
                (next & 0x1f) == rd && ((next >> 5) & 0x1f) == rd) {
                target += (next >> 10) & 0xfff;
            }
        }

        page = target & ~(uint64_t)0x3fff;
        for (j = 0; j < n_seen; j++) {
            if (seen[j] == page) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (n_seen < ARRAY_SIZE(seen)) {
            seen[n_seen++] = page;
        }
        hvf_gxf_retag_page(cpu, target, "GL1 vector reference");
    }
}

/*
 * Force the guest's TLBs to be invalidated.
 *
 * Re-tagging a PTE is not enough on its own: the class bits are cached in the
 * TLB, and Apple reports an access that violates the cached class as a data
 * abort with an IMPDEF fault status (0x21, which reads as "alignment fault"
 * architecturally but is raised here for an access that is properly aligned on
 * Normal write-back memory with SCTLR_EL1.A clear). HVF exposes no TLBI, but
 * it must invalidate the combined stage-1/stage-2 entries for a region when
 * its stage-2 permissions change, so cycling them does the job.
 *
 * Must run with the other vCPUs stopped -- see hvf_sprr_relax_work().
 */
static void hvf_flush_guest_tlb(void)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(hvf_state->slots); i++) {
        hvf_slot *slot = &hvf_state->slots[i];

        if (slot->size == 0 || slot->mem == NULL) {
            continue;
        }
        hv_vm_protect(slot->start, slot->size,
                      HV_MEMORY_READ | HV_MEMORY_EXEC);
        hv_vm_protect(slot->start, slot->size,
                      HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    }
}

static void hvf_sprr_relax_work(CPUState *cpu, run_on_cpu_data data)
{
    hvf_sprr_relax_mask = data.host_int;
    hvf_sprr_relax_all(cpu);
    hvf_flush_guest_tlb();
}

static void hvf_gxf_fixup_vectors(CPUState *cpu, uint64_t vbar)
{
    if (!hvf_gxf_defer_irq() || vbar == 0 ||
        vbar == cpu->accel->gxf_vbar_fixed) {
        return;
    }
    cpu->accel->gxf_vbar_fixed = vbar;
    hvf_gxf_retag_page(cpu, vbar, "VBAR_GL1 page");
    /*
     * Re-tagging the pages the vector code references (the per-CPU PPL array)
     * as well was measured at 15/19, against 36/40 for the vector page alone,
     * and the failures relocate rather than disappear -- so the scan is kept
     * behind a switch rather than run by default.
     */
    if (getenv("INFERNO_HVF_GXF_RETAG_REFS")) {
        hvf_gxf_fixup_vector_refs(cpu, vbar);
    }
}

/*
 * Apple XPRR/SPRR permission classes.
 *
 * A PTE's AP[7:6] and PXN/UXN[53:54] form a 4-bit index into SPRR_PERM_EL1,
 * and each 4-bit entry holds *two* permissions: bits [3:2] apply while the CPU
 * is in guarded state (GL1), bits [1:0] the rest of the time. XNU isolates PPL
 * with classes whose two halves differ -- PPL text reads "RX guarded, R
 * otherwise", PPL data "RW guarded, nothing otherwise".
 *
 * We emulate GXF in software, so the host CPU never actually enters GL1 and
 * always decodes the second half. Everything PPL touches while QEMU believes
 * it is guarded therefore faults. The decode below mirrors
 * pte_to_sprr_prot_is_guarded() in target/arm/ptw.c, which is what TCG uses --
 * and is why none of this is needed there.
 */
#define SPRR_P_R 1
#define SPRR_P_W 2
#define SPRR_P_X 4

static unsigned hvf_sprr_prot_guarded(unsigned nibble)
{
    switch (nibble >> 2) {
    case 0:  return 0;
    case 1:  return SPRR_P_R | SPRR_P_X;
    case 2:  return SPRR_P_R;
    default: return SPRR_P_R | SPRR_P_W;
    }
}

static unsigned hvf_sprr_prot_plain(unsigned nibble)
{
    unsigned guarded = nibble >> 2;

    switch (nibble & 3) {
    case 0:  return 0;
    case 1:  return guarded == 2 ? SPRR_P_X : (SPRR_P_R | SPRR_P_X);
    case 2:  return SPRR_P_R;
    default: return guarded == 1 ? 0 : (SPRR_P_R | SPRR_P_W);
    }
}

/*
 * Given the class the kernel wants to install, return one that grants at least
 * as much *outside* guarded mode as the requested one grants inside it.
 *
 * Derived from the guest's own SPRR_PERM_EL1 rather than hardcoded, so it is
 * not tied to one kernel's class numbering. Classes whose two halves are equal
 * are preferred: those are what the kernel uses for ordinary text and data, so
 * they are known to decode sanely under whatever permission table the host has
 * programmed -- which is not necessarily the guest's, since HVF exposes no way
 * to write the Apple IMPDEF SPRR registers.
 */
static unsigned hvf_sprr_permissive_class(CPUARMState *env, unsigned cls)
{
    uint64_t perm = env->sprr.sprr_el_br_el1[1][1];
    unsigned nibble = (perm >> (4 * (cls & 0xf))) & 0xf;
    unsigned want = hvf_sprr_prot_guarded(nibble) | hvf_sprr_prot_plain(nibble);
    unsigned i;

    if ((hvf_sprr_prot_plain(nibble) & want) == want) {
        return cls;             /* already permissive enough */
    }
    for (i = 0; i < 16; i++) {
        unsigned c = (perm >> (4 * i)) & 0xf;

        if ((c >> 2) == (c & 3) &&
            (hvf_sprr_prot_plain(c) & want) == want) {
            return i;
        }
    }
    return cls;                 /* nothing better available */
}

/*
 * Emulate pmap_set_pte_xprr_perm(ptep, expected_perm, new_perm).
 *
 * The kernel patcher replaced the function's first instruction with an HVC, so
 * the whole thing happens here: we write the PTE ourselves, substituting a
 * class that is usable outside guarded mode. QEMU's writes go through the
 * software page-table walk and bypass SPRR entirely, which is the point -- the
 * guest cannot make this change itself, because the moment a page carries a
 * PPL-only class the guest can no longer touch it.
 *
 * The function's own assertion that the PTE currently holds `expected_perm` is
 * dropped: once a class has been substituted it will not match what the kernel
 * remembers, and the panic it raises ("perm=%llu does not match
 * expected_perm") is exactly what made a blanket page-table rewrite unusable.
 */
static void hvf_xprr_set_pte(CPUState *cpu)
{
    CPUARMState *env = &ARM_CPU(cpu)->env;
    uint64_t ptep = env->xregs[0];
    unsigned want = env->xregs[2] & 0xf;
    uint64_t pte = 0;
    unsigned repl;

    if (cpu_memory_rw_debug(cpu, ptep, (uint8_t *)&pte, sizeof(pte), 0) == 0) {
        pte = le64_to_cpu(pte);
        repl = hvf_sprr_permissive_class(env, want);
        if (getenv("INFERNO_HVF_XPRR_TRACE")) {
            static uint64_t calls, remapped;
            static uint64_t last_perm = ~0ULL;
            uint64_t perm = env->sprr.sprr_el_br_el1[1][1];

            static uint32_t seen_want;

            calls++;
            remapped += (repl != want);
            if (!(seen_want & (1u << want))) {
                seen_want |= 1u << want;
                fprintf(stderr, "xprr: first request for class %u -> %u "
                        "(sprr=%#" PRIx64 ")\n", want, repl, perm);
            }
            if (perm != last_perm || calls % 5000 == 0) {
                last_perm = perm;
                fprintf(stderr, "xprr: calls=%" PRIu64 " remapped=%" PRIu64
                        " sprr=%#" PRIx64 " want=%u -> %u\n",
                        calls, remapped, perm, want, repl);
            }
        }
        pte &= 0xFF9FFFFFFFFFFF3FULL;
        pte |= ((uint64_t)(repl & 0xc) << 4) | ((uint64_t)(repl & 3) << 53);
        pte = cpu_to_le64(pte);
        cpu_memory_rw_debug(cpu, ptep, (uint8_t *)&pte, sizeof(pte), 1);
    }

    /*
     * Return to the caller. The replaced instruction was the PACIBSP at the
     * top of the function, so the matching RETAB never runs either and x30
     * still holds the unsigned return address.
     */
    env->pc = env->xregs[30];
}

static bool hvf_gxf_is_guarded(CPUARMState *env)
{
    return arm_feature(env, ARM_FEATURE_GXF) &&
           (env->gxf.gxf_status_el[1] & 1);
}

/* Copy the live EL1 registers (== GL1 bank while guarded) into gxf.*_gl. */
static void hvf_gxf_sync_live_to_gl(CPUARMState *env)
{
    env->gxf.vbar_gl[1] = env->cp15.vbar_el[1];
    env->gxf.tpidr_gl[1] = env->cp15.tpidr_el[1];
    env->gxf.spsr_gl[1] = env->banked_spsr[BANK_SVC];
    env->gxf.elr_gl[1] = env->elr_el[1];
    env->gxf.esr_gl[1] = env->cp15.esr_el[1];
    env->gxf.far_gl[1] = env->cp15.far_el[1];
    env->gxf.sp_gl[1] = env->sp_el[1];
}

/* Make gxf.*_gl live in the real EL1 registers. */
static void hvf_gxf_sync_gl_to_live(CPUARMState *env)
{
    env->cp15.vbar_el[1] = env->gxf.vbar_gl[1];
    env->cp15.tpidr_el[1] = env->gxf.tpidr_gl[1];
    env->banked_spsr[BANK_SVC] = env->gxf.spsr_gl[1];
    env->elr_el[1] = env->gxf.elr_gl[1];
    env->cp15.esr_el[1] = env->gxf.esr_gl[1];
    env->cp15.far_el[1] = env->gxf.far_gl[1];
    env->sp_el[1] = env->gxf.sp_gl[1];
}

static bool hvf_trace_exceptions(void)
{
    static int on = -1;

    if (on < 0) {
        on = getenv("INFERNO_HVF_TRACE_EXC") != NULL;
    }
    return on;
}

static void hvf_raise_exception(CPUState *cpu, uint32_t excp,
                                uint32_t syndrome, int target_el)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    bool guarded = hvf_gxf_is_guarded(env);

    cpu->exception_index = excp;
    env->exception.target_el = target_el;
    env->exception.syndrome = syndrome;

    /*
     * Every synthetic exception QEMU injects goes through here. When one is
     * delivered while the guest is in guarded mode and already on SP_EL1, XNU
     * vectors it to the "Current EL with SPx" entry of VBAR_GL1, which is an
     * unconditional `B .` -- the vCPU then spins at 100% forever and the boot
     * wedges with no further serial output. INFERNO_HVF_TRACE_EXC prints the
     * cause so that hang can be attributed.
     */
    hvf_ev(cpu, HVF_EV_RAISE, ((uint64_t)excp << 32) | syndrome);
    if (hvf_trace_exceptions()) {
        fprintf(stderr, "hvf-exc: cpu%d excp=%u syn=%#010x target_el=%d "
                "pc=%#018" PRIx64 " guarded=%d spsel=%d\n",
                cpu->cpu_index, excp, syndrome, target_el, env->pc,
                (int)guarded, (int)!!(env->pstate & PSTATE_SP));
    }

    /*
     * arm_cpu_do_interrupt() uses the gxf.*_gl shadow bank when guarded;
     * keep it coherent with the live registers around the call.
     */
    if (guarded) {
        hvf_gxf_sync_live_to_gl(env);
    }
    arm_cpu_do_interrupt(cpu);
    if (guarded) {
        hvf_gxf_sync_gl_to_live(env);
    }
}

static void hvf_gxf_enter(CPUState *cpu, uint32_t imm)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    AccelCPUState *acc = cpu->accel;
    uint32_t old_mode;
    uint32_t new_mode;

    if (!arm_feature(env, ARM_FEATURE_GXF) ||
        !(env->gxf.gxf_config_el[1] & 1) || arm_current_el(env) != 1) {
        trace_hvf_unknown_hvc(env->pc, env->xregs[0]);
        hvf_raise_exception(cpu, EXCP_UDEF, syn_uncategorized(), 1);
        return;
    }

    if (hvf_gxf_is_guarded(env)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: GENTER while already guarded\n",
                      __func__);
        hvf_raise_exception(cpu, EXCP_UDEF, syn_uncategorized(), 1);
        return;
    }

    old_mode = pstate_read(env);
    hvf_save_sp(env);

    /* Stash the EL1 bank (SP_EL1 included: GL1 has its own SP1). */
    acc->gxf_el1_saved.vbar = env->cp15.vbar_el[1];
    acc->gxf_el1_saved.tpidr = env->cp15.tpidr_el[1];
    acc->gxf_el1_saved.spsr = env->banked_spsr[BANK_SVC];
    acc->gxf_el1_saved.elr = env->elr_el[1];
    acc->gxf_el1_saved.esr = env->cp15.esr_el[1];
    acc->gxf_el1_saved.far = env->cp15.far_el[1];
    acc->gxf_el1_saved.sp_el1 = env->sp_el[1];

    /* Build the GL1 bank. For HVC exits the PC already points past it. */
    env->gxf.spsr_gl[1] = old_mode;
    env->gxf.elr_gl[1] = env->pc;
    env->gxf.esr_gl[1] = syn_aa64_genter(imm);
    hvf_gxf_fixup_vectors(cpu, env->gxf.vbar_gl[1]);
    /*
     * PPL does not lock itself down by re-tagging pages -- it rewrites
     * SPRR_PERM_EL1 so that classes which used to grant the same in both
     * halves become guarded-only, retroactively taking away access to every
     * page already tagged with them. Watch for that write (it happens once,
     * and GENTER is the cheapest place to notice) and re-tag the affected
     * pages to the equivalents that still grant the same outside GL1.
     *
     * Safe to do here only because pmap_set_pte_xprr_perm() is now emulated by
     * hvf_xprr_set_pte(), which drops the assertion on a PTE's current class.
     * Doing this walk without that is what panicked the guest with
     * "perm=3 does not match expected_perm".
     */
    if (gxf_hvf_xprr_remap_enabled()) {
        uint64_t perm = env->sprr.sprr_el_br_el1[1][1];

        if (perm != cpu->accel->gxf_sprr_seen) {
            unsigned mask = hvf_sprr_guarded_only_classes(perm);

            cpu->accel->gxf_sprr_seen = perm;
            if (mask != 0) {
                /*
                 * Defer to a safe point: the walk rewrites page tables the
                 * other vCPUs are actively translating, and the TLB flush
                 * that has to follow it cannot race them either. GENTER may
                 * well be running on the lock-free path here.
                 */
                async_safe_run_on_cpu(cpu, hvf_sprr_relax_work,
                                      RUN_ON_CPU_HOST_INT(mask));
            }
        }
    }
    hvf_gxf_sync_gl_to_live(env);

    new_mode = PSTATE_MODE_EL1h;
    if (cpu_isar_feature(aa64_pan, arm_cpu)) {
        new_mode |= old_mode & PSTATE_PAN;
        if ((env->cp15.sctlr_el[1] & SCTLR_SPAN) == 0) {
            new_mode |= PSTATE_PAN;
        }
    }
    if (cpu_isar_feature(aa64_ssbs, arm_cpu) &&
        (env->cp15.sctlr_el[1] & SCTLR_DSSBS_64)) {
        new_mode |= PSTATE_SSBS;
    }
    pstate_write(env, PSTATE_DAIF | new_mode);
    env->gxf.gxf_status_el[1] |= 1;

    /*
     * Withdraw any interrupt HVF already has armed for this vCPU.
     *
     * hv_vcpu_set_pending_interrupt() is sticky, and GENTER is handled inside
     * the lock-free loop in hvf_arch_vcpu_exec() which never revisits
     * hvf_inject_interrupts(). Without this, an interrupt armed before the
     * guest entered GL1 stays armed, and XNU's ppl_dispatch unmasks DAIF
     * around interruptible PPL calls -- the interrupt is then delivered in
     * guarded mode and vectors through VBAR_GL1, whose page SPRR makes
     * executable only in GL1. The host CPU is never really in GL1 (GXF is
     * emulated), so the fetch takes a permission fault that vectors to itself
     * forever. Re-arming happens at the top of hvf_arch_vcpu_exec() once the
     * guest GEXITs.
     */
    if (hvf_gxf_defer_irq()) {
        hv_vcpu_set_pending_interrupt(cpu->accel->fd, HV_INTERRUPT_TYPE_IRQ,
                                      false);
        hv_vcpu_set_pending_interrupt(cpu->accel->fd, HV_INTERRUPT_TYPE_FIQ,
                                      false);
        /*
         * The virtual timer is armed in hardware and does not go through
         * hv_vcpu_set_pending_interrupt() at all, so withdrawing the pending
         * lines alone still leaves one way for an interrupt to arrive in
         * guarded mode. Mask it for the duration; hvf_gxf_exit() restores it.
         */
        if (!acc->vtimer_masked && !acc->gxf_vtimer_masked) {
            hvf_ev(cpu, HVF_EV_VT_MASK, 0);
            hv_vcpu_set_vtimer_mask(acc->fd, true);
            acc->gxf_vtimer_masked = true;
        }
    }

    /*
     * The guarded stack is deliberately NOT re-tagged here. Re-tagging a page
     * the guest is already using is unsound: there is no way to invalidate the
     * guest's TLBs from the host under HVF, so the change is observed
     * inconsistently and the failures move somewhere worse (measured: 14/18
     * with it, against 36/40 without). The vector page is safe only because it
     * is re-tagged once, early, before anything has translated it.
     */
    hvf_ev(cpu, HVF_EV_GENTER, env->gxf.sp_gl[1]);
    hvf_restore_sp(env);
    env->pc = env->gxf.gxf_enter_el[1];

    trace_hvf_gxf_enter(env->gxf.elr_gl[1], env->pc);
}

static void hvf_gxf_exit(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    AccelCPUState *acc = cpu->accel;
    uint32_t spsr;
    uint64_t elr;

    if (!hvf_gxf_is_guarded(env) || arm_current_el(env) != 1) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: GEXIT while not guarded\n",
                      __func__);
        hvf_raise_exception(cpu, EXCP_UDEF, syn_uncategorized(), 1);
        return;
    }

    hvf_save_sp(env);

    /* Save the (live) GL1 bank, restore the EL1 bank. */
    hvf_gxf_sync_live_to_gl(env);
    spsr = env->gxf.spsr_gl[1];
    elr = env->gxf.elr_gl[1];

    env->cp15.vbar_el[1] = acc->gxf_el1_saved.vbar;
    env->cp15.tpidr_el[1] = acc->gxf_el1_saved.tpidr;
    env->banked_spsr[BANK_SVC] = acc->gxf_el1_saved.spsr;
    env->elr_el[1] = acc->gxf_el1_saved.elr;
    env->cp15.esr_el[1] = acc->gxf_el1_saved.esr;
    env->cp15.far_el[1] = acc->gxf_el1_saved.far;
    env->sp_el[1] = acc->gxf_el1_saved.sp_el1;

    spsr &= aarch64_pstate_valid_mask(&arm_cpu->isar);
    pstate_write(env, spsr);
    env->pstate &= ~PSTATE_SS;
    env->gxf.gxf_status_el[1] &= ~1;

    if (acc->gxf_vtimer_masked) {
        acc->gxf_vtimer_masked = false;
        if (!acc->vtimer_masked) {
            hv_vcpu_set_vtimer_mask(acc->fd, false);
        }
    }

    hvf_ev(cpu, HVF_EV_GEXIT, elr);
    hvf_restore_sp(env);
    env->pc = elr;

    trace_hvf_gxf_exit(env->pc);
}

/* GL1-banked system registers: S3_6_C15_C9_* and ASPSR_GL11 (S3_6_C15_C8_3). */
static bool hvf_gxf_sysreg_needs_sync(uint32_t reg)
{
    return SYSREG_OP0(reg) == 3 && SYSREG_OP1(reg) == 6 &&
           SYSREG_CRN(reg) == 15;
}

#define SYSREG_SP_GL11 SYSREG(3, 6, 15, 9, 0)
#define SYSREG_TPIDR_GL11 SYSREG(3, 6, 15, 9, 1)
#define SYSREG_VBAR_GL11 SYSREG(3, 6, 15, 9, 2)
#define SYSREG_SPSR_GL11 SYSREG(3, 6, 15, 9, 3)
#define SYSREG_ESR_GL11 SYSREG(3, 6, 15, 9, 5)
#define SYSREG_ELR_GL11 SYSREG(3, 6, 15, 9, 6)
#define SYSREG_FAR_GL11 SYSREG(3, 6, 15, 9, 7)

/*
 * From GL1, the *_GL11 names address the EL1 (non-guarded) bank, which we
 * keep in cpu->accel->gxf_el1_saved while guarded. Returns the storage for
 * such a register, or NULL if `reg` is not one of them / not guarded.
 */
static uint64_t *hvf_gxf_el1_bank_slot(CPUState *cpu, uint32_t reg)
{
    CPUARMState *env = cpu_env(cpu);
    AccelCPUState *acc = cpu->accel;

    if (!hvf_gxf_is_guarded(env)) {
        return NULL;
    }

    switch (reg) {
    case SYSREG_SP_GL11:
        return &acc->gxf_el1_saved.sp_el1;
    case SYSREG_TPIDR_GL11:
        return &acc->gxf_el1_saved.tpidr;
    case SYSREG_VBAR_GL11:
        return &acc->gxf_el1_saved.vbar;
    case SYSREG_SPSR_GL11:
        return &acc->gxf_el1_saved.spsr;
    case SYSREG_ESR_GL11:
        return &acc->gxf_el1_saved.esr;
    case SYSREG_ELR_GL11:
        return &acc->gxf_el1_saved.elr;
    case SYSREG_FAR_GL11:
        return &acc->gxf_el1_saved.far;
    default:
        return NULL;
    }
}

static void hvf_psci_cpu_off(ARMCPU *arm_cpu)
{
    int32_t ret = arm_set_cpu_off(arm_cpu_mp_affinity(arm_cpu));
    assert(ret == QEMU_ARM_POWERCTL_RET_SUCCESS);
}

/*
 * Handle a PSCI call.
 *
 * Returns 0 on success
 *         -1 when the PSCI call is unknown,
 */
static bool hvf_handle_psci_call(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    uint64_t param[4] = {
        env->xregs[0],
        env->xregs[1],
        env->xregs[2],
        env->xregs[3]
    };
    uint64_t context_id, mpidr;
    bool target_aarch64 = true;
    CPUState *target_cpu_state;
    ARMCPU *target_cpu;
    target_ulong entry;
    int target_el = 1;
    int32_t ret = 0;

    trace_hvf_psci_call(param[0], param[1], param[2], param[3],
                        arm_cpu_mp_affinity(arm_cpu));

    switch (param[0]) {
    case QEMU_PSCI_0_2_FN_PSCI_VERSION:
        ret = QEMU_PSCI_VERSION_1_1;
        break;
    case QEMU_PSCI_0_2_FN_MIGRATE_INFO_TYPE:
        ret = QEMU_PSCI_0_2_RET_TOS_MIGRATION_NOT_REQUIRED; /* No trusted OS */
        break;
    case QEMU_PSCI_0_2_FN_AFFINITY_INFO:
    case QEMU_PSCI_0_2_FN64_AFFINITY_INFO:
        mpidr = param[1];

        switch (param[2]) {
        case 0:
            target_cpu_state = arm_get_cpu_by_id(mpidr);
            if (!target_cpu_state) {
                ret = QEMU_PSCI_RET_INVALID_PARAMS;
                break;
            }
            target_cpu = ARM_CPU(target_cpu_state);

            ret = target_cpu->power_state;
            break;
        default:
            /* Everything above affinity level 0 is always on. */
            ret = 0;
        }
        break;
    case QEMU_PSCI_0_2_FN_SYSTEM_RESET:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        /*
         * QEMU reset and shutdown are async requests, but PSCI
         * mandates that we never return from the reset/shutdown
         * call, so power the CPU off now so it doesn't execute
         * anything further.
         */
        hvf_psci_cpu_off(arm_cpu);
        break;
    case QEMU_PSCI_0_2_FN_SYSTEM_OFF:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        hvf_psci_cpu_off(arm_cpu);
        break;
    case QEMU_PSCI_0_1_FN_CPU_ON:
    case QEMU_PSCI_0_2_FN_CPU_ON:
    case QEMU_PSCI_0_2_FN64_CPU_ON:
        mpidr = param[1];
        entry = param[2];
        context_id = param[3];
        ret = arm_set_cpu_on(mpidr, entry, context_id,
                             target_el, target_aarch64);
        break;
    case QEMU_PSCI_0_1_FN_CPU_OFF:
    case QEMU_PSCI_0_2_FN_CPU_OFF:
        hvf_psci_cpu_off(arm_cpu);
        break;
    case QEMU_PSCI_0_1_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN64_CPU_SUSPEND:
        /* Affinity levels are not supported in QEMU */
        if (param[1] & 0xfffe0000) {
            ret = QEMU_PSCI_RET_INVALID_PARAMS;
            break;
        }
        /* Powerdown is not supported, we always go into WFI */
        env->xregs[0] = 0;
        hvf_wfi(cpu);
        break;
    case QEMU_PSCI_0_1_FN_MIGRATE:
    case QEMU_PSCI_0_2_FN_MIGRATE:
        ret = QEMU_PSCI_RET_NOT_SUPPORTED;
        break;
    case QEMU_PSCI_1_0_FN_PSCI_FEATURES:
        switch (param[1]) {
        case QEMU_PSCI_0_2_FN_PSCI_VERSION:
        case QEMU_PSCI_0_2_FN_MIGRATE_INFO_TYPE:
        case QEMU_PSCI_0_2_FN_AFFINITY_INFO:
        case QEMU_PSCI_0_2_FN64_AFFINITY_INFO:
        case QEMU_PSCI_0_2_FN_SYSTEM_RESET:
        case QEMU_PSCI_0_2_FN_SYSTEM_OFF:
        case QEMU_PSCI_0_1_FN_CPU_ON:
        case QEMU_PSCI_0_2_FN_CPU_ON:
        case QEMU_PSCI_0_2_FN64_CPU_ON:
        case QEMU_PSCI_0_1_FN_CPU_OFF:
        case QEMU_PSCI_0_2_FN_CPU_OFF:
        case QEMU_PSCI_0_1_FN_CPU_SUSPEND:
        case QEMU_PSCI_0_2_FN_CPU_SUSPEND:
        case QEMU_PSCI_0_2_FN64_CPU_SUSPEND:
        case QEMU_PSCI_1_0_FN_PSCI_FEATURES:
            ret = 0;
            break;
        case QEMU_PSCI_0_1_FN_MIGRATE:
        case QEMU_PSCI_0_2_FN_MIGRATE:
        default:
            ret = QEMU_PSCI_RET_NOT_SUPPORTED;
        }
        break;
    default:
        return false;
    }

    env->xregs[0] = ret;
    return true;
}

static bool is_id_sysreg(uint32_t reg)
{
    return SYSREG_OP0(reg) == 3 &&
           SYSREG_OP1(reg) == 0 &&
           SYSREG_CRN(reg) == 0 &&
           SYSREG_CRM(reg) >= 1 &&
           SYSREG_CRM(reg) < 8;
}

static uint32_t hvf_reg2cp_reg(uint32_t reg)
{
    return ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP,
                              (reg >> SYSREG_CRN_SHIFT) & SYSREG_CRN_MASK,
                              (reg >> SYSREG_CRM_SHIFT) & SYSREG_CRM_MASK,
                              (reg >> SYSREG_OP0_SHIFT) & SYSREG_OP0_MASK,
                              (reg >> SYSREG_OP1_SHIFT) & SYSREG_OP1_MASK,
                              (reg >> SYSREG_OP2_SHIFT) & SYSREG_OP2_MASK);
}

static bool hvf_sysreg_read_cp(CPUState *cpu, const char *cpname,
                               uint32_t reg, uint64_t *val)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    const ARMCPRegInfo *ri;

    ri = ARMCPRegTable_cget(arm_cpu->cp_regs, hvf_reg2cp_reg(reg));
    if (ri) {
        if (!cp_access_ok(1, ri, true)) {
            return false;
        }
        if (ri->accessfn) {
            if (ri->accessfn(env, ri, true) != CP_ACCESS_OK) {
                return false;
            }
        }
        if (ri->type & ARM_CP_CONST) {
            *val = ri->resetvalue;
        } else if (ri->readfn) {
            *val = ri->readfn(env, ri);
        } else {
            *val = raw_read(env, ri);
        }
        trace_hvf_emu_reginfo_read(cpname, ri->name, *val);
        return true;
    }

    return false;
}

static bool hvf_sysreg_write_cp(CPUState *cpu, const char *cpname,
                                uint32_t reg, uint64_t val)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    const ARMCPRegInfo *ri;

    ri = ARMCPRegTable_cget(arm_cpu->cp_regs, hvf_reg2cp_reg(reg));

    if (ri) {
        if (!cp_access_ok(1, ri, false)) {
            return false;
        }
        if (ri->accessfn) {
            if (ri->accessfn(env, ri, false) != CP_ACCESS_OK) {
                return false;
            }
        }
        if (ri->writefn) {
            ri->writefn(env, ri, val);
        } else {
            raw_write(env, ri, val);
        }

        trace_hvf_emu_reginfo_write(cpname, ri->name, val);
        return true;
    }

    return false;
}

static int hvf_sysreg_read(CPUState *cpu, uint32_t reg, uint64_t *val)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    uint64_t *gl11;

    gl11 = hvf_gxf_el1_bank_slot(cpu, reg);
    if (gl11 != NULL) {
        *val = *gl11;
        trace_hvf_emu_reginfo_read("gxf-el1-bank", "GL11", *val);
        return 0;
    }

    if (arm_feature(env, ARM_FEATURE_PMU)) {
        switch (reg) {
        case SYSREG_PMCR_EL0:
            *val = env->cp15.c9_pmcr;
            return 0;
        case SYSREG_PMCCNTR_EL0:
            pmu_op_start(env);
            *val = env->cp15.c15_ccnt;
            pmu_op_finish(env);
            return 0;
        case SYSREG_PMCNTENCLR_EL0:
            *val = env->cp15.c9_pmcnten;
            return 0;
        case SYSREG_PMOVSCLR_EL0:
            *val = env->cp15.c9_pmovsr;
            return 0;
        case SYSREG_PMSELR_EL0:
            *val = env->cp15.c9_pmselr;
            return 0;
        case SYSREG_PMINTENCLR_EL1:
            *val = env->cp15.c9_pminten;
            return 0;
        case SYSREG_PMCCFILTR_EL0:
            *val = env->cp15.pmccfiltr_el0;
            return 0;
        case SYSREG_PMCNTENSET_EL0:
            *val = env->cp15.c9_pmcnten;
            return 0;
        case SYSREG_PMUSERENR_EL0:
            *val = env->cp15.c9_pmuserenr;
            return 0;
        case SYSREG_PMCEID0_EL0:
        case SYSREG_PMCEID1_EL0:
            /* We can't really count anything yet, declare all events invalid */
            *val = 0;
            return 0;
        }
    }

    switch (reg) {
    case SYSREG_CNTPCT_EL0:
        *val = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
              gt_cntfrq_period_ns(arm_cpu);
        return 0;
    case SYSREG_OSLSR_EL1:
        *val = env->cp15.oslsr_el1;
        return 0;
    case SYSREG_OSDLR_EL1:
        /* Dummy register */
        return 0;
    case SYSREG_ICC_AP0R0_EL1:
    case SYSREG_ICC_AP0R1_EL1:
    case SYSREG_ICC_AP0R2_EL1:
    case SYSREG_ICC_AP0R3_EL1:
    case SYSREG_ICC_AP1R0_EL1:
    case SYSREG_ICC_AP1R1_EL1:
    case SYSREG_ICC_AP1R2_EL1:
    case SYSREG_ICC_AP1R3_EL1:
    case SYSREG_ICC_ASGI1R_EL1:
    case SYSREG_ICC_BPR0_EL1:
    case SYSREG_ICC_BPR1_EL1:
    case SYSREG_ICC_DIR_EL1:
    case SYSREG_ICC_EOIR0_EL1:
    case SYSREG_ICC_EOIR1_EL1:
    case SYSREG_ICC_HPPIR0_EL1:
    case SYSREG_ICC_HPPIR1_EL1:
    case SYSREG_ICC_IAR0_EL1:
    case SYSREG_ICC_IAR1_EL1:
    case SYSREG_ICC_IGRPEN0_EL1:
    case SYSREG_ICC_IGRPEN1_EL1:
    case SYSREG_ICC_PMR_EL1:
    case SYSREG_ICC_RPR_EL1:
    case SYSREG_ICC_SGI0R_EL1:
    case SYSREG_ICC_SGI1R_EL1:
    case SYSREG_ICC_SRE_EL1:
    case SYSREG_ICC_CTLR_EL1:
        /* Call the TCG sysreg handler. This is only safe for GICv3 regs. */
        if (hvf_sysreg_read_cp(cpu, "GICv3", reg, val)) {
            return 0;
        }
        break;
    case SYSREG_DBGBVR0_EL1:
    case SYSREG_DBGBVR1_EL1:
    case SYSREG_DBGBVR2_EL1:
    case SYSREG_DBGBVR3_EL1:
    case SYSREG_DBGBVR4_EL1:
    case SYSREG_DBGBVR5_EL1:
    case SYSREG_DBGBVR6_EL1:
    case SYSREG_DBGBVR7_EL1:
    case SYSREG_DBGBVR8_EL1:
    case SYSREG_DBGBVR9_EL1:
    case SYSREG_DBGBVR10_EL1:
    case SYSREG_DBGBVR11_EL1:
    case SYSREG_DBGBVR12_EL1:
    case SYSREG_DBGBVR13_EL1:
    case SYSREG_DBGBVR14_EL1:
    case SYSREG_DBGBVR15_EL1:
        *val = env->cp15.dbgbvr[SYSREG_CRM(reg)];
        return 0;
    case SYSREG_DBGBCR0_EL1:
    case SYSREG_DBGBCR1_EL1:
    case SYSREG_DBGBCR2_EL1:
    case SYSREG_DBGBCR3_EL1:
    case SYSREG_DBGBCR4_EL1:
    case SYSREG_DBGBCR5_EL1:
    case SYSREG_DBGBCR6_EL1:
    case SYSREG_DBGBCR7_EL1:
    case SYSREG_DBGBCR8_EL1:
    case SYSREG_DBGBCR9_EL1:
    case SYSREG_DBGBCR10_EL1:
    case SYSREG_DBGBCR11_EL1:
    case SYSREG_DBGBCR12_EL1:
    case SYSREG_DBGBCR13_EL1:
    case SYSREG_DBGBCR14_EL1:
    case SYSREG_DBGBCR15_EL1:
        *val = env->cp15.dbgbcr[SYSREG_CRM(reg)];
        return 0;
    case SYSREG_DBGWVR0_EL1:
    case SYSREG_DBGWVR1_EL1:
    case SYSREG_DBGWVR2_EL1:
    case SYSREG_DBGWVR3_EL1:
    case SYSREG_DBGWVR4_EL1:
    case SYSREG_DBGWVR5_EL1:
    case SYSREG_DBGWVR6_EL1:
    case SYSREG_DBGWVR7_EL1:
    case SYSREG_DBGWVR8_EL1:
    case SYSREG_DBGWVR9_EL1:
    case SYSREG_DBGWVR10_EL1:
    case SYSREG_DBGWVR11_EL1:
    case SYSREG_DBGWVR12_EL1:
    case SYSREG_DBGWVR13_EL1:
    case SYSREG_DBGWVR14_EL1:
    case SYSREG_DBGWVR15_EL1:
        *val = env->cp15.dbgwvr[SYSREG_CRM(reg)];
        return 0;
    case SYSREG_DBGWCR0_EL1:
    case SYSREG_DBGWCR1_EL1:
    case SYSREG_DBGWCR2_EL1:
    case SYSREG_DBGWCR3_EL1:
    case SYSREG_DBGWCR4_EL1:
    case SYSREG_DBGWCR5_EL1:
    case SYSREG_DBGWCR6_EL1:
    case SYSREG_DBGWCR7_EL1:
    case SYSREG_DBGWCR8_EL1:
    case SYSREG_DBGWCR9_EL1:
    case SYSREG_DBGWCR10_EL1:
    case SYSREG_DBGWCR11_EL1:
    case SYSREG_DBGWCR12_EL1:
    case SYSREG_DBGWCR13_EL1:
    case SYSREG_DBGWCR14_EL1:
    case SYSREG_DBGWCR15_EL1:
        *val = env->cp15.dbgwcr[SYSREG_CRM(reg)];
        return 0;
    default:
        if (is_id_sysreg(reg)) {
            /* ID system registers read as RES0 */
            *val = 0;
            return 0;
        } else if (hvf_sysreg_read_cp(cpu, "fallback", reg, val)) {
            return 0;
        }
    }

    cpu_synchronize_state(cpu);
    trace_hvf_unhandled_sysreg_read(env->pc, reg,
                                    SYSREG_OP0(reg),
                                    SYSREG_OP1(reg),
                                    SYSREG_CRN(reg),
                                    SYSREG_CRM(reg),
                                    SYSREG_OP2(reg));
    hvf_raise_exception(cpu, EXCP_UDEF, syn_uncategorized(), 1);
    return 1;
}

static void pmu_update_irq(CPUARMState *env)
{
    ARMCPU *cpu = env_archcpu(env);
    qemu_set_irq(cpu->pmu_interrupt, (env->cp15.c9_pmcr & PMCRE) &&
            (env->cp15.c9_pminten & env->cp15.c9_pmovsr));
}

static bool pmu_event_supported(uint16_t number)
{
    return false;
}

/* Returns true if the counter (pass 31 for PMCCNTR) should count events using
 * the current EL, security state, and register configuration.
 */
static bool pmu_counter_enabled(CPUARMState *env, uint8_t counter)
{
    uint64_t filter;
    bool enabled, filtered = true;
    int el = arm_current_el(env);

    enabled = (env->cp15.c9_pmcr & PMCRE) &&
              (env->cp15.c9_pmcnten & (1 << counter));

    if (counter == 31) {
        filter = env->cp15.pmccfiltr_el0;
    } else {
        filter = env->cp15.c14_pmevtyper[counter];
    }

    if (el == 0) {
        filtered = filter & PMXEVTYPER_U;
    } else if (el == 1) {
        filtered = filter & PMXEVTYPER_P;
    }

    if (counter != 31) {
        /*
         * If not checking PMCCNTR, ensure the counter is setup to an event we
         * support
         */
        uint16_t event = filter & PMXEVTYPER_EVTCOUNT;
        if (!pmu_event_supported(event)) {
            return false;
        }
    }

    return enabled && !filtered;
}

static void pmswinc_write(CPUARMState *env, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < pmu_num_counters(env); i++) {
        /* Increment a counter's count iff: */
        if ((value & (1 << i)) && /* counter's bit is set */
                /* counter is enabled and not filtered */
                pmu_counter_enabled(env, i) &&
                /* counter is SW_INCR */
                (env->cp15.c14_pmevtyper[i] & PMXEVTYPER_EVTCOUNT) == 0x0) {
            /*
             * Detect if this write causes an overflow since we can't predict
             * PMSWINC overflows like we can for other events
             */
            uint32_t new_pmswinc = env->cp15.c14_pmevcntr[i] + 1;

            if (env->cp15.c14_pmevcntr[i] & ~new_pmswinc & INT32_MIN) {
                env->cp15.c9_pmovsr |= (1 << i);
                pmu_update_irq(env);
            }

            env->cp15.c14_pmevcntr[i] = new_pmswinc;
        }
    }
}

static int hvf_sysreg_write(CPUState *cpu, uint32_t reg, uint64_t val)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    uint64_t *gl11;

    gl11 = hvf_gxf_el1_bank_slot(cpu, reg);
    if (gl11 != NULL) {
        *gl11 = val;
        trace_hvf_emu_reginfo_write("gxf-el1-bank", "GL11", val);
        return 0;
    }

    trace_hvf_sysreg_write(reg,
                           SYSREG_OP0(reg),
                           SYSREG_OP1(reg),
                           SYSREG_CRN(reg),
                           SYSREG_CRM(reg),
                           SYSREG_OP2(reg),
                           val);

    if (arm_feature(env, ARM_FEATURE_PMU)) {
        switch (reg) {
        case SYSREG_PMCCNTR_EL0:
            pmu_op_start(env);
            env->cp15.c15_ccnt = val;
            pmu_op_finish(env);
            return 0;
        case SYSREG_PMCR_EL0:
            pmu_op_start(env);

            if (val & PMCRC) {
                /* The counter has been reset */
                env->cp15.c15_ccnt = 0;
            }

            if (val & PMCRP) {
                unsigned int i;
                for (i = 0; i < pmu_num_counters(env); i++) {
                    env->cp15.c14_pmevcntr[i] = 0;
                }
            }

            env->cp15.c9_pmcr &= ~PMCR_WRITABLE_MASK;
            env->cp15.c9_pmcr |= (val & PMCR_WRITABLE_MASK);

            pmu_op_finish(env);
            return 0;
        case SYSREG_PMUSERENR_EL0:
            env->cp15.c9_pmuserenr = val & 0xf;
            return 0;
        case SYSREG_PMCNTENSET_EL0:
            env->cp15.c9_pmcnten |= (val & pmu_counter_mask(env));
            return 0;
        case SYSREG_PMCNTENCLR_EL0:
            env->cp15.c9_pmcnten &= ~(val & pmu_counter_mask(env));
            return 0;
        case SYSREG_PMINTENCLR_EL1:
            pmu_op_start(env);
            env->cp15.c9_pminten |= val;
            pmu_op_finish(env);
            return 0;
        case SYSREG_PMOVSCLR_EL0:
            pmu_op_start(env);
            env->cp15.c9_pmovsr &= ~val;
            pmu_op_finish(env);
            return 0;
        case SYSREG_PMSWINC_EL0:
            pmu_op_start(env);
            pmswinc_write(env, val);
            pmu_op_finish(env);
            return 0;
        case SYSREG_PMSELR_EL0:
            env->cp15.c9_pmselr = val & 0x1f;
            return 0;
        case SYSREG_PMCCFILTR_EL0:
            pmu_op_start(env);
            env->cp15.pmccfiltr_el0 = val & PMCCFILTR_EL0;
            pmu_op_finish(env);
            return 0;
        }
    }

    switch (reg) {
    case SYSREG_OSLAR_EL1:
        env->cp15.oslsr_el1 = val & 1;
        return 0;
    /*
     * SYSREG_CNTP_CTL_EL0 is deliberately *not* handled here.
     *
     * Upstream ignores writes to it ("guests should not rely on the physical
     * counter, but macOS emits disable writes to it"). Dropping them silently
     * breaks any guest that actually uses the physical timer -- notably the
     * emulated SEPROM, which runs on the SEP core under HVF: it does
     *
     *     msr CNTP_TVAL_EL0, x9   ; <- worked (falls through to the cpreg path)
     *     msr CNTP_CTL_EL0,  #3   ; ENABLE|IMASK  <- was silently dropped
     *     ...
     *     mrs x8, CNTP_CTL_EL0
     *     tbz w8, #0, <panic 0x7E>
     *
     * and the read then returned 0, so SEPROM panicked, and its panic handler
     * panicked again (0x118) and parked the core in wfi/wfe forever. That is
     * why SEP never serviced its mailbox and a restore's RSEP boot timed out.
     *
     * Letting it fall through to the generic fallback below (the same path
     * CNTP_TVAL_EL0 already takes) runs the real gt_phys_redir_ctl_write(),
     * which is also what macOS's disable writes actually mean.
     */
    case SYSREG_OSDLR_EL1:
        /* Dummy register */
        return 0;
    case SYSREG_LORC_EL1:
        /* Dummy register */
        return 0;
    case SYSREG_ICC_AP0R0_EL1:
    case SYSREG_ICC_AP0R1_EL1:
    case SYSREG_ICC_AP0R2_EL1:
    case SYSREG_ICC_AP0R3_EL1:
    case SYSREG_ICC_AP1R0_EL1:
    case SYSREG_ICC_AP1R1_EL1:
    case SYSREG_ICC_AP1R2_EL1:
    case SYSREG_ICC_AP1R3_EL1:
    case SYSREG_ICC_ASGI1R_EL1:
    case SYSREG_ICC_BPR0_EL1:
    case SYSREG_ICC_BPR1_EL1:
    case SYSREG_ICC_CTLR_EL1:
    case SYSREG_ICC_DIR_EL1:
    case SYSREG_ICC_EOIR0_EL1:
    case SYSREG_ICC_EOIR1_EL1:
    case SYSREG_ICC_HPPIR0_EL1:
    case SYSREG_ICC_HPPIR1_EL1:
    case SYSREG_ICC_IAR0_EL1:
    case SYSREG_ICC_IAR1_EL1:
    case SYSREG_ICC_IGRPEN0_EL1:
    case SYSREG_ICC_IGRPEN1_EL1:
    case SYSREG_ICC_PMR_EL1:
    case SYSREG_ICC_RPR_EL1:
    case SYSREG_ICC_SGI0R_EL1:
    case SYSREG_ICC_SGI1R_EL1:
    case SYSREG_ICC_SRE_EL1:
        /* Call the TCG sysreg handler. This is only safe for GICv3 regs. */
        if (hvf_sysreg_write_cp(cpu, "GICv3", reg, val)) {
            return 0;
        }
        break;
    case SYSREG_MDSCR_EL1:
        env->cp15.mdscr_el1 = val;
        return 0;
    case SYSREG_DBGBVR0_EL1:
    case SYSREG_DBGBVR1_EL1:
    case SYSREG_DBGBVR2_EL1:
    case SYSREG_DBGBVR3_EL1:
    case SYSREG_DBGBVR4_EL1:
    case SYSREG_DBGBVR5_EL1:
    case SYSREG_DBGBVR6_EL1:
    case SYSREG_DBGBVR7_EL1:
    case SYSREG_DBGBVR8_EL1:
    case SYSREG_DBGBVR9_EL1:
    case SYSREG_DBGBVR10_EL1:
    case SYSREG_DBGBVR11_EL1:
    case SYSREG_DBGBVR12_EL1:
    case SYSREG_DBGBVR13_EL1:
    case SYSREG_DBGBVR14_EL1:
    case SYSREG_DBGBVR15_EL1:
        env->cp15.dbgbvr[SYSREG_CRM(reg)] = val;
        return 0;
    case SYSREG_DBGBCR0_EL1:
    case SYSREG_DBGBCR1_EL1:
    case SYSREG_DBGBCR2_EL1:
    case SYSREG_DBGBCR3_EL1:
    case SYSREG_DBGBCR4_EL1:
    case SYSREG_DBGBCR5_EL1:
    case SYSREG_DBGBCR6_EL1:
    case SYSREG_DBGBCR7_EL1:
    case SYSREG_DBGBCR8_EL1:
    case SYSREG_DBGBCR9_EL1:
    case SYSREG_DBGBCR10_EL1:
    case SYSREG_DBGBCR11_EL1:
    case SYSREG_DBGBCR12_EL1:
    case SYSREG_DBGBCR13_EL1:
    case SYSREG_DBGBCR14_EL1:
    case SYSREG_DBGBCR15_EL1:
        env->cp15.dbgbcr[SYSREG_CRM(reg)] = val;
        return 0;
    case SYSREG_DBGWVR0_EL1:
    case SYSREG_DBGWVR1_EL1:
    case SYSREG_DBGWVR2_EL1:
    case SYSREG_DBGWVR3_EL1:
    case SYSREG_DBGWVR4_EL1:
    case SYSREG_DBGWVR5_EL1:
    case SYSREG_DBGWVR6_EL1:
    case SYSREG_DBGWVR7_EL1:
    case SYSREG_DBGWVR8_EL1:
    case SYSREG_DBGWVR9_EL1:
    case SYSREG_DBGWVR10_EL1:
    case SYSREG_DBGWVR11_EL1:
    case SYSREG_DBGWVR12_EL1:
    case SYSREG_DBGWVR13_EL1:
    case SYSREG_DBGWVR14_EL1:
    case SYSREG_DBGWVR15_EL1:
        env->cp15.dbgwvr[SYSREG_CRM(reg)] = val;
        return 0;
    case SYSREG_DBGWCR0_EL1:
    case SYSREG_DBGWCR1_EL1:
    case SYSREG_DBGWCR2_EL1:
    case SYSREG_DBGWCR3_EL1:
    case SYSREG_DBGWCR4_EL1:
    case SYSREG_DBGWCR5_EL1:
    case SYSREG_DBGWCR6_EL1:
    case SYSREG_DBGWCR7_EL1:
    case SYSREG_DBGWCR8_EL1:
    case SYSREG_DBGWCR9_EL1:
    case SYSREG_DBGWCR10_EL1:
    case SYSREG_DBGWCR11_EL1:
    case SYSREG_DBGWCR12_EL1:
    case SYSREG_DBGWCR13_EL1:
    case SYSREG_DBGWCR14_EL1:
    case SYSREG_DBGWCR15_EL1:
        env->cp15.dbgwcr[SYSREG_CRM(reg)] = val;
        return 0;
    default:
        if (!is_id_sysreg(reg) && hvf_sysreg_write_cp(cpu, "fallback", reg, val)) {
            return 0;
        }
        break;
    }

    cpu_synchronize_state(cpu);
    trace_hvf_unhandled_sysreg_write(env->pc, reg,
                                     SYSREG_OP0(reg),
                                     SYSREG_OP1(reg),
                                     SYSREG_CRN(reg),
                                     SYSREG_CRM(reg),
                                     SYSREG_OP2(reg));
    hvf_raise_exception(cpu, EXCP_UDEF, syn_uncategorized(), 1);
    return 1;
}

/* Must be called by the owning thread */
/*
 * Off by default: none of this is needed any more.
 *
 * Withholding interrupts from guarded mode, masking the vtimer across it and
 * re-tagging the VBAR_GL1 page were all built to work around boots that wedged
 * with a vCPU spinning on a fault taken inside GL1. The actual cause was the
 * pmap_switch PAC patch corrupting SCTLR_EL1 (see
 * ck_kp_hvf_pac_pmap_switch_callback); with that fixed, 24/24 boots succeed
 * with all of this disabled, slightly faster than with it enabled -- each
 * GENTER was paying two extra hypercalls, ~1.2M of them per boot.
 *
 * Kept behind INFERNO_HVF_GXF_IRQ_DEFER=1 because the hazard it describes is
 * real: the host CPU never enters GL1, so a fault taken there vectors through
 * a page it cannot fetch.
 */
static bool hvf_gxf_defer_irq(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("INFERNO_HVF_GXF_IRQ_DEFER");
        on = e != NULL && atoi(e) != 0;
    }
    return on;
}

static int hvf_inject_interrupts(CPUState *cpu)
{
    /*
     * Never deliver an interrupt while the guest is in guarded mode.
     *
     * XNU's ppl_dispatch unmasks IRQs around interruptible PPL calls, so an
     * interrupt can land while GL1 is active. On real silicon that vectors
     * through VBAR_GL1 into PPL text, which GXF makes executable for the
     * duration. We emulate the GXF register bank but the guest's SPRR/APRR
     * writes are trapped and answered out of `env`, so the host's stage-1
     * permissions still say that page is not executable at EL1. The fetch
     * takes a permission fault, which vectors to the *same* page, and the vCPU
     * spins on an instruction abort forever at 100% with no hypervisor exit --
     * the boot stops dead with no further serial output. It hit roughly a
     * third of boots, which is how often an interrupt happened to arrive
     * inside that window.
     *
     * Holding the interrupt costs nothing: GEXIT is rewritten to an HVC, so we
     * are called again as soon as the guest leaves guarded mode, and guarded
     * sections are short. INFERNO_HVF_GXF_IRQ_DEFER=0 restores the old
     * behaviour.
     */
    if (hvf_gxf_defer_irq() && hvf_gxf_is_guarded(&ARM_CPU(cpu)->env)) {
        /*
         * Withdraw rather than merely skip. hv_vcpu_set_pending_interrupt() is
         * sticky: an interrupt armed before the guest ran GENTER stays pending
         * and HVF delivers it the instant ppl_dispatch unmasks IRQs, so not
         * calling it again changes nothing. Both lines are re-asserted from
         * the top of hvf_arch_vcpu_exec() once the guest leaves GL1.
         */
        hv_vcpu_set_pending_interrupt(cpu->accel->fd, HV_INTERRUPT_TYPE_IRQ,
                                      false);
        hv_vcpu_set_pending_interrupt(cpu->accel->fd, HV_INTERRUPT_TYPE_FIQ,
                                      false);
        if (cpu_test_interrupt(cpu, CPU_INTERRUPT_FIQ) ||
            cpu_test_interrupt(cpu, CPU_INTERRUPT_HARD)) {
            cpu->accel->gxf_irq_deferred++;
            hvf_ev(cpu, HVF_EV_WITHDRAW, 0);
        }
        return 0;
    }

    if (cpu_test_interrupt(cpu, CPU_INTERRUPT_FIQ)) {
        hvf_ev(cpu, HVF_EV_FIQ_ARM, 0);
        trace_hvf_inject_fiq();
        hv_vcpu_set_pending_interrupt(cpu->accel->fd, HV_INTERRUPT_TYPE_FIQ,
                                      true);
    }

    if (cpu_test_interrupt(cpu, CPU_INTERRUPT_HARD)) {
        hvf_ev(cpu, HVF_EV_IRQ_ARM, 0);
        trace_hvf_inject_irq();
        hv_vcpu_set_pending_interrupt(cpu->accel->fd, HV_INTERRUPT_TYPE_IRQ,
                                      true);
    }

    return 0;
}

static uint64_t hvf_vtimer_val_raw(void)
{
    /*
     * mach_absolute_time() returns the vtimer value without the VM
     * offset that we define. Add our own offset on top.
     */
    return mach_absolute_time() - hvf_state->vtimer_offset;
}

static uint64_t hvf_vtimer_val(void)
{
    if (!runstate_is_running()) {
        /* VM is paused, the vtimer value is in vtimer.vtimer_val */
        return vtimer.vtimer_val;
    }

    return hvf_vtimer_val_raw();
}

static void hvf_wait_for_ipi(CPUState *cpu, struct timespec *ts)
{
    /*
     * Use pselect to sleep so that other threads can IPI us while we're
     * sleeping.
     */
    qatomic_set_mb(&cpu->thread_kicked, false);
    bql_unlock();
    pselect(0, 0, 0, 0, ts, &cpu->accel->unblock_ipi_mask);
    bql_lock();
}

static void hvf_wfi(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    struct timespec ts;
    hv_return_t r;
    uint64_t ctl;
    uint64_t cval;
    int64_t ticks_to_sleep;
    uint64_t seconds;
    uint64_t nanos;
    uint32_t cntfrq;

    if (cpu_test_interrupt(cpu, CPU_INTERRUPT_HARD | CPU_INTERRUPT_FIQ)) {
        /* Interrupt pending, no need to wait */
        return;
    }

    r = hv_vcpu_get_sys_reg(cpu->accel->fd, HV_SYS_REG_CNTV_CTL_EL0, &ctl);
    assert_hvf_ok(r);

    if (!(ctl & 1) || (ctl & 2)) {
        /* Timer disabled or masked, just wait for an IPI. */
        hvf_wait_for_ipi(cpu, NULL);
        return;
    }

    r = hv_vcpu_get_sys_reg(cpu->accel->fd, HV_SYS_REG_CNTV_CVAL_EL0, &cval);
    assert_hvf_ok(r);

    ticks_to_sleep = cval - hvf_vtimer_val();
    if (ticks_to_sleep < 0) {
        return;
    }

    cntfrq = gt_cntfrq_period_ns(arm_cpu);
    seconds = muldiv64(ticks_to_sleep, cntfrq, NANOSECONDS_PER_SECOND);
    ticks_to_sleep -= muldiv64(seconds, NANOSECONDS_PER_SECOND, cntfrq);
    nanos = ticks_to_sleep * cntfrq;

    /*
     * Don't sleep for less than the time a context switch would take,
     * so that we can satisfy fast timer requests on the same CPU.
     * Measurements on M1 show the sweet spot to be ~2ms.
     */
    if (!seconds && nanos < (2 * SCALE_MS)) {
        return;
    }

    ts = (struct timespec) { seconds, nanos };
    hvf_wait_for_ipi(cpu, &ts);
}

/* Must be called by the owning thread */
static void hvf_sync_vtimer(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    hv_return_t r;
    uint64_t ctl;
    bool irq_state;

    if (!cpu->accel->vtimer_masked) {
        /* We will get notified on vtimer changes by hvf, nothing to do */
        return;
    }

    r = hv_vcpu_get_sys_reg(cpu->accel->fd, HV_SYS_REG_CNTV_CTL_EL0, &ctl);
    assert_hvf_ok(r);

    irq_state = (ctl & (TMR_CTL_ENABLE | TMR_CTL_IMASK | TMR_CTL_ISTATUS)) ==
                (TMR_CTL_ENABLE | TMR_CTL_ISTATUS);
    qemu_set_irq(arm_cpu->gt_timer_outputs[GTIMER_VIRT], irq_state);

    if (!irq_state) {
        /*
         * Timer no longer asserting, we can unmask it -- but not while the
         * guest is in guarded mode. The virtual timer is delivered by hardware
         * without going through hv_vcpu_set_pending_interrupt(), so unmasking
         * here would reopen the one path by which an interrupt can still be
         * taken in GL1, where the vector page is not executable. GEXIT calls
         * back in through hvf_handle_vmexit(), so the unmask only slips by one
         * guarded section.
         */
        if (hvf_gxf_defer_irq() && hvf_gxf_is_guarded(&ARM_CPU(cpu)->env)) {
            return;
        }
        hvf_ev(cpu, HVF_EV_VT_UNMASK, 0);
        r = hv_vcpu_set_vtimer_mask(cpu->accel->fd, false);
        assert_hvf_ok(r);
        cpu->accel->vtimer_masked = false;
    }
}

/*
 * Exit accounting. Profiling the guest boot showed the vCPU threads spending
 * ~61% of their time blocked on the BQL in hvf_arch_vcpu_exec and only ~14%
 * actually inside hv_vcpu_run -- every exit takes the global lock, so what
 * matters for guest speed is how many exits there are and of what kind.
 * INFERNO_HVF_EXIT_STATS=<n> prints a histogram every n exits.
 */
static uint64_t hvf_exit_counts[64];
#define HVF_SYSREG_SLOTS 512
static uint32_t hvf_sysreg_keys[HVF_SYSREG_SLOTS];
static uint64_t hvf_sysreg_counts[HVF_SYSREG_SLOTS];
static uint64_t hvf_exit_total;

/* Keep the whole encoding, not a truncated one: op0/op1/op2 live above CRN. */
static void hvf_count_sysreg(uint32_t reg)
{
    int i;

    for (i = 0; i < HVF_SYSREG_SLOTS; i++) {
        if (hvf_sysreg_keys[i] == reg) {
            hvf_sysreg_counts[i]++;
            return;
        }
        if (hvf_sysreg_keys[i] == 0 && hvf_sysreg_counts[i] == 0) {
            hvf_sysreg_keys[i] = reg;
            hvf_sysreg_counts[i] = 1;
            return;
        }
    }
}

static void hvf_account_exit(uint32_t ec, uint64_t syndrome)
{
    static int64_t every = -1;

    if (every < 0) {
        const char *env_every = getenv("INFERNO_HVF_EXIT_STATS");
        every = env_every != NULL ? strtoll(env_every, NULL, 0) : 0;
    }
    if (every == 0) {
        return;
    }

    qatomic_inc(&hvf_exit_counts[ec & 63]);
    if (ec == EC_SYSTEMREGISTERTRAP) {
        /* keep reads and writes apart: a spin shows up as reads >> writes */
        hvf_count_sysreg((syndrome & SYSREG_MASK) | (syndrome & 1 ? 0x80000000u : 0));
    }
    if (qatomic_fetch_inc(&hvf_exit_total) % every == 0) {
        int i;
        fprintf(stderr, "hvf-exits total=%llu:",
                (unsigned long long)hvf_exit_total);
        for (i = 0; i < 64; i++) {
            if (hvf_exit_counts[i]) {
                fprintf(stderr, " ec%02x=%llu", i,
                        (unsigned long long)hvf_exit_counts[i]);
            }
        }
        fprintf(stderr, "\n");
        if (hvf_exit_counts[EC_SYSTEMREGISTERTRAP & 63]) {
            int top[8] = {0}, n;
            for (i = 0; i < HVF_SYSREG_SLOTS; i++) {
                for (n = 0; n < 8; n++) {
                    if (hvf_sysreg_counts[i] > hvf_sysreg_counts[top[n]]) {
                        memmove(&top[n + 1], &top[n], (7 - n) * sizeof(int));
                        top[n] = i;
                        break;
                    }
                }
            }
            fprintf(stderr, "hvf-sysreg:");
            for (n = 0; n < 8 && hvf_sysreg_counts[top[n]]; n++) {
                uint32_t reg = hvf_sysreg_keys[top[n]];
                fprintf(stderr, " s%d_%d_c%d_c%d_%d%s=%llu",
                        SYSREG_OP0(reg), SYSREG_OP1(reg), SYSREG_CRN(reg),
                        SYSREG_CRM(reg), SYSREG_OP2(reg),
                        (hvf_sysreg_keys[top[n]] & 0x80000000u) ? "/r" : "/w",
                        (unsigned long long)hvf_sysreg_counts[top[n]]);
            }
            fprintf(stderr, "\n");
        }
    }
}


/*
 * A GXF transition only touches a dozen registers, but the HVC path was
 * reaching for cpu_synchronize_state() and then leaving vcpu_dirty set, i.e. a
 * full get of every cpreg on the way in and a full put on the way out -- about
 * 250 hv_vcpu_get_sys_reg()/set_sys_reg() calls each way, 2.4 million times per
 * boot. Sync exactly what hvf_gxf_enter()/hvf_gxf_exit() read and write
 * instead. Off by default: measured over 5 boots each it showed no benefit
 * (min 31 s vs 36 s, median worse) and it is a partial state sync, so the risk
 * is not worth an unproven gain. INFERNO_HVF_FAST_GXF=1 turns it on.
 */
static const struct {
    uint16_t hv;
    size_t offset;
} hvf_gxf_regs[] = {
    { HV_SYS_REG_SP_EL0,    offsetof(CPUARMState, sp_el[0]) },
    { HV_SYS_REG_SP_EL1,    offsetof(CPUARMState, sp_el[1]) },
    { HV_SYS_REG_VBAR_EL1,  offsetof(CPUARMState, cp15.vbar_el[1]) },
    { HV_SYS_REG_TPIDR_EL1, offsetof(CPUARMState, cp15.tpidr_el[1]) },
    { HV_SYS_REG_SPSR_EL1,  offsetof(CPUARMState, banked_spsr[BANK_SVC]) },
    { HV_SYS_REG_ELR_EL1,   offsetof(CPUARMState, elr_el[1]) },
    { HV_SYS_REG_ESR_EL1,   offsetof(CPUARMState, cp15.esr_el[1]) },
    { HV_SYS_REG_FAR_EL1,   offsetof(CPUARMState, cp15.far_el[1]) },
    { HV_SYS_REG_SCTLR_EL1, offsetof(CPUARMState, cp15.sctlr_el[1]) },
};

static bool hvf_gxf_fast_enabled(void)
{
    static int8_t enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("INFERNO_HVF_FAST_GXF");
        enabled = (e == NULL || atoi(e) != 0) ? 1 : 0;
    }
    return enabled != 0;
}

static void hvf_gxf_fast_sync_in(CPUState *cpu)
{
    CPUARMState *env = cpu_env(cpu);
    uint64_t val;
    int i;

    for (i = 0; i < ARRAY_SIZE(hvf_gxf_regs); i++) {
        assert_hvf_ok(hv_vcpu_get_sys_reg(cpu->accel->fd, hvf_gxf_regs[i].hv,
                                          &val));
        *(uint64_t *)((void *)env + hvf_gxf_regs[i].offset) = val;
    }
    assert_hvf_ok(hv_vcpu_get_reg(cpu->accel->fd, HV_REG_PC, &val));
    env->pc = val;
    assert_hvf_ok(hv_vcpu_get_reg(cpu->accel->fd, HV_REG_CPSR, &val));
    pstate_write(env, val);
    hvf_restore_sp(env);
}

static void hvf_gxf_fast_sync_out(CPUState *cpu)
{
    CPUARMState *env = cpu_env(cpu);
    int i;

    hvf_save_sp(env);
    for (i = 0; i < ARRAY_SIZE(hvf_gxf_regs); i++) {
        assert_hvf_ok(hv_vcpu_set_sys_reg(
            cpu->accel->fd, hvf_gxf_regs[i].hv,
            *(uint64_t *)((void *)env + hvf_gxf_regs[i].offset)));
    }
    assert_hvf_ok(hv_vcpu_set_reg(cpu->accel->fd, HV_REG_PC, env->pc));
    assert_hvf_ok(hv_vcpu_set_reg(cpu->accel->fd, HV_REG_CPSR,
                                  pstate_read(env)));
}

/* Only take the short path when enter/exit will not need anything else. */
static bool hvf_gxf_fast_ok(CPUState *cpu, bool genter)
{
    CPUARMState *env = cpu_env(cpu);

    if (arm_current_el(env) != 1) {
        return false;
    }
    if (genter) {
        return (env->gxf.gxf_config_el[1] & 1) && !hvf_gxf_is_guarded(env);
    }
    return hvf_gxf_is_guarded(env);
}

static int hvf_handle_exception(CPUState *cpu, hv_vcpu_exit_exception_t *excp)
{
    CPUARMState *env = cpu_env(cpu);
    ARMCPU *arm_cpu = env_archcpu(env);
    uint64_t syndrome = excp->syndrome;
    uint32_t ec = syn_get_ec(syndrome);
    bool advance_pc = false;
    hv_return_t r;
    int ret = 0;

    hvf_account_exit(ec, syndrome);

    /*
     * The hottest trap by far is one Apple implementation-defined register --
     * INFERNO_HVF_TRACE_SYSREG=<encoding> prints the guest PC for the first few
     * accesses so the kernel code hammering it can be found.
     */
    if (ec == EC_SYSTEMREGISTERTRAP) {
        static int64_t want = -1;
        static int shown;

        if (want < 0) {
            const char *e = getenv("INFERNO_HVF_TRACE_SYSREG");
            want = e != NULL ? strtoll(e, NULL, 0) : 0;
        }
        if (want != 0 && (syndrome & SYSREG_MASK) == (uint32_t)want &&
            shown < 8) {
            shown++;
            cpu_synchronize_state(cpu);
            fprintf(stderr, "hvf-sysreg-pc: reg=%#x read=%d pc=%#llx\n",
                    (uint32_t)(syndrome & SYSREG_MASK), (int)(syndrome & 1),
                    (unsigned long long)env->pc);
        }
    }

    switch (ec) {
    case EC_SOFTWARESTEP: {
        ret = EXCP_DEBUG;

        if (!cpu->singlestep_enabled) {
            error_report("EC_SOFTWARESTEP but single-stepping not enabled");
        }
        break;
    }
    case EC_AA64_BKPT: {
        ret = EXCP_DEBUG;

        cpu_synchronize_state(cpu);

        if (!hvf_find_sw_breakpoint(cpu, env->pc)) {
            /* Re-inject into the guest */
            ret = 0;
            hvf_raise_exception(cpu, EXCP_BKPT, syn_aa64_bkpt(0), 1);
        }
        break;
    }
    case EC_BREAKPOINT: {
        ret = EXCP_DEBUG;

        cpu_synchronize_state(cpu);

        if (!find_hw_breakpoint(cpu, env->pc)) {
            error_report("EC_BREAKPOINT but unknown hw breakpoint");
        }
        break;
    }
    case EC_WATCHPOINT: {
        ret = EXCP_DEBUG;

        cpu_synchronize_state(cpu);

        CPUWatchpoint *wp =
            find_hw_watchpoint(cpu, excp->virtual_address);
        if (!wp) {
            error_report("EXCP_DEBUG but unknown hw watchpoint");
        }
        cpu->watchpoint_hit = wp;
        break;
    }
    case EC_DATAABORT: {
        bool isv = syndrome & ARM_EL_ISV;
        bool iswrite = (syndrome >> 6) & 1;
        bool s1ptw = (syndrome >> 7) & 1;
        bool sse = (syndrome >> 21) & 1;
        uint32_t sas = (syndrome >> 22) & 3;
        uint32_t len = 1 << sas;
        uint32_t srt = (syndrome >> 16) & 0x1f;
        uint32_t cm = (syndrome >> 8) & 0x1;
        uint64_t val = 0;

        trace_hvf_data_abort(excp->virtual_address,
                             excp->physical_address, isv,
                             iswrite, s1ptw, len, srt);

        if (cm) {
            /* We don't cache MMIO regions */
            advance_pc = true;
            break;
        }

        AddressSpace *as = cpu_get_address_space(cpu, ARMASIdx_NS);
        if (isv) {
            if (iswrite) {
                val = hvf_get_reg(cpu, srt);
                if (address_space_write(as,
                        excp->physical_address,
                        MEMTXATTRS_UNSPECIFIED, &val, len) != MEMTX_OK) {
                    fprintf(stderr, "%s: Failed to write MMIO at 0x%llX\n",
                            __func__, excp->physical_address);
                    hvf_raise_exception(cpu, EXCP_DATA_ABORT, syndrome, 1);
                    break;
                }
            } else {
                if (address_space_read(as, excp->physical_address,
                        MEMTXATTRS_UNSPECIFIED, &val, len) != MEMTX_OK) {
                    fprintf(stderr, "%s: Failed to read MMIO at 0x%llX\n",
                            __func__, excp->physical_address);
                    hvf_raise_exception(cpu, EXCP_DATA_ABORT, syndrome, 1);
                    break;
                }
                if (sse) {
                    val = sextract64(val, 0, len * 8);
                }
                hvf_set_reg(cpu, srt, val);
            }
        } else if (!arm_aarch64_fallback_emu_single(cpu, as, hvf_get_reg,
                                                    hvf_set_reg)) {
            hvf_raise_exception(cpu, EXCP_DATA_ABORT, syndrome, 1);
            fprintf(stderr, "%s: instruction decoding failed\n", __func__);
            break;
        }

        advance_pc = true;
        break;
    }
    case EC_SYSTEMREGISTERTRAP: {
        bool isread = (syndrome >> 0) & 1;
        uint32_t rt = (syndrome >> 5) & 0x1f;
        uint32_t reg = syndrome & SYSREG_MASK;
        uint64_t val;
        int sysreg_ret = 0;

        /*
         * The GXF/SPRR register handlers look at PSTATE and, while guarded,
         * at the live EL1 bank; make sure env is current (and dirty, so
         * that any changes get flushed back).
         */
        bool gxf_sync = hvf_gxf_sysreg_needs_sync(reg);
        bool gxf_guarded = false;

        if (gxf_sync) {
            cpu_synchronize_state(cpu);
            gxf_guarded = hvf_gxf_is_guarded(env);
            if (getenv("INFERNO_DEBUG_SPRR") &&
                (reg == SYSREG(3, 6, 15, 1, 5) ||
                 reg == SYSREG(3, 6, 15, 3, 1) ||
                 reg == SYSREG(3, 6, 15, 1, 7) ||
                 reg == SYSREG(3, 6, 15, 3, 3))) {
                fprintf(stderr,
                        "[sprr] cpu%d el%d %s S3_6_C15_C%u_%u pc=%#llx "
                        "in=%#llx cur_sprr_el0=%#llx mprr_el0=%#llx\n",
                        cpu->cpu_index, arm_current_el(env),
                        isread ? "rd" : "wr", SYSREG_CRM(reg), SYSREG_OP2(reg),
                        (unsigned long long)env->pc,
                        (unsigned long long)(isread ? 0 : hvf_get_reg(cpu, rt)),
                        (unsigned long long)env->sprr.sprr_el_br_el1[0][0],
                        (unsigned long long)env->sprr.mprr_el_br_el1[0][0]);
            }
            if (gxf_guarded) {
                /* While guarded the GL1 bank lives in the real EL1 regs. */
                hvf_gxf_sync_live_to_gl(env);
            }
        }

        if (isread) {
            sysreg_ret = hvf_sysreg_read(cpu, reg, &val);
        } else {
            val = hvf_get_reg(cpu, rt);
            sysreg_ret = hvf_sysreg_write(cpu, reg, val);
        }

        if (gxf_sync) {
            /* hvf_get_reg() flushed; re-dirty so our changes get pushed. */
            cpu->vcpu_dirty = true;
            if (gxf_guarded) {
                hvf_gxf_sync_gl_to_live(env);
            }
        }

        if (getenv("INFERNO_DEBUG_SPRR") && isread && !sysreg_ret &&
            (reg == SYSREG(3, 6, 15, 1, 5) || reg == SYSREG(3, 6, 15, 3, 1))) {
            fprintf(stderr, "[sprr]   -> read value %#llx\n",
                    (unsigned long long)val);
        }
        if (isread && !sysreg_ret) {
            trace_hvf_sysreg_read(reg,
                                  SYSREG_OP0(reg),
                                  SYSREG_OP1(reg),
                                  SYSREG_CRN(reg),
                                  SYSREG_CRM(reg),
                                  SYSREG_OP2(reg),
                                  val);
            hvf_set_reg(cpu, rt, val);
        }

        advance_pc = !sysreg_ret;
        break;
    }
    case EC_WFX_TRAP:
        advance_pc = true;
        if (!(syndrome & WFX_IS_WFE)) {
            hvf_wfi(cpu);
        }
        break;
    case EC_AA64_HVC:
        if (hvf_gxf_fast_enabled() && !cpu->vcpu_dirty &&
            arm_feature(env, ARM_FEATURE_GXF) &&
            (GXF_HVC_IMM_IS_GENTER(syndrome & 0xffff) ||
             GXF_HVC_IMM_IS_GEXIT(syndrome & 0xffff))) {
            bool genter = GXF_HVC_IMM_IS_GENTER(syndrome & 0xffff);

            hvf_gxf_fast_sync_in(cpu);
            if (hvf_gxf_fast_ok(cpu, genter)) {
                if (genter) {
                    hvf_gxf_enter(cpu, GXF_INSN_IMM(syndrome));
                } else {
                    hvf_gxf_exit(cpu);
                }
                hvf_gxf_fast_sync_out(cpu);
                break;
            }
            /* Not a clean transition -- fall back to the full sync below. */
        }
        cpu_synchronize_state(cpu);
        /* INFERNO_GXF_COUNT: how hot is the rewritten GENTER/GEXIT path? Each
         * one costs a full cpu_synchronize_state() in and a full register
         * writeback out, so the rate decides whether that is worth trimming. */
        if (GXF_HVC_IMM_IS_GENTER(syndrome & 0xffff) ||
            GXF_HVC_IMM_IS_GEXIT(syndrome & 0xffff)) {
            static uint64_t inferno_gxf_traps;
            static int inferno_gxf_report;
            if (++inferno_gxf_traps % 200000 == 0 && getenv("INFERNO_GXF_COUNT")) {
                fprintf(stderr, "INFERNO: %llu GXF traps\n",
                        (unsigned long long)inferno_gxf_traps);
                inferno_gxf_report++;
            }
        }
        if (GXF_HVC_IMM_IS_XPRR(syndrome & 0xffff)) {
            /* Rewritten pmap_set_pte_xprr_perm(), see kernel_patches.c */
            hvf_xprr_set_pte(cpu);
            cpu->vcpu_dirty = true;
            break;
        }
        if (arm_feature(env, ARM_FEATURE_GXF) &&
            GXF_HVC_IMM_IS_GENTER(syndrome & 0xffff)) {
            /* Rewritten GENTER, see hw/arm/apple-silicon/kernel_patches.c */
            hvf_gxf_enter(cpu, GXF_INSN_IMM(syndrome));
            cpu->vcpu_dirty = true;
            break;
        }
        if (arm_feature(env, ARM_FEATURE_GXF) &&
            GXF_HVC_IMM_IS_GEXIT(syndrome & 0xffff)) {
            hvf_gxf_exit(cpu);
            cpu->vcpu_dirty = true;
            break;
        }
        if (arm_cpu->psci_conduit == QEMU_PSCI_CONDUIT_HVC) {
            /* Do NOT advance $pc for HVC */
            if (!hvf_handle_psci_call(cpu)) {
                trace_hvf_unknown_hvc(env->pc, env->xregs[0]);
                /* SMCCC 1.3 section 5.2 says every unknown SMCCC call returns -1 */
                env->xregs[0] = -1;
            }
            cpu->vcpu_dirty = true;
        } else {
            trace_hvf_unknown_hvc(env->pc, env->xregs[0]);
            hvf_raise_exception(cpu, EXCP_UDEF, syn_uncategorized(), 1);
        }
        break;
    case EC_AA64_SMC:
        cpu_synchronize_state(cpu);
        if (arm_cpu->psci_conduit == QEMU_PSCI_CONDUIT_SMC) {
            /* Secure Monitor Call exception, we need to advance $pc */
            advance_pc = true;

            if (!hvf_handle_psci_call(cpu)) {
                trace_hvf_unknown_smc(env->xregs[0]);
                /* SMCCC 1.3 section 5.2 says every unknown SMCCC call returns -1 */
                env->xregs[0] = -1;
            }
            cpu->vcpu_dirty = true;
        } else {
            trace_hvf_unknown_smc(env->xregs[0]);
            hvf_raise_exception(cpu, EXCP_UDEF, syn_uncategorized(), 1);
        }
        break;
    case EC_INSNABORT: {
        uint32_t set = (syndrome >> 12) & 3;
        bool fnv = (syndrome >> 10) & 1;
        bool ea = (syndrome >> 9) & 1;
        bool s1ptw = (syndrome >> 7) & 1;
        uint32_t ifsc = (syndrome >> 0) & 0x3f;

        trace_hvf_insn_abort(env->pc, set, fnv, ea, s1ptw, ifsc);

        /* fall through */
    }
    default:
        cpu_synchronize_state(cpu);
        trace_hvf_exit(syndrome, ec, env->pc);
        error_report("0x%llx: unhandled exception ec=0x%x", env->pc, ec);
    }

    /* flush any changed cpu state back to HVF */
    flush_cpu_state(cpu);

    if (advance_pc) {
        uint64_t pc;

        r = hv_vcpu_get_reg(cpu->accel->fd, HV_REG_PC, &pc);
        assert_hvf_ok(r);
        pc += 4;
        r = hv_vcpu_set_reg(cpu->accel->fd, HV_REG_PC, pc);
        assert_hvf_ok(r);

        /* Handle single-stepping over instructions which trigger a VM exit */
        if (cpu->singlestep_enabled) {
            ret = EXCP_DEBUG;
        }
    }

    return ret;
}

static int hvf_handle_vmexit(CPUState *cpu, hv_vcpu_exit_t *exit)
{
    ARMCPU *arm_cpu = env_archcpu(cpu_env(cpu));
    int ret = 0;

    switch (exit->reason) {
    case HV_EXIT_REASON_EXCEPTION:
        hvf_sync_vtimer(cpu);
        ret = hvf_handle_exception(cpu, &exit->exception);
        break;
    case HV_EXIT_REASON_VTIMER_ACTIVATED:
        hvf_ev(cpu, HVF_EV_VT_ACTIVE, 0);
        qemu_set_irq(arm_cpu->gt_timer_outputs[GTIMER_VIRT], 1);
        cpu->accel->vtimer_masked = true;
        break;
    case HV_EXIT_REASON_CANCELED:
        /* we got kicked, no exit to process */
        ret = -1;
        break;
    default:
        assert_not_reached();
    }

    return ret;
}


/* Registers that hvf_sysreg_read/write answer from `env` alone, with no device
 * or timer state behind them, so no BQL is needed: Apple's APCTL_EL1 and the
 * op1=6 CRN=15 block (GXF/GL11/SPRR). */
static bool hvf_sysreg_is_cpu_local(uint32_t reg)
{
    if (reg == SYSREG(3, 4, 15, 0, 4)) {
        return true;                                    /* APCTL_EL1 */
    }
    return SYSREG_OP0(reg) == 3 && SYSREG_OP1(reg) == 6 &&
           SYSREG_CRN(reg) == 15;
}

#define HVF_MAX_UNLOCKED_EXITS 256

/*
 * True once an interrupt hvf_inject_interrupts() held back for guarded mode
 * can finally be delivered. GEXIT is handled inside the unlocked loop when the
 * GXF fast path is on, so without this the loop could keep re-entering the
 * guest for up to HVF_MAX_UNLOCKED_EXITS before returning to the injection
 * point at the top of hvf_arch_vcpu_exec().
 */
static bool hvf_gxf_irq_releasable(CPUState *cpu)
{
    return hvf_gxf_defer_irq() &&
           !hvf_gxf_is_guarded(&ARM_CPU(cpu)->env) &&
           (cpu_test_interrupt(cpu, CPU_INTERRUPT_FIQ) ||
            cpu_test_interrupt(cpu, CPU_INTERRUPT_HARD));
}

static bool hvf_handle_sysreg_unlocked(CPUState *cpu)
{
    static int8_t enabled = -1;
    hv_vcpu_exit_t *exit = cpu->accel->exit;
    uint64_t syndrome;
    uint32_t ec, reg;

    if (enabled < 0) {
        const char *e = getenv("INFERNO_HVF_FAST_SYSREG");
        enabled = (e == NULL || atoi(e) != 0) ? 1 : 0;
    }
    if (!enabled || exit->reason != HV_EXIT_REASON_EXCEPTION) {
        return false;
    }

    syndrome = exit->exception.syndrome;
    ec = syn_get_ec(syndrome);

    /*
     * GXF transitions are the other half of the exit budget (2.4M per boot),
     * and hvf_gxf_enter()/hvf_gxf_exit() only move state around inside `env`
     * -- so long as we sync just the registers they touch rather than reaching
     * for cpu_synchronize_state(), which would need the lock.
     */
    if (ec == EC_AA64_HVC && hvf_gxf_fast_enabled() && !cpu->vcpu_dirty &&
        arm_feature(cpu_env(cpu), ARM_FEATURE_GXF) &&
        (GXF_HVC_IMM_IS_GENTER(syndrome & 0xffff) ||
         GXF_HVC_IMM_IS_GEXIT(syndrome & 0xffff))) {
        bool genter = GXF_HVC_IMM_IS_GENTER(syndrome & 0xffff);

        hvf_gxf_fast_sync_in(cpu);
        if (!hvf_gxf_fast_ok(cpu, genter)) {
            return false;       /* let the locked path sort it out */
        }
        hvf_account_exit(ec, syndrome);
        if (genter) {
            hvf_gxf_enter(cpu, GXF_INSN_IMM(syndrome));
        } else {
            hvf_gxf_exit(cpu);
        }
        hvf_gxf_fast_sync_out(cpu);
        return true;
    }

    if (ec != EC_SYSTEMREGISTERTRAP) {
        return false;
    }
    reg = syndrome & SYSREG_MASK;
    if (!hvf_sysreg_is_cpu_local(reg)) {
        return false;
    }

    return hvf_handle_exception(cpu, &exit->exception) == 0;
}

int hvf_arch_vcpu_exec(CPUState *cpu)
{
    int ret;
    hv_return_t r;

    if (cpu->halted) {
        return EXCP_HLT;
    }

    flush_cpu_state(cpu);

    do {
        int fast;

        if (!(cpu->singlestep_enabled & SSTEP_NOIRQ) &&
            hvf_inject_interrupts(cpu)) {
            return EXCP_INTERRUPT;
        }

        /*
         * Apple's implementation-defined registers are the hot exits by a wide
         * margin -- APCTL_EL1 alone is ~42% of them, and the GXF banked
         * registers most of the rest -- and they are pure per-CPU state that
         * hvf_sysreg_read/write serve straight out of `env`. Taking the BQL for
         * those serialises all six vCPUs on one mutex: a boot profile showed
         * the vCPU threads spending ~61% of their time in bql_lock_impl and
         * only ~14% inside hv_vcpu_run. Re-enter the guest for those without
         * ever taking the lock, but cap the run so interrupt injection at the
         * top of the outer loop still happens promptly.
         */
        bql_unlock();
        for (fast = 0; ; fast++) {
            cpu_exec_start(cpu);
            r = hv_vcpu_run(cpu->accel->fd);
            cpu_exec_end(cpu);
            if (r != HV_SUCCESS || fast >= HVF_MAX_UNLOCKED_EXITS ||
                hvf_gxf_irq_releasable(cpu) ||
                !hvf_handle_sysreg_unlocked(cpu)) {
                break;
            }
        }
        bql_lock();
        switch (r) {
        case HV_SUCCESS:
            ret = hvf_handle_vmexit(cpu, cpu->accel->exit);
            break;
        case HV_ILLEGAL_GUEST_STATE:
            trace_hvf_illegal_guest_state();
            /* fall through */
        default:
            assert_not_reached();
        }
    } while (ret == 0);

    return ret;
}

static const VMStateDescription vmstate_hvf_vtimer = {
    .name = "hvf-vtimer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(vtimer_val, HVFVTimer),
        VMSTATE_END_OF_LIST()
    },
};

static void hvf_vm_state_change(void *opaque, bool running, RunState state)
{
    HVFVTimer *s = opaque;

    if (running) {
        /* Update vtimer offset on all CPUs */
        hvf_state->vtimer_offset = mach_absolute_time() - s->vtimer_val;
        cpu_synchronize_all_states();
    } else {
        /* Remember vtimer value on every pause */
        s->vtimer_val = hvf_vtimer_val_raw();
    }
}

/*
 * Hang watchdog.
 *
 * A vCPU that takes a synchronous exception while in guarded mode (GXF) and
 * already on SP_EL1 vectors to the "Current EL with SPx" entry of VBAR_GL1,
 * which XNU fills with an unconditional `B .`. The vCPU then spins at 100%
 * without ever exiting to the hypervisor, so nothing in QEMU notices -- the
 * boot simply stops. INFERNO_HVF_HANG_WATCH=<seconds> samples every vCPU's PC
 * once a second and, when one has not moved for that long, dumps the state
 * that identifies the faulting access (ESR/FAR/ELR are still the ones from the
 * exception that landed there).
 */
static QEMUTimer *hvf_hang_timer;
static int hvf_hang_secs;

/*
 * Walk the guest's TTBR1 stage-1 tables by hand and print every descriptor on
 * the way down. The faulting address comes back as a permission fault, so the
 * mapping exists -- what matters is the AP/PXN/UXN bits of the leaf and how
 * SPRR remaps them.
 */
static void hvf_hang_walk(CPUState *cpu, uint64_t va)
{
    CPUARMState *env = &ARM_CPU(cpu)->env;
    uint64_t tcr = env->cp15.tcr_el[1];
    uint64_t desc, pa;
    unsigned tg1 = extract64(tcr, 30, 2);
    unsigned t1sz = extract64(tcr, 16, 6);
    unsigned page_bits, stride, va_bits, bits;
    int level;

    switch (tg1) {
    case 1: page_bits = 14; break;      /* 16K */
    case 2: page_bits = 12; break;      /* 4K  */
    case 3: page_bits = 16; break;      /* 64K */
    default:
        fprintf(stderr, "  ptw: unexpected TG1=%u\n", tg1);
        return;
    }
    stride = page_bits - 3;
    va_bits = 64 - t1sz;
    level = 3;
    bits = page_bits;
    while (bits + stride < va_bits) {
        bits += stride;
        level--;
    }

    pa = env->cp15.ttbr1_el[1] & MAKE_64BIT_MASK(1, 47);
    pa &= ~MAKE_64BIT_MASK(0, page_bits > 12 ? 4 : 0);
    fprintf(stderr, "  ptw: va=%#018" PRIx64 " tcr=%#018" PRIx64
            " tg1=%u(%uK) t1sz=%u start_level=%d ttbr1=%#018" PRIx64 "\n",
            va, tcr, tg1, 1u << (page_bits - 10), t1sz, level,
            env->cp15.ttbr1_el[1]);

    for (; level <= 3; level++) {
        unsigned shift = page_bits + stride * (3 - level);
        unsigned idx_bits = (level == 3) ? stride :
                            MIN(stride, va_bits - shift);
        uint64_t idx = extract64(va, shift, idx_bits);

        desc = address_space_ldq(&address_space_memory, pa + idx * 8,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
        fprintf(stderr, "  ptw: L%d table=%#018" PRIx64 " idx=%#" PRIx64
                " desc=%#018" PRIx64 "%s\n", level, pa, idx, desc,
                (desc & 1) ? "" : "  <INVALID>");
        if (!(desc & 1)) {
            return;
        }
        if (level < 3 && (desc & 3) == 1) {
            fprintf(stderr, "  ptw: L%d is a block\n", level);
            break;
        }
        pa = desc & MAKE_64BIT_MASK(page_bits, 48 - page_bits);
    }

    fprintf(stderr, "  ptw: leaf ap=%u sh=%u af=%u ns=%u attridx=%u "
            "pxn=%u uxn=%u nG=%u\n",
            (unsigned)extract64(desc, 6, 2), (unsigned)extract64(desc, 8, 2),
            (unsigned)extract64(desc, 10, 1), (unsigned)extract64(desc, 5, 1),
            (unsigned)extract64(desc, 2, 3), (unsigned)extract64(desc, 53, 1),
            (unsigned)extract64(desc, 54, 1), (unsigned)extract64(desc, 11, 1));
    fprintf(stderr, "  sprr: config_el1=%#018" PRIx64
            " perm_el1=%#018" PRIx64 " perm_el0=%#018" PRIx64 "\n",
            env->sprr.sprr_config_el[1], env->sprr.sprr_el_br_el1[1][1],
            env->sprr.sprr_el_br_el1[1][0]);
}

static void hvf_hang_dump(CPUState *cpu)
{
    CPUARMState *env = &ARM_CPU(cpu)->env;
    uint32_t esr = env->cp15.esr_el[1];

    fprintf(stderr,
            "hvf-hang: cpu%d stuck %ds at pc=%#018" PRIx64 "\n"
            "  pstate=%#010x el=%d spsel=%d guarded=%d\n"
            "  esr_el1=%#010x ec=%#04x iss=%#08x far_el1=%#018" PRIx64 "\n"
            "  elr_el1=%#018" PRIx64 " spsr_el1=%#010x vbar_el1=%#018" PRIx64
            "\n"
            "  sp=%#018" PRIx64 " sp_el0=%#018" PRIx64 " sp_el1=%#018" PRIx64
            "\n"
            "  gl1: vbar=%#018" PRIx64 " sp=%#018" PRIx64 " elr=%#018" PRIx64
            " esr=%#010x far=%#018" PRIx64 "\n"
            "  x30=%#018" PRIx64 " x0=%#018" PRIx64 " x1=%#018" PRIx64 "\n",
            cpu->cpu_index, hvf_hang_secs, env->pc,
            pstate_read(env), arm_current_el(env),
            (int)!!(env->pstate & PSTATE_SP), (int)hvf_gxf_is_guarded(env),
            esr, esr >> 26, esr & 0x1ffffff, env->cp15.far_el[1],
            env->elr_el[1], env->banked_spsr[BANK_SVC], env->cp15.vbar_el[1],
            env->xregs[31], env->sp_el[0], env->sp_el[1],
            env->gxf.vbar_gl[1], env->gxf.sp_gl[1], env->gxf.elr_gl[1],
            (uint32_t)env->gxf.esr_gl[1], env->gxf.far_gl[1],
            env->xregs[30], env->xregs[0], env->xregs[1]);

    fprintf(stderr, "  sctlr_el1=%#018" PRIx64 " (A=%d SA=%d SA0=%d) "
            "mair_el1=%#018" PRIx64 " tcr=%#018" PRIx64 "\n",
            env->cp15.sctlr_el[1],
            (int)extract64(env->cp15.sctlr_el[1], 1, 1),
            (int)extract64(env->cp15.sctlr_el[1], 3, 1),
            (int)extract64(env->cp15.sctlr_el[1], 4, 1),
            env->cp15.mair_el[1], env->cp15.tcr_el[1]);
    fprintf(stderr, "  gxf: enter_el1=%#018" PRIx64 " status=%#018" PRIx64
            " config=%#018" PRIx64 " irq_deferred=%" PRIu64 "\n",
            env->gxf.gxf_enter_el[1], env->gxf.gxf_status_el[1],
            env->gxf.gxf_config_el[1], cpu->accel->gxf_irq_deferred);

    if ((esr >> 26) == 0x21 || (esr >> 26) == 0x25) {
        hvf_hang_walk(cpu, env->cp15.far_el[1]);
    }
    /*
     * When the GL1 vector handler has run far enough to build an exception
     * frame, the frame holds the *original* exception -- the one that started
     * the whole chain and which is otherwise overwritten by everything that
     * happens afterwards. Layout recovered from the handler at VBAR_GL1+0x1000:
     * ELR at +0x108, SPSR +0x110, FAR +0x118, ESR +0x120.
     */
    if ((esr >> 26) == 0x25 && env->cp15.far_el[1] > 0xffffff0000000000ULL) {
        uint64_t frame = env->cp15.far_el[1] - 8;
        uint64_t f_elr = 0, f_far = 0;
        uint32_t f_spsr = 0, f_esr = 0;

        if (cpu_memory_rw_debug(cpu, frame + 0x108, (uint8_t *)&f_elr, 8, 0) == 0 &&
            cpu_memory_rw_debug(cpu, frame + 0x110, (uint8_t *)&f_spsr, 4, 0) == 0 &&
            cpu_memory_rw_debug(cpu, frame + 0x118, (uint8_t *)&f_far, 8, 0) == 0 &&
            cpu_memory_rw_debug(cpu, frame + 0x120, (uint8_t *)&f_esr, 4, 0) == 0) {
            f_elr = le64_to_cpu(f_elr);
            f_far = le64_to_cpu(f_far);
            f_spsr = le32_to_cpu(f_spsr);
            f_esr = le32_to_cpu(f_esr);
            fprintf(stderr, "  saved GL1 frame @%#" PRIx64 ": elr=%#018" PRIx64
                    " spsr=%#010x far=%#018" PRIx64 " esr=%#010x "
                    "(ec=%#04x iss=%#08x)\n", frame, f_elr, f_spsr, f_far,
                    f_esr, f_esr >> 26, f_esr & 0x1ffffff);
        }
    }

    hvf_ev_dump(cpu);
    if (hvf_gxf_is_guarded(env)) {
        fprintf(stderr, "  -- guarded entry point --\n");
        hvf_hang_walk(cpu, env->gxf.gxf_enter_el[1]);
        fprintf(stderr, "  -- ppl_dispatch caller (x30) --\n");
        hvf_hang_walk(cpu, env->xregs[30]);
    }
}

static void hvf_hang_tick(void *opaque)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        AccelCPUState *acc = cpu->accel;
        CPUARMState *env = &ARM_CPU(cpu)->env;

        cpu_synchronize_state(cpu);
        if (env->pc == acc->hang_last_pc) {
            if (++acc->hang_ticks == hvf_hang_secs) {
                hvf_hang_dump(cpu);
            }
        } else {
            acc->hang_last_pc = env->pc;
            acc->hang_ticks = 0;
        }
    }
    timer_mod(hvf_hang_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
}

int hvf_arch_init(void)
{
    const char *hang = getenv("INFERNO_HVF_HANG_WATCH");

    if (hang) {
        hvf_hang_secs = atoi(hang);
        hvf_ev_on = hvf_hang_secs > 0;
        if (hvf_hang_secs > 0) {
            hvf_hang_timer = timer_new_ms(QEMU_CLOCK_REALTIME, hvf_hang_tick,
                                          NULL);
            timer_mod(hvf_hang_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000);
        }
    }

    hvf_state->vtimer_offset = mach_absolute_time();
    vmstate_register(NULL, 0, &vmstate_hvf_vtimer, &vtimer);
    qemu_add_vm_change_state_handler(hvf_vm_state_change, &vtimer);

    hvf_arm_init_debug();

    return 0;
}

static const uint32_t brk_insn = 0xd4200000;

int hvf_arch_insert_sw_breakpoint(CPUState *cpu, struct hvf_sw_breakpoint *bp)
{
    if (cpu_memory_rw_debug(cpu, bp->pc, (uint8_t *)&bp->saved_insn, 4, 0) ||
        cpu_memory_rw_debug(cpu, bp->pc, (uint8_t *)&brk_insn, 4, 1)) {
        return -EINVAL;
    }
    return 0;
}

int hvf_arch_remove_sw_breakpoint(CPUState *cpu, struct hvf_sw_breakpoint *bp)
{
    static uint32_t brk;

    if (cpu_memory_rw_debug(cpu, bp->pc, (uint8_t *)&brk, 4, 0) ||
        brk != brk_insn ||
        cpu_memory_rw_debug(cpu, bp->pc, (uint8_t *)&bp->saved_insn, 4, 1)) {
        return -EINVAL;
    }
    return 0;
}

int hvf_arch_insert_hw_breakpoint(vaddr addr, vaddr len, int type)
{
    switch (type) {
    case GDB_BREAKPOINT_HW:
        return insert_hw_breakpoint(addr);
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_ACCESS:
        return insert_hw_watchpoint(addr, len, type);
    default:
        return -ENOSYS;
    }
}

int hvf_arch_remove_hw_breakpoint(vaddr addr, vaddr len, int type)
{
    switch (type) {
    case GDB_BREAKPOINT_HW:
        return delete_hw_breakpoint(addr);
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_ACCESS:
        return delete_hw_watchpoint(addr, len, type);
    default:
        return -ENOSYS;
    }
}

void hvf_arch_remove_all_hw_breakpoints(void)
{
    if (cur_hw_wps > 0) {
        g_array_remove_range(hw_watchpoints, 0, cur_hw_wps);
    }
    if (cur_hw_bps > 0) {
        g_array_remove_range(hw_breakpoints, 0, cur_hw_bps);
    }
}

/*
 * Update the vCPU with the gdbstub's view of debug registers. This view
 * consists of all hardware breakpoints and watchpoints inserted so far while
 * debugging the guest.
 * Must be called by the owning thread.
 */
static void hvf_put_gdbstub_debug_registers(CPUState *cpu)
{
    hv_return_t r = HV_SUCCESS;
    int i;

    for (i = 0; i < cur_hw_bps; i++) {
        HWBreakpoint *bp = get_hw_bp(i);
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgbcr_regs[i], bp->bcr);
        assert_hvf_ok(r);
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgbvr_regs[i], bp->bvr);
        assert_hvf_ok(r);
    }
    for (i = cur_hw_bps; i < max_hw_bps; i++) {
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgbcr_regs[i], 0);
        assert_hvf_ok(r);
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgbvr_regs[i], 0);
        assert_hvf_ok(r);
    }

    for (i = 0; i < cur_hw_wps; i++) {
        HWWatchpoint *wp = get_hw_wp(i);
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgwcr_regs[i], wp->wcr);
        assert_hvf_ok(r);
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgwvr_regs[i], wp->wvr);
        assert_hvf_ok(r);
    }
    for (i = cur_hw_wps; i < max_hw_wps; i++) {
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgwcr_regs[i], 0);
        assert_hvf_ok(r);
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgwvr_regs[i], 0);
        assert_hvf_ok(r);
    }
}

/*
 * Update the vCPU with the guest's view of debug registers. This view is kept
 * in the environment at all times.
 * Must be called by the owning thread.
 */
static void hvf_put_guest_debug_registers(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    hv_return_t r = HV_SUCCESS;
    int i;

    for (i = 0; i < max_hw_bps; i++) {
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgbcr_regs[i],
                                env->cp15.dbgbcr[i]);
        assert_hvf_ok(r);
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgbvr_regs[i],
                                env->cp15.dbgbvr[i]);
        assert_hvf_ok(r);
    }

    for (i = 0; i < max_hw_wps; i++) {
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgwcr_regs[i],
                                env->cp15.dbgwcr[i]);
        assert_hvf_ok(r);
        r = hv_vcpu_set_sys_reg(cpu->accel->fd, dbgwvr_regs[i],
                                env->cp15.dbgwvr[i]);
        assert_hvf_ok(r);
    }
}

static inline bool hvf_arm_hw_debug_active(CPUState *cpu)
{
    return ((cur_hw_wps > 0) || (cur_hw_bps > 0));
}

/* Must be called by the owning thread */
static void hvf_arch_set_traps(CPUState *cpu)
{
    bool should_enable_traps = false;
    hv_return_t r = HV_SUCCESS;

    /* Check whether guest debugging is enabled for at least one vCPU; if it
     * is, enable exiting the guest on all vCPUs */
    should_enable_traps |= cpu->accel->guest_debug_enabled;
    /* Set whether debug exceptions exit the guest */
    r = hv_vcpu_set_trap_debug_exceptions(cpu->accel->fd,
                                            should_enable_traps);
    assert_hvf_ok(r);

    /* Set whether accesses to debug registers exit the guest */
    r = hv_vcpu_set_trap_debug_reg_accesses(cpu->accel->fd,
                                            should_enable_traps);
    assert_hvf_ok(r);
}

void hvf_arch_update_guest_debug(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;

    /* Check whether guest debugging is enabled */
    cpu->accel->guest_debug_enabled = cpu->singlestep_enabled ||
                                    hvf_sw_breakpoints_active(cpu) ||
                                    hvf_arm_hw_debug_active(cpu);

    /* Update debug registers */
    if (cpu->accel->guest_debug_enabled) {
        hvf_put_gdbstub_debug_registers(cpu);
    } else {
        hvf_put_guest_debug_registers(cpu);
    }

    cpu_synchronize_state(cpu);

    /* Enable/disable single-stepping */
    if (cpu->singlestep_enabled) {
        env->cp15.mdscr_el1 =
            deposit64(env->cp15.mdscr_el1, MDSCR_EL1_SS_SHIFT, 1, 1);
        pstate_write(env, pstate_read(env) | PSTATE_SS);
    } else {
        env->cp15.mdscr_el1 =
            deposit64(env->cp15.mdscr_el1, MDSCR_EL1_SS_SHIFT, 1, 0);
    }

    /* Enable/disable Breakpoint exceptions */
    if (hvf_arm_hw_debug_active(cpu)) {
        env->cp15.mdscr_el1 =
            deposit64(env->cp15.mdscr_el1, MDSCR_EL1_MDE_SHIFT, 1, 1);
    } else {
        env->cp15.mdscr_el1 =
            deposit64(env->cp15.mdscr_el1, MDSCR_EL1_MDE_SHIFT, 1, 0);
    }

    hvf_arch_set_traps(cpu);
}

bool hvf_arch_supports_guest_debug(void)
{
    return true;
}
