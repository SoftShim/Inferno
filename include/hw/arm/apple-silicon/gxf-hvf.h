/*
 * Apple GXF (Guarded Execution) support under Hypervisor.framework.
 *
 * Hypervisor.framework does not expose GXF (GENTER/GEXIT and the GL1
 * register bank) to guests. To run XNU kernels that use GXF for the PPL
 * under HVF, the kernel patcher rewrites every GENTER/GEXIT instruction
 * into an HVC with a magic immediate, and the HVF exit handler emulates
 * the GL1 register bank switch.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_ARM_APPLE_SILICON_GXF_HVF_H
#define HW_ARM_APPLE_SILICON_GXF_HVF_H

/*
 * Apple implementation-defined instruction encodings. The low 5 bits (Rd)
 * act as an immediate which the kernel reads back from ESR_GL1[4:0]
 * (XNU uses e.g. `genter #2`), so match with GXF_INSN_MASK and preserve it.
 */
#define GXF_INSN_MASK (0xFFFFFFE0u)
#define GXF_INSN_GENTER (0x00201420u)
#define GXF_INSN_GEXIT (0x00201400u)
#define GXF_INSN_IMM(_insn) ((_insn) & 0x1Fu)

/*
 * HVC immediates used for the trap-and-emulate rewrite:
 * 0x47E0 | imm5 for GENTER, 0x47C0 | imm5 for GEXIT.
 */
#define GXF_HVC_IMM_MASK (0xFFE0u)
#define GXF_HVC_IMM_GENTER (0x47E0u)
#define GXF_HVC_IMM_GEXIT (0x47C0u)
#define GXF_HVC_IMM_IS_GENTER(_imm) (((_imm) & GXF_HVC_IMM_MASK) == GXF_HVC_IMM_GENTER)
#define GXF_HVC_IMM_IS_GEXIT(_imm) (((_imm) & GXF_HVC_IMM_MASK) == GXF_HVC_IMM_GEXIT)

/*
 * 0x47A0 replaces the first instruction of pmap_set_pte_xprr_perm(), so QEMU
 * performs the XPRR class change itself and can substitute a class that is
 * permissive outside guarded mode. See hvf_xprr_set_pte() in
 * target/arm/hvf/hvf.c and ck_kp_hvf_ppl_xprr_patch() in kernel_patches.c.
 */
#define GXF_HVC_IMM_XPRR (0x47A0u)
#define GXF_HVC_IMM_IS_XPRR(_imm) \
    (((_imm) & GXF_HVC_IMM_MASK) == GXF_HVC_IMM_XPRR)

/*
 * INFERNO_HVF_XPRR_REMAP=1 hands every XPRR class change to QEMU and re-tags
 * PPL's pages when the kernel tightens SPRR_PERM_EL1. That removes the GXF
 * vector wedge completely -- 0 of 5 wedges showed it, against all of them
 * before -- but a second, unrelated failure then dominates at a similar rate
 * (19/24, against 47/56 for the VBAR_GL1 re-tag alone), so it is off by
 * default. See inferno-gxf-vector-wedge in the project notes.
 */
static inline bool gxf_hvf_xprr_remap_enabled(void)
{
    const char *e = getenv("INFERNO_HVF_XPRR_REMAP");

    return e != NULL && atoi(e) != 0;
}

/* HVC #imm16 encoding. */
#define A64_HVC(_imm16) (0xD4000002u | (((_imm16) & 0xFFFFu) << 5))

#endif /* HW_ARM_APPLE_SILICON_GXF_HVF_H */
