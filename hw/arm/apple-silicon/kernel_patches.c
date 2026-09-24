/*
 * ChefKiss Kernel Patches.
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "hw/arm/apple-silicon/gxf-hvf.h"
#include "hw/arm/apple-silicon/kernel_patches.h"
#include "hw/arm/apple-silicon/patcher.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "system/hvf.h"

#define NOP (0xD503201F)
#define MOV_W0_0 (0x52800000)
#define NOP_BYTES 0x1F, 0x20, 0x03, 0xD5
#define RET (0xD65F03C0)
#define RETAB (0xD65F0FFF)
#define PACIBSP (0xD503237F)

static CKPatcherRange *ck_kp_range_from_va(const char *name, vaddr base,
                                           vaddr size)
{
    CKPatcherRange *range = g_new0(CKPatcherRange, 1);
    range->addr = base;
    range->length = size;
    range->ptr = apple_boot_va_to_ptr(base);
    range->name = name;
    return range;
}

static CKPatcherRange *ck_kp_find_section_range(MachoHeader64 *hdr,
                                                const char *segment,
                                                const char *section)
{
    MachoSection64 *sec;
    MachoSegmentCommand64 *seg;

    seg = apple_boot_get_segment(hdr, segment);
    if (seg == NULL) {
        return NULL;
    }

    sec = apple_boot_get_section(seg, section);
    return sec == NULL ? NULL :
                         ck_kp_range_from_va(segment, sec->addr, sec->size);
}

// TODO: Fix host endianness BE vs LE troubles in here.
static MachoHeader64 *ck_kp_find_image_header(MachoHeader64 *hdr,
                                              const char *bundle_id)
{
    g_autofree CKPatcherRange *kmod_info_range = NULL;
    g_autofree CKPatcherRange *kext_info_range = NULL;
    g_autofree CKPatcherRange *kmod_start_range = NULL;
    uint64_t *info;
    uint64_t *start;
    uint32_t count;
    uint32_t i;
    char kname[256] = { 0 };
    const char *prelinkinfo;
    const char *last_dict;

    if (hdr->file_type == MH_FILESET) {
        return apple_boot_get_fileset_header(hdr, bundle_id);
    }

    kmod_info_range =
        ck_kp_find_section_range(hdr, "__PRELINK_INFO", "__kmod_info");
    if (kmod_info_range == NULL) {
        kext_info_range =
            ck_kp_find_section_range(hdr, "__PRELINK_INFO", "__info");
        if (kext_info_range == NULL) {
            error_report("Unsupported XNU.");
            return NULL;
        }

        prelinkinfo =
            strstr((const char *)kext_info_range->ptr, "PrelinkInfoDictionary");
        last_dict = strstr(prelinkinfo, "<array>") + 7;
        while (last_dict) {
            const char *nested_dict;
            const char *ident;
            const char *end_dict = strstr(last_dict, "</dict>");
            if (!end_dict) {
                break;
            }

            nested_dict = strstr(last_dict + 1, "<dict>");
            while (nested_dict) {
                if (nested_dict > end_dict) {
                    break;
                }

                nested_dict = strstr(nested_dict + 1, "<dict>");
                end_dict = strstr(end_dict + 1, "</dict>");
            }

            ident = g_strstr_len(last_dict, end_dict - last_dict,
                                 "CFBundleIdentifier");
            if (ident != NULL) {
                const char *value = strstr(ident, "<string>");
                if (value != NULL) {
                    const char *value_end;

                    value += strlen("<string>");
                    value_end = strstr(value, "</string>");
                    if (value_end != NULL) {
                        memcpy(kname, value, value_end - value);
                        kname[value_end - value] = 0;
                        if (strcmp(kname, bundle_id) == 0) {
                            const char *addr =
                                g_strstr_len(last_dict, end_dict - last_dict,
                                             "_PrelinkExecutableLoadAddr");
                            if (addr != NULL) {
                                const char *avalue = strstr(addr, "<integer");
                                if (avalue != NULL) {
                                    avalue = strstr(avalue, ">");
                                    if (avalue != NULL) {
                                        return apple_boot_va_to_ptr(
                                            strtoull(++avalue, 0, 0));
                                    }
                                }
                            }
                        }
                    }
                }
            }

            last_dict = strstr(end_dict, "<dict>");
        }

        return NULL;
    }
    kmod_start_range =
        ck_kp_find_section_range(hdr, "__PRELINK_INFO", "__kmod_start");
    if (kmod_start_range != NULL) {
        info = (uint64_t *)kmod_info_range->ptr;
        start = (uint64_t *)kmod_start_range->ptr;
        count = kmod_info_range->length / 8;
        for (i = 0; i < count; i++) {
            const char *kext_name =
                (const char *)apple_boot_va_to_ptr(info[i]) + 0x10;
            if (strcmp(kext_name, bundle_id) == 0) {
                return apple_boot_va_to_ptr(start[i]);
            }
        }
    }

    return NULL;
}

static CKPatcherRange *ck_kp_find_image_text(MachoHeader64 *hdr,
                                             const char *bundle_id)
{
    hdr = ck_kp_find_image_header(hdr, bundle_id);
    return hdr == NULL ? NULL :
                         ck_kp_find_section_range(hdr, "__TEXT_EXEC", "__text");
}

static CKPatcherRange *ck_kp_get_kernel_section(MachoHeader64 *hdr,
                                                const char *segment,
                                                const char *section)
{
    if (hdr->file_type == MH_FILESET) {
        MachoHeader64 *kernel =
            ck_kp_find_image_header(hdr, "com.apple.kernel");
        return kernel == NULL ?
                   NULL :
                   ck_kp_find_section_range(kernel, segment, section);
    }

    return ck_kp_find_section_range(hdr, segment, section);
}

static bool ck_kp_root_auth_callback(void *ctx, uint8_t *buffer)
{
    void *func_start =
        ck_patcher_find_prev_insn(buffer, 30, PACIBSP, 0xFFFFFFFF, 0);
    if (func_start == NULL) {
        warn_report("%s: failed to find pacibsp, trying to find old logic",
                    __func__);
        void *ret = ck_patcher_find_next_insn(buffer, 4, RET, 0xFFFFFFFF, 0);
        if (ret == NULL) {
            error_report("%s: neither variants matched", __func__);
            return false;
        }
        stl_le_p(ret - 4, MOV_W0_0);
        return true;
    }
    stl_le_p(func_start, MOV_W0_0);
    stl_le_p(func_start + 4, RET);

    return true;
}

static bool ck_kp_root_hash_callback(void *ctx, uint8_t *buffer)
{
    void *func_start =
        ck_patcher_find_prev_insn(buffer, 0x40, PACIBSP, 0xFFFFFFFF, 0);
    if (func_start == NULL) {
        error_report("%s: failed to find pacibsp", __func__);
        return false;
    }
    stl_le_p(func_start, MOV_W0_0);
    stl_le_p(func_start + 4, RET);

    return true;
}

static void ck_kp_apfs_patches(CKPatcherRange *range)
{
    static const uint8_t root_auth_pattern[] = {
        0x08, 0xE0, 0x40, 0x39, // ldrb w8, [x?, #0x38]
        0x08, 0x00, 0x00, 0x37, // tbnz w8, #0x5, #?
        0x00, 0x0A, 0x80, 0x52, // mov w?, #0x50
    };
    static const uint8_t root_auth_mask[] = { 0x1F, 0xFC, 0xFF, 0xFF,
                                              0x1F, 0x00, 0x00, 0xFF,
                                              0xE0, 0xFF, 0xFF, 0xFF };
    QEMU_BUILD_BUG_ON(sizeof(root_auth_pattern) != sizeof(root_auth_mask));
    ck_patcher_find_callback(
        range, "bypass root authentication", root_auth_pattern, root_auth_mask,
        sizeof(root_auth_pattern), sizeof(uint32_t), ck_kp_root_auth_callback);

    static const uint8_t root_rw_pattern[] = {
        0x00, 0x00, 0x00, 0x94, // bl ?
        0x00, 0x00, 0x70, 0x37, // tbnz w0, 0xE, ?
        0xA0, 0x03, 0x40, 0xB9, // ldr x?, [x29/sp, ?]
        0x00, 0x78, 0x1F, 0x12, // and w?, w?, 0xFFFFFFFE
        0xA0, 0x03, 0x00, 0xB9, // str x?, [x29/sp, ?]
    };
    static const uint8_t root_rw_mask[] = {
        0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0xF8, 0xFF, 0xA0, 0x03,
        0xFE, 0xFF, 0x00, 0xFC, 0xFF, 0xFF, 0xA0, 0x03, 0xC0, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(root_rw_pattern) != sizeof(root_rw_mask));
    static const uint8_t root_rw_repl[] = { MOV_W0_0_BYTES };
    if (!ck_patcher_find_replace(range, "allow mounting root as R/W",
                                 root_rw_pattern, root_rw_mask,
                                 sizeof(root_rw_pattern), sizeof(uint32_t),
                                 root_rw_repl, NULL, 4, sizeof(root_rw_repl))) {
        static const uint8_t root_rw_pattern_new[] = {
            0x00, 0x00, 0x00, 0x94, // bl ?
            0x00, 0x00, 0x70, 0x37, // tbnz w0, 0xE, ?
            0x00, 0x00, 0x80, 0x52, // mov w0, #0
            00,   0x00, 0x00, 0x14, // bl ?
        };
        static const uint8_t root_rw_mask_new[] = {
            0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0xF8, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFC,
        };
        QEMU_BUILD_BUG_ON(sizeof(root_rw_pattern_new) !=
                          sizeof(root_rw_mask_new));
        ck_patcher_find_replace(range, "allow mounting root as R/W (new)",
                                root_rw_pattern_new, root_rw_mask_new,
                                sizeof(root_rw_pattern_new), sizeof(uint32_t),
                                root_rw_repl, NULL, 4, sizeof(root_rw_repl));
    }

    static const uint8_t root_hash_pattern[] = {
        0x88, 0x62, 0x40, 0xF9, // ldr x8, [x20, #0xC0]
        0x08, 0x89, 0x47, 0x79, // ldrh w8, [x8, #0x3C4]
        0x1F, 0x11, 0x00, 0x71, // cmp w8, #0x4
    };
    ck_patcher_find_callback(range, "bypass root hash authentication",
                             root_hash_pattern, NULL, sizeof(root_hash_pattern),
                             sizeof(uint32_t), ck_kp_root_hash_callback);
}

static bool ck_kp_tc_callback(void *ctx, uint8_t *buffer)
{
    if (((ldl_le_p(buffer - 4) & 0xFF000000) != 0x91000000) &&
        ((ldl_le_p(buffer - 8) & 0xFF000000) != 0x91000000)) {
        return false;
    }

    void *ldrb =
        ck_patcher_find_next_insn(buffer, 256, 0x39402C00, 0xFFFFFC00, 0);
    uint32_t cdhash_param = extract32(ldl_le_p(ldrb), 5, 5);
    void *frame;
    void *start = buffer;
    bool pac;

    frame = ck_patcher_find_prev_insn(buffer, 10, 0x910003FD, 0xFF8003FF, 0);
    if (frame == NULL) {
        info_report("%s: found AMFI (Leaf)", __func__);
    } else {
        info_report("%s: found AMFI (Routine)", __func__);
        start = ck_patcher_find_prev_insn(frame, 10, 0xA9A003E0, 0xFFE003E0, 0);
        if (start == NULL) {
            start =
                ck_patcher_find_prev_insn(frame, 10, 0xD10003FF, 0xFF8003FF, 0);
            if (start == NULL) {
                error_report("%s: failed to find AMFI start", __func__);
                return false;
            }
        }
    }

    pac = ck_patcher_find_prev_insn(start, 5, PACIBSP, 0xFFFFFFFF, 0) != NULL;
    switch (cdhash_param) {
    case 0: {
        // adrp x8, ?
        void *adrp =
            ck_patcher_find_prev_insn(start, 10, 0x90000008, 0x9F00001F, 0);
        if (adrp != NULL) {
            start = adrp;
        }
        stl_le_p(start, 0x52802020); // mov w0, 0x101
        stl_le_p(start + 4, (pac ? RETAB : RET));
        return true;
    }
    case 1:
        stl_le_p(start, 0x52800040); // mov w0, 2
        stl_le_p(start + 4, 0x39000040); // strb w0, [x2]
        stl_le_p(start + 8, 0x52800020); // mov w0, 1
        stl_le_p(start + 12, 0x39000060); // strb w0, [x3]
        stl_le_p(start + 16, 0x52800020); // mov w0, 1
        stl_le_p(start + 20, (pac ? RETAB : RET));
        return true;
    default:
        error_report("%s: found unexpected AMFI prototype: %u", __func__,
                     cdhash_param);
        break;
    }
    return false;
}

static bool ck_kp_tc_ios16_callback(void *ctx, uint8_t *buffer)
{
    void *start =
        ck_patcher_find_prev_insn(buffer, 100, PACIBSP, 0xFFFFFFFF, 0);

    if (start == NULL) {
        return false;
    }

    stl_le_p(start, 0x52802020); // mov w0, 0x101
    stl_le_p(start + 4, RET);

    return true;
}

static void ck_kp_tc_patch(CKPatcherRange *range)
{
    static const uint8_t pattern[] = {
        0x00, 0x02, 0x80, 0x52, // mov w?, 0x16
        0x00, 0x00, 0x00, 0xD3, // lsr ?
        0x00, 0x00, 0x00, 0x9B, // madd ?
    };
    static const uint8_t mask[] = { 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                                    0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    if (!ck_patcher_find_callback(range, "all binaries in trustcache", pattern,
                                  mask, sizeof(pattern), sizeof(uint32_t),
                                  ck_kp_tc_callback)) {
        static const uint8_t ios16_pattern[] = { 0xC0, 0xCF, 0x9D,
                                                 0xD2 }; // mov w?, 0xEE7E
        static const uint8_t io16_mask[] = { 0xC0, 0xFF, 0xFF, 0xFF };
        QEMU_BUILD_BUG_ON(sizeof(ios16_pattern) != sizeof(io16_mask));
        ck_patcher_find_callback(range, "all binaries in trustcache (iOS 16+)",
                                 ios16_pattern, io16_mask,
                                 sizeof(ios16_pattern), sizeof(uint32_t),
                                 ck_kp_tc_ios16_callback);
    }
}

static bool ck_kp_amfi_sha1_callback(void *ctx, uint8_t *buffer)
{
    void *cmp = ck_patcher_find_next_insn(buffer, 0x10, 0x7100081F, 0xFFFFFFFF,
                                          0); // cmp w0, 2

    if (cmp == NULL) {
        error_report("%s: failed to find cmp", __func__);
        return false;
    }

    stl_le_p(cmp, 0x6B00001F); // cmp w0, w0
    return true;
}

static bool ck_kp_amfi_tc_callback(void *ctx, uint8_t *buffer)
{
    void *start =
        ck_patcher_find_prev_insn(buffer, 0x20, PACIBSP, 0xFFFFFFFF, 0);
    if (start == NULL) {
        error_report("%s: failed to find start of function", __func__);
        return false;
    }

    stl_le_p(start, 0xD2800020); // mov x0, #1
    stl_le_p(start + 4, 0xB4000042); // cbz x2, #0x8
    stl_le_p(start + 8, 0xF9000040); // str x0, [x2]
    stl_le_p(start + 12, RET);
    return true;
}

static void ck_kp_amfi_patches(CKPatcherRange *range)
{
    static const uint8_t pattern[] = { 0x02, 0x00, 0xD0,
                                       0x36 }; // tbz w2, 0x1A, ?
    static const uint8_t mask[] = { 0x1F, 0x00, 0xF8, 0xFF };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    ck_patcher_find_callback(range, "allow SHA1 signatures in AMFI", pattern,
                             mask, sizeof(pattern), sizeof(uint32_t),
                             ck_kp_amfi_sha1_callback);

    static const uint8_t amfi_tc_cache_pattern[] = {
        0xE0, 0x03, 0x00, 0x91, // mov x0, sp
        0xE1, 0x03, 0x13, 0xAA, // mov x1, x19
        0x00, 0x00, 0x00, 0x94, // bl trustCacheQueryGetFlags
        0x9F, 0x02, 0x00, 0x71, // cmp w20, 0
        0xE0, 0x17, 0x9F, 0x1A, // cset w0, eq
    };
    static const uint8_t amfi_tc_cache_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
        0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(amfi_tc_cache_pattern) !=
                      sizeof(amfi_tc_cache_mask));
    ck_patcher_find_callback(range, "all binaries in TrustCache (AMFI)",
                             amfi_tc_cache_pattern, amfi_tc_cache_mask,
                             sizeof(amfi_tc_cache_pattern), sizeof(uint32_t),
                             ck_kp_amfi_tc_callback);
}

static bool ck_kp_mac_mount_callback(void *ctx, uint8_t *buffer)
{
    // Search for tbnz w?, 5, ?
    void *inst =
        ck_patcher_find_prev_insn(buffer, 0x40, 0x37280000, 0xFFFE0000, 0);
    if (inst == NULL) {
        inst =
            ck_patcher_find_next_insn(buffer, 0x40, 0x37280000, 0xFFFE0000, 0);
        if (inst == NULL) {
            error_report("%s: failed to find nop point", __func__);
            return false;
        }
    }

    // Allow MNT_UNION mounts
    stl_le_p(inst, NOP);

    // Search for ldrb w8, [x?, 0x71]
    inst = ck_patcher_find_prev_insn(buffer, 0x40, 0x3941C408, 0xFFFFFC1F, 0);
    if (inst == NULL) {
        // Search for the same, but forwards.
        inst =
            ck_patcher_find_next_insn(buffer, 0x40, 0x3941C408, 0xFFFFFC1F, 0);
        if (inst == NULL) {
            // Search for add x8, x8/16, #0x70
            inst = ck_patcher_find_prev_insn(buffer, 0x40, 0x9101C008,
                                             0xFFFFFCFF, 0);
            // Search for ldr w8, [x8, #0x1]
            if (inst != NULL && ldl_le_p(inst + 4) == 0x39400508) {
                inst += 4;
            } else {
                error_report("%s: failed to find xzr point", __func__);
                return false;
            }
        }
    }

    // Replace with a mov x8, xzr
    // This will bypass the (vp->v_mount->mnt_flag & MNT_ROOTFS) check
    stl_le_p(inst, 0xAA1F03E8);

    return true;
}

static void ck_kp_mac_mount_patch(CKPatcherRange *range)
{
    static const uint8_t pattern[] = {
        0xE9, 0x2F, 0x1F, 0x32, // orr w9, wzr, 0x1FFE
    };
    if (!ck_patcher_find_callback(
            range, "allow remounting rootfs, union mounts (old)", pattern, NULL,
            sizeof(pattern), sizeof(uint32_t), ck_kp_mac_mount_callback)) {
        static const uint8_t new_pattern[] = {
            0xC9, 0xFF, 0x83, 0x12, // movz w/x9, 0x1FFE/-0x1FFF
        };
        static const uint8_t new_mask[] = { 0xFF, 0xFF, 0xFF, 0x3F };
        QEMU_BUILD_BUG_ON(sizeof(new_pattern) != sizeof(new_mask));
        ck_patcher_find_callback(range,
                                 "allow remounting rootfs, union mounts (new)",
                                 new_pattern, new_mask, sizeof(new_pattern),
                                 sizeof(uint32_t), ck_kp_mac_mount_callback);
    }
}

static bool ck_kp_kprintf_callback(void *ctx, uint8_t *buffer)
{
    uint8_t *comparison = buffer + sizeof(uint32_t) * 3;
    uint32_t comparison_inst = ldl_le_p(comparison);
    // if cbnz
    if (comparison_inst & BIT32(24)) {
        stl_le_p(comparison, NOP);
    } else {
        // turn into unconditional branch
        stl_le_p(comparison, 0x14000000 | extract32(comparison_inst, 5, 19));
    }
    return true;
}

static void ck_kp_kprintf_patch(CKPatcherRange *range)
{
    static const uint8_t pattern[] = {
        0xAA, 0x43, 0x00, 0x91, // add x10, fp, #0x10
        0xEA, 0x07, 0x00, 0xF9, // str x10, [sp, #0x8]
        0x08, 0x00, 0x00, 0x2A, // orr w8, w?, w?
        0x08, 0x00, 0x00, 0x34, // cbz w8, #?
    };
    static const uint8_t mask[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0x1F, 0xFC, 0xE0, 0xFF,
                                    0x1F, 0x00, 0x00, 0xFE };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    if (!ck_patcher_find_callback(range, "force enable kprintf", pattern, mask,
                                  sizeof(pattern), sizeof(uint32_t),
                                  ck_kp_kprintf_callback)) {
        static const uint8_t pattern_new[] = {
            0x08, 0x01, 0x40, 0x39, // ldrb w8, [x8, #0x?]
            0x08, 0x00, 0x00, 0x36, // tbz w8, #0, #?
            0xA0, 0x43, 0x00, 0x91, // add x?, fp, #0x10
            0xE0, 0x17, 0x00, 0xF9, // str x?, [sp, #0x28]
            0xE0, 0xA3, 0x00, 0x91, // add x?, sp, #0x28
            0x00, 0x00, 0x00, 0x14, // b #?
        };
        static const uint8_t mask_new[] = {
            0xFF, 0x03, 0xC0, 0xFF, 0x1F, 0x00, 0x00, 0xFF,
            0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF,
            0xE0, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFC
        };
        QEMU_BUILD_BUG_ON(sizeof(pattern_new) != sizeof(mask_new));
        static const uint8_t repl_new[] = { NOP_BYTES, NOP_BYTES, NOP_BYTES,
                                            NOP_BYTES, NOP_BYTES, NOP_BYTES };
        ck_patcher_find_replace(range, "force enable kprintf (new)",
                                pattern_new, mask_new, sizeof(pattern_new),
                                sizeof(uint32_t), repl_new, NULL, 0,
                                sizeof(repl_new));
    }
}

// gAMXVersion seemingly unused, but removing it just in case.
// New: Used in iOS 17+ to set the cpu_capabilities bit.
static bool ck_kp_amx_common(uint8_t *buffer, bool newer)
{
    void *amx_ver_str = ck_patcher_find_prev_insn(
        buffer, newer ? 6 : 10, 0xB8000000, 0xFEC00000, newer ? 0 : 1);
    if (amx_ver_str == NULL) {
        error_report("%s: Failed to find store to gAMXVersion.", __func__);
        return false;
    }
    stl_le_p(amx_ver_str, NOP);

    return true;
}

static bool ck_kp_amx_callback(void *ctx, uint8_t *buffer)
{
    stl_le_p(buffer, 0x52810009); // mov w9, #0x800

    return ck_kp_amx_common(buffer, false);
}

static bool ck_kp_amx_new_callback(void *ctx, uint8_t *buffer)
{
    // remove AMX support bit from movk
    // movk w?, #0x100, lsl #0x10
    stl_le_p(buffer + 4, ldl_le_p(buffer + 4) & ~(0x800 << 5));

    return ck_kp_amx_common(buffer, false);
}

static bool ck_kp_amx_newer_callback(void *ctx, uint8_t *buffer)
{
    return ck_kp_amx_common(buffer, true);
}

// in _commpage_populate
static void ck_kp_amx_patch(CKPatcherRange *range)
{
    static const uint8_t pattern_new[] = {
        0x00, 0x90, 0x87, 0x52, // mov w?, #0x3C80
        0x00, 0x20, 0xA1, 0x72, // movk w?, #0x900, lsl #0x10
        0x00, 0x40, 0x00, 0x2A, // orr w?, w?, w?, lsl #0x10
    };
    static const uint8_t mask_new[] = { 0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF,
                                        0xFF, 0xFF, 0x00, 0xFC, 0xE0, 0xFF };
    QEMU_BUILD_BUG_ON(sizeof(pattern_new) != sizeof(mask_new));
    if (!ck_patcher_find_callback(range, "disable AMX (new)", pattern_new,
                                  mask_new, sizeof(pattern_new),
                                  sizeof(uint32_t), ck_kp_amx_new_callback)) {
        static const uint8_t pattern[] = {
            0xE9, 0x83, 0x05, 0x32, // mov w9, #0x8000800
            0x09, 0x00, 0x00, 0xAA, // orr x9, x?, x?
        };
        static const uint8_t mask[] = { 0xFF, 0xFF, 0xFF, 0xFF,
                                        0x1F, 0xFC, 0xE0, 0xFF };
        QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
        if (!ck_patcher_find_callback(range, "disable AMX", pattern, mask,
                                      sizeof(pattern), sizeof(uint32_t),
                                      ck_kp_amx_callback)) {
            static const uint8_t pattern_newer[] = {
                0x0A, 0xF1, 0x1C, 0xD5, // msr amx_config_el1, x?
                0xDF, 0x3F, 0x03, 0xD5, // isb
            };
            static const uint8_t mask_newer[] = { 0x0F, 0xFF, 0xFF, 0xFF,
                                                  0xFF, 0xFF, 0xFF, 0xFF };
            ck_patcher_find_callback(range, "disable AMX (newer)",
                                     pattern_newer, mask_newer,
                                     sizeof(pattern_newer), sizeof(uint32_t),
                                     ck_kp_amx_newer_callback);
        }
    }
}

/*
 * With the simulated SEP there is no data encryption, so APFS refuses to
 * create the Data volume: panic("unencrypted data volume is not allowed").
 * Neuter the `tbnz` that reaches that panic.
 */
/*
 * Without data protection (simulated SEP), APFS refuses to open files with a
 * protection class: "rejecting class open (class %u) because we're not content
 * protected", which wedges first boot of the installed OS. Let the check fall
 * through to the success path.
 */
static void ck_kp_apfs_allow_class_open(CKPatcherRange *range) G_GNUC_UNUSED;
static void ck_kp_apfs_allow_class_open(CKPatcherRange *range)
{
    // mov w9,#9 ; tst w8,w9 ; b.eq <reject> ; mov w0,#0
    static const uint8_t pattern[] = { 0x29, 0x01, 0x80, 0x52, 0x1f, 0x01,
                                       0x09, 0x6a, 0x20, 0x03, 0x00, 0x54,
                                       0x00, 0x00, 0x80, 0x52 };
    static const uint8_t mask[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0xff, 0xff, 0xff };
    static const uint8_t repl[] = { NOP_BYTES };
    static const uint8_t keep[] = { 0, 0, 0, 0 };

    if (range == NULL) {
        return;
    }
    ck_patcher_find_replace(range, "allow protection-class open", pattern,
                            mask, sizeof(pattern), sizeof(uint32_t), repl,
                            keep, 8, sizeof(repl));
}

static void ck_kp_apfs_allow_unencrypted_data(CKPatcherRange *range) G_GNUC_UNUSED;
static void ck_kp_apfs_allow_unencrypted_data(CKPatcherRange *range)
{
    // ldrh w9,[x8,#0x3c4] ; cmp w9,#0x40 ; b.ne ret ; ldrb w8,[x8,#0x108] ;
    // tbnz w8,#0, panic
    static const uint8_t pattern[] = { 0x09, 0x89, 0x47, 0x79, 0x3f, 0x01,
                                       0x01, 0x71, 0x61, 0x00, 0x00, 0x54,
                                       0x08, 0x21, 0x44, 0x39, 0xa8, 0x00,
                                       0x00, 0x37 };
    static const uint8_t mask[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0xff };
    static const uint8_t repl[] = { NOP_BYTES };
    static const uint8_t keep[] = { 0, 0, 0, 0 };

    if (range == NULL) {
        return;
    }
    ck_patcher_find_replace(range, "allow unencrypted data volume", pattern,
                            mask, sizeof(pattern), sizeof(uint32_t), repl,
                            keep, 16, sizeof(repl));
}

static void ck_kp_apfs_snapshot_patch(CKPatcherRange *range)
{
    static const uint8_t pattern[] = "com.apple.os.update-";
    static const uint8_t repl[] = "shitcode.os.bullshit";
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(repl));
    ck_patcher_find_replace(range, "disable APFS snapshots", pattern, NULL,
                            sizeof(pattern), 0, repl, NULL, 0, sizeof(repl));
}

// this will tell launchd this is an internal build,
// and that way we can get hactivation without bypassing
// or patching the activation procedure.
// This is NOT an iCloud bypass. This is utilising code that ALREADY exists
// in the activation daemon. This is essentially telling iOS, it's a
// development kernel/device, NOT the real product sold on market. IF you
// decide to use this knowledge to BYPASS technological countermeasures
// or any other intellectual theft or crime, YOU are responsible in full,
// AND SHOULD BE PROSECUTED TO THE FULL EXTENT OF THE LAW.
// We do NOT endorse nor approve the theft of property.
static void ck_kp_hactivation_patch(CKPatcherRange *range)
{
    static const uint8_t pattern[] = "\0release";
    static const uint8_t repl[] = "profile";
    ck_patcher_find_replace(range, "enable hactivation", pattern, NULL,
                            sizeof(pattern), 0, repl, NULL, 1, sizeof(repl));
}

static void ck_kp_sep_mgr_patches(CKPatcherRange *range)
{
    static const uint8_t pattern[] = {
        0x00, 0x04, 0x00, 0xF9, // str x?, [x?, #0x8]
        0x08, 0x04, 0x80, 0x52, // mov w8, #0x20
        0x08, 0x10, 0x00, 0xB9, // str w8, [x?, #0x10]
    };
    static const uint8_t mask[] = { 0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0x1F, 0xFC, 0xFF, 0xFF };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    static const uint8_t repl[] = { 0x28, 0x00, 0xA0,
                                    0x52 }; // mov w8, #0x10000
    if (!ck_patcher_find_replace(
            range, "increase SCOT size to 0x10000 to use it as TRAC", pattern,
            mask, sizeof(pattern), sizeof(uint32_t), repl, NULL, 4,
            sizeof(repl))) {
        static const uint8_t pattern_new[] = {
            0x00, 0x00, 0x1E, 0xF8, // stur x?, [x?, #-0x20]
            0x08, 0x04, 0x80, 0x52, // mov w8, #0x20
            0x08, 0x80, 0x1E, 0xB8, // stur w8, [x?, #-0x18]
        };
        static const uint8_t mask_new[] = {
            0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0x1F, 0xFC, 0xFF, 0xFF
        };
        QEMU_BUILD_BUG_ON(sizeof(pattern_new) != sizeof(mask_new));
        ck_patcher_find_replace(
            range, "increase SCOT size to 0x10000 to use it as TRAC (new)",
            pattern_new, mask_new, sizeof(pattern_new), sizeof(uint32_t), repl,
            NULL, 4, sizeof(repl));
    }
}

static bool ck_kp_img4_callback(void *ctx, uint8_t *buffer)
{
    void *start =
        ck_patcher_find_prev_insn(buffer, 200, PACIBSP, 0xFFFFFFFF, 0);

    if (start == NULL) {
        return false;
    }

    stl_le_p(start, MOV_W0_0);
    stl_le_p(start + 4, RET);

    return true;
}

static void ck_kp_img4_patches(CKPatcherRange *range)
{
    // in Img4DecodePerformTrustEvaluationWithCallbacksInternal
    static const uint8_t pattern[] = {
        0x21, 0x09, 0x43, 0xB2, // orr x1, x9, #0xe000000000000000
    };
    ck_patcher_find_callback(
        range, "allow unsigned firmware in img4_firmware_evaluate", pattern,
        NULL, sizeof(pattern), sizeof(uint32_t), ck_kp_img4_callback);

    {
        /*
         * The patch above makes the trust evaluation return early without
         * filling in its `dr`/`ct` out-parameters, so callers that inspect
         * them (e.g. restored's seal_system_volume, which evaluates the
         * `msys` mtree) see dr = -1 / ct = 0xaaaaaaaa and bail out with
         * error 80. Turn that error return into success.
         *
         * mov x2, #0 ; mov x17, #0xf288 ; blraa x20, x17 ; mov w20, #0x50
         *                                             -> mov w20, #0
         */
        static const uint8_t te_pat[] = { 0x02, 0x00, 0x80, 0xd2, 0x11, 0x51,
                                          0x9e, 0xd2, 0x91, 0x0a, 0x3f, 0xd7,
                                          0x14, 0x0a, 0x80, 0x52 };
        /*
         * This patch does NOT match the 21A329 restore kernelcache -- the
         * `mov x17, #imm16` is a build-specific PAC discriminator and 0xf288 is
         * not what this build uses, so the patch is silently skipped and
         * `seal_system_volume` fails with dr = -1 / ct = 0xaaaaaaaa, error 80.
         *
         * Do NOT "fix" that by masking the imm16 field: tried, and the looser
         * pattern matches somewhere it should not. The restore ramdisk then
         * panics during its own boot, long before the restore starts --
         * `mount[9] exited ... exit status 65`. (Note also that the patcher
         * asserts pattern == (pattern & mask), so masked bits must be zeroed in
         * the pattern too, or QEMU aborts at startup with no guest output.)
         *
         * The right fix is to find this build's actual discriminator, or anchor
         * on more surrounding context so the match stays unique.
         */
        static const uint8_t te_mask[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                           0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                           0xff, 0xff, 0xff, 0xff };
        static const uint8_t te_repl[] = { 0x14, 0x00, 0x80, 0x52 };
        static const uint8_t te_keep[] = { 0, 0, 0, 0 };

        ck_patcher_find_replace(range,
                                "trust evaluation returns success (seal)",
                                te_pat, te_mask, sizeof(te_pat),
                                sizeof(uint32_t), te_repl, te_keep, 12,
                                sizeof(te_repl));

        /*
         * Root cause: the evaluation result field is left at -1 (unset),
         * and the caller does `cmn w20, #1 / b.eq <fail>`. Load a success
         * value instead of the unset result.
         *
         * ldr x23,[x2,#0x58] ; ldr w20,[x23,#0x170] -> mov w20, #0
         */
        static const uint8_t dr_pat[] = { 0x57, 0x2c, 0x40, 0xf9, 0xf4, 0x72,
                                          0x41, 0xb9, 0x48, 0x24, 0x40, 0xf9,
                                          0x16, 0x21, 0x40, 0xb9 };
        static const uint8_t dr_mask[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                           0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                           0xff, 0xff, 0xff, 0xff };
        static const uint8_t dr_repl[] = { 0x14, 0x00, 0x80, 0x52 };
        static const uint8_t dr_keep[] = { 0, 0, 0, 0 };

        ck_patcher_find_replace(range, "img4 trust evaluation result = OK",
                                dr_pat, dr_mask, sizeof(dr_pat),
                                sizeof(uint32_t), dr_repl, dr_keep, 4,
                                sizeof(dr_repl));

        /*
         * DISABLED: "img4 execution context error = 0".
         *
         * This was meant to make img4_firmware_execute pass success to its
         * execution-context callback (x2) so `seal_system_volume` proceeds. Two
         * separate problems, both found the hard way:
         *
         * 1. The pattern -- four generic register moves
         *    `mov x0,x19 ; mov x1,x22 ; mov x2,x21 ; mov x3,x20` -- occurs
         *    **168 times** in the 21A329 restore kernelcache, and the matcher
         *    takes the first hit and stops. It was landing inside the IPv4 AH
         *    input handler (`sub_FFFFFFF0080B7A78`, the one with "IPv4 AH input:
         *    can't pullup"), corrupting the network stack while never touching
         *    img4 -- and still printing "patch applied". Anchoring on the
         *    preceding `blraa` (the "calling out to execution context: %d" log
         *    call, in `sub_FFFFFFF008C8B320`) does make it unique.
         *
         * 2. But once it actually applies, **SEP stops booting**:
         *    `panic: SEP Panic: :sars/sars: ...`, reproducibly, from a pristine
         *    disk set that otherwise restores fine. `img4_firmware_execute` is
         *    shared by every firmware type, so forcing the error to 0 lets an
         *    invalid SEP image through and SEPOS dies in its `sars` app.
         *
         * Leaving it out is strictly better than either version: the restore
         * fails at `seal_system_volume` exactly as it did before (the patch was
         * never affecting img4 anyway), SEP keeps working, and the network stack
         * is no longer being corrupted. A real fix has to be selective -- only
         * the `msys` / SSV evaluation, not the SEP firmware path.
         *
         * For reference, the unique form was:
         *   pat  = blraa x23,x17 ; mov x0,x19 ; mov x1,x22 ; mov x2,x21 ;
         *          mov x3,x20      (20 bytes)
         *   repl = mov x2,xzr at offset 12
         */
    }
}

static void ck_kp_cs_patches(CKPatcherRange *range)
{
    // skip code signature checks in vm_fault_enter
    static const uint8_t pattern[] = {
        0x00, 0x00, 0x18, 0x36, // tbz w?, #3, #?
        0x00, 0x00, 0x80, 0x52, // mov w?, #0
    };
    static const uint8_t mask[] = { 0x00, 0x00, 0xF8, 0xFF,
                                    0xE0, 0xFF, 0xFF, 0xFF };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    static const uint8_t repl[] = { NOP_BYTES };
    if (!ck_patcher_find_replace(range, "bypass code signature checks", pattern,
                                 mask, sizeof(pattern), sizeof(uint32_t), repl,
                                 NULL, 0, sizeof(repl))) {
        static const uint8_t alt[] = {
            0x00, 0x00, 0x18, 0x36, // tbz w?, #3, #?
            0x10, 0x02, 0x17, 0xAA, // mov x?, x?
            0x00, 0x00, 0x80, 0x52, // mov w?, #0
        };
        static const uint8_t mask_alt[] = {
            0x00, 0x00, 0xF8, 0xFF, 0x10, 0xFE,
            0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF
        };
        QEMU_BUILD_BUG_ON(sizeof(alt) != sizeof(mask_alt));
        ck_patcher_find_replace(range, "bypass code signature checks (alt)",
                                alt, mask_alt, sizeof(alt), sizeof(uint32_t),
                                repl, NULL, 0, sizeof(repl));
    }
}

static void ck_kp_pmap_cs_enforce_patch(CKPatcherRange *range)
{
    // in pmap_enter_options_internal
    static const uint8_t pattern[] = {
        0x00, 0x00, 0x00, 0x94, // bl #?
        0x00, 0x00, 0x00, 0x35, // cbnz w0, #?
        0x88, 0x63, 0x80, 0x92, // mov x8, #0xfffffffffffffce3
    };
    static const uint8_t mask[] = {
        0x00, 0x00, 0x00, 0xFC, 0x1F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    static const uint8_t repl[] = { MOV_W0_0_BYTES, NOP_BYTES };
    ck_patcher_find_replace(range, "bypass pmap_cs_enforce", pattern, mask,
                            sizeof(pattern), sizeof(uint32_t), repl, NULL, 0,
                            sizeof(repl));
}

/*
 * Let QEMU perform every XPRR class change, so PPL's pages stay reachable.
 *
 * Apple silicon decodes a PTE's AP/PXN/UXN as a 4-bit index into SPRR_PERM,
 * and each entry holds two permissions: one for guarded (GL1) execution, one
 * for everything else. XNU isolates PPL that way -- PPL text is the class that
 * reads "RX guarded, R otherwise", PPL data "RW guarded, nothing otherwise".
 *
 * Under TCG that works, because target/arm/ptw.c models it and consults
 * arm_is_guarded(). Under HVF it cannot: the host CPU decodes the guest's PTEs
 * through its own permission-remap register and never actually enters GL1, so
 * everything PPL touches while QEMU thinks it is guarded faults. The fault
 * vectors through VBAR_GL1, which is itself PPL text, so it re-faults on its
 * own vector and the vCPU spins there forever with no exit to the hypervisor.
 *
 * pmap_set_pte_xprr_perm() is the single choke point for XPRR class changes.
 * Replacing its first instruction with an HVC hands the whole operation to
 * hvf_xprr_set_pte(), which does the PTE update itself and substitutes a class
 * that is permissive outside guarded mode. Simply returning early instead was
 * tried and is worse (17/24 against 47/56): it also skips the transitions that
 * make pages writable *again*.
 *
 * Matched on the shape of the function rather than any address: the #0x10
 * bound on the XPRR index, the hint-bit test at #0x34, and the #0x35,#2 field
 * extraction that builds the current class. Register fields are masked out, so
 * this is not tied to one build. The function lives in __PPLTEXT, not
 * __TEXT_EXEC.
 */
static bool ck_kp_ppl_xprr_callback(void *ctx, uint8_t *buffer)
{
    void *func_start =
        ck_patcher_find_prev_insn(buffer, 0x20, PACIBSP, 0xFFFFFFFF, 0);

    if (func_start == NULL) {
        error_report("%s: failed to find pacibsp", __func__);
        return false;
    }
    stl_le_p(func_start, A64_HVC(GXF_HVC_IMM_XPRR));
    return true;
}

static void ck_kp_hvf_ppl_xprr_patch(CKPatcherRange *range)
{
    static const uint8_t pattern[] = {
        0x00, 0x00, 0x00, 0x2A,  // orr  w8, w2, w1
        0x1F, 0x40, 0x00, 0x71,  // cmp  w8, #0x10
        0x02, 0x00, 0x00, 0x54,  // b.cs <panic>
        0x00, 0x00, 0x40, 0xF9,  // ldr  x8, [x0]
        0x00, 0x00, 0x08, 0x36,  // tbz  w8, #1, <panic>
        0x00, 0x00, 0xA0, 0xB7,  // tbnz x8, #0x34, <panic>
        0x00, 0x00, 0x00, 0x2A,  // mov  w10, w1
        0x00, 0xFC, 0x44, 0xD3,  // lsr  x9, x8, #4
        0x00, 0x04, 0x7E, 0x92,  // and  x9, x9, #0xC
        0x00, 0xD8, 0x75, 0xB3,  // bfxil x9, x8, #0x35, #2
        0x1F, 0x00, 0x00, 0xEB,  // cmp  x9, x10
        0x01, 0x00, 0x00, 0x54,  // b.ne <panic>
    };
    static const uint8_t mask[] = {
        0x00, 0x00, 0x00, 0xFF, 0x1F, 0xFC, 0xFF, 0xFF,
        0x1F, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xC0, 0xFF,
        0x00, 0x00, 0xF8, 0xFF, 0x00, 0x00, 0xF8, 0xFF,
        0x00, 0x00, 0x00, 0xFF, 0x00, 0xFC, 0xFF, 0xFF,
        0x00, 0xFC, 0xFF, 0xFF, 0x00, 0xFC, 0xFF, 0xFF,
        0x1F, 0xFC, 0xE0, 0xFF, 0x1F, 0x00, 0x00, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));

    if (!ck_patcher_find_callback(range, "leave PPL pages in kernel XPRR "
                                  "classes", pattern, mask, sizeof(pattern),
                                  sizeof(uint32_t), ck_kp_ppl_xprr_callback)) {
        warn_report("GXF/HVF: pmap_set_pte_xprr_perm not found; boots may "
                    "wedge on a permission fault taken in guarded mode");
    }
}

/*
 * Hypervisor.framework does not expose Apple's GXF to guests. Rewrite every
 * GENTER/GEXIT in the kernel's executable segments into an HVC with a magic
 * immediate; target/arm/hvf/hvf.c emulates the GL1 bank switch on the exit.
 */
static void ck_kp_gxf_hvc_patch_segment(MachoSegmentCommand64 *seg,
                                        uint32_t *genter, uint32_t *gexit)
{
    uint32_t *insns;
    uint64_t count;
    uint64_t i;

    if (seg == NULL || (seg->initprot & VM_PROT_EXECUTE) == 0 ||
        seg->filesize == 0) {
        return;
    }

    insns = apple_boot_va_to_ptr(seg->vmaddr);
    count = MIN(seg->vmsize, seg->filesize) / sizeof(uint32_t);

    for (i = 0; i < count; i++) {
        uint32_t insn = ldl_le_p(&insns[i]);

        switch (insn & GXF_INSN_MASK) {
        case GXF_INSN_GENTER:
            stl_le_p(&insns[i],
                     A64_HVC(GXF_HVC_IMM_GENTER | GXF_INSN_IMM(insn)));
            *genter += 1;
            break;
        case GXF_INSN_GEXIT:
            stl_le_p(&insns[i],
                     A64_HVC(GXF_HVC_IMM_GEXIT | GXF_INSN_IMM(insn)));
            *gexit += 1;
            break;
        default:
            break;
        }
    }
}

static void ck_kp_gxf_hvc_patch_image(MachoHeader64 *image, uint32_t *genter,
                                      uint32_t *gexit)
{
    MachoLoadCommand *cmd;
    uint32_t i;

    cmd = (MachoLoadCommand *)(image + 1);
    for (i = 0; i < image->n_cmds; i++) {
        if (cmd->cmd == LC_SEGMENT_64) {
            ck_kp_gxf_hvc_patch_segment((MachoSegmentCommand64 *)cmd, genter,
                                        gexit);
        }
        cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
    }
}

static void ck_kp_gxf_hvc_patch(MachoHeader64 *hdr)
{
    MachoLoadCommand *cmd;
    uint32_t genter = 0;
    uint32_t gexit = 0;
    uint32_t i;

    /*
     * The encodings live in the UDF space, so rewriting every match in every
     * executable segment (kernel and kexts alike) cannot break valid code.
     */
    if (hdr->file_type == MH_FILESET) {
        cmd = (MachoLoadCommand *)(hdr + 1);
        for (i = 0; i < hdr->n_cmds; i++) {
            if (cmd->cmd == LC_FILESET_ENTRY) {
                MachoFilesetEntryCommand *entry =
                    (MachoFilesetEntryCommand *)cmd;
                ck_kp_gxf_hvc_patch_image(apple_boot_va_to_ptr(entry->vm_addr),
                                          &genter, &gexit);
            }
            cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
        }
    } else {
        ck_kp_gxf_hvc_patch_image(hdr, &genter, &gexit);
    }

    if (genter == 0 || gexit == 0) {
        warn_report("GXF/HVF: found %u GENTER and %u GEXIT; PPL will not "
                    "work under HVF",
                    genter, gexit);
    } else {
        info_report("GXF/HVF: rewrote %u GENTER and %u GEXIT to HVC", genter,
                    gexit);
    }
}

/*
 * Under HVF the host applies stock ARM semantics to SCTLR_EL1.En{IA,IB,DA,DB}:
 * the kernel's own PAC instructions become NOPs whenever a key is disabled.
 * On Apple silicon those bits only gate EL0 (the kernel keys are always on),
 * and XNU relies on that: pmap_switch() clears EnIA/EnDA/EnDB for pmaps with
 * JOP disabled while the kernel keeps signing data pointers (kstackptr, ...)
 * with the DA key. With FPAC on the host a later AUTDA then faults. Keep all
 * four key enables set at all times: boot value and the pmap_switch toggle.
 */
static bool ck_kp_hvf_pac_pmap_switch_callback(void *ctx, uint8_t *buffer)
{
    /*
     * pmap_switch clears the PAC key-enable bits with `and x9, x9, x10`, and
     * we do not want it to, because the host enforces FPAC. Drop the AND
     * rather than turning it into an ORR.
     *
     * x10 is the *inverted* mask -- almost all ones, with holes where the bits
     * to clear are -- so `orr x9, x9, x10` sets nearly every bit instead of
     * keeping the handful the AND would have kept. The result goes to
     * SCTLR_EL1, which ends up as e.g. 0x9e03efd8fcfdffdf against a healthy
     * 0xfc54799d, with SCTLR_EL1.A set. Alignment checking then turns the
     * kernel's ordinary unaligned 128-bit accesses (`ldur q0, [x20, #8]` in
     * PPL's exception-frame handler) into data aborts, and because the handler
     * re-takes the same fault on the frame it is building, it recurses ~0x500
     * of stack per round until XNU's stack-bounds check spins on `b.lt .`.
     */
    const char *mode = getenv("INFERNO_HVF_PAC_PMAP");
    void *insn = ck_patcher_find_next_insn(buffer, 16, 0x8A0A0129, 0xFFFFFFFF, 0);

    if (insn == NULL) {
        return false;
    }
    if (mode != NULL && strcmp(mode, "none") == 0) {
        return true;            /* leave the kernel alone */
    }
    if (mode != NULL && strcmp(mode, "orr") == 0) {
        stl_le_p(insn, 0xAA0A0129);     /* the original, corrupting, patch */
        return true;
    }
    stl_le_p(insn, NOP);
    return true;
}

/*
 * The INFERNO_HVF_PAC=off half of the same instruction sequence.
 *
 * pmap_switch both clears and sets the three JOP key enables:
 *
 *     mov  w10, #0x88002000
 *     mrs  x9, SCTLR_EL1
 *     ldrb w11, [x0, #0xC7]        ; pmap->disable_jop
 *     tst  x9, x10                 ; are they set right now?
 *     b.eq currently_clear
 *       cbz w11, done              ; want on, already on
 *       and x9, x9, #~0x88002000   ; want off  <- what PAC=on NOPs
 *       b   write
 *     currently_clear:
 *       cbnz w11, done             ; want off, already off
 *       orr  x9, x9, x10           ; want on   <- what PAC=off NOPs
 *     write:
 *       msr  SCTLR_EL1, x9
 *
 * so NOPing the `orr` is enough on its own: the boot value already has all
 * three clear (0x7454593d), nothing else in the kernel sets them, and the
 * `and` is then a no-op on bits that are never set. EnIB stays set throughout,
 * exactly as on hardware -- Apple only ever disables the JOP keys, never the
 * return-address key, so `pacibsp`/`retab` keep working at both exception
 * levels and keep working for each other.
 */
static bool ck_kp_hvf_pac_never_enable_callback(void *ctx, uint8_t *buffer)
{
    void *insn = ck_patcher_find_next_insn(buffer, 16, 0xAA0A0129, 0xFFFFFFFF, 0);

    if (insn == NULL) {
        return false;
    }
    stl_le_p(insn, NOP);
    return true;
}

/*
 * dyld's hardware TPRO fast path toggles write access to __DATA_CONST via the
 * Apple SPRR uperm register instead of vm_protect(). HVF's host CPU doesn't
 * enforce Apple SPRR remapping for the guest, so those writes still fault
 * against the read-only PTE and initproc dies. Force the commpage
 * `dyld_hw_tpro` byte (offset 0x10c) to 0 so dyld uses the syscall fallback.
 */
/*
 * Apple's hardware TPRO relies on SPRR permission remapping. Hypervisor.framework
 * does not expose the SPRR registers to guest EL0 at all: dyld's
 * `mrs/msr sprr_uperm_el0` raises an undefined-instruction exception straight to
 * the guest kernel, which kills dyld (SIGILL), and reporting "no TPRO" through
 * only the commpage makes dyld `brk #1` instead (SIGTRAP). The only consistent
 * configuration is to disable hardware TPRO kernel-wide, so both the kernel and
 * dyld use the software (vm_protect) path.
 *
 * Every reader of the master "hardware TPRO supported" global is rewritten from
 * `ldr wD, [xN, #0xff8]` to `mov wD, #0`.
 */
typedef struct {
    uint8_t bytes[8]; /* adrp xN, <page> ; ldr wD, [xN, #0xff8] */
    uint8_t repl[4]; /* mov wD, #0 */
    bool ppl;
} CKHvfTproSite;

static void ck_kp_hvf_disable_hw_tpro(CKPatcherRange *kernel_text,
                                      CKPatcherRange *ppl_text)
{
    static const CKHvfTproSite sites[] = {
        { { 0x08, 0xd5, 0xff, 0xf0, 0x08, 0xf9, 0x4f, 0xb9 },
          { 0x08, 0x00, 0x80, 0x52 }, false },
        { { 0x0a, 0xd5, 0xff, 0xd0, 0x4a, 0xf9, 0x4f, 0xb9 },
          { 0x0a, 0x00, 0x80, 0x52 }, false },
        { { 0x0b, 0xd5, 0xff, 0xb0, 0x6b, 0xf9, 0x4f, 0xb9 },
          { 0x0b, 0x00, 0x80, 0x52 }, false },
        { { 0xe8, 0xd2, 0xff, 0xb0, 0x08, 0xf9, 0x4f, 0xb9 },
          { 0x08, 0x00, 0x80, 0x52 }, false },
        { { 0xa8, 0xd2, 0xff, 0xf0, 0x08, 0xf9, 0x4f, 0xb9 },
          { 0x08, 0x00, 0x80, 0x52 }, false },
        { { 0x0a, 0xbc, 0xff, 0xd0, 0x4a, 0xf9, 0x4f, 0xb9 },
          { 0x0a, 0x00, 0x80, 0x52 }, false },
        { { 0xa8, 0xa3, 0xff, 0xf0, 0x08, 0xf9, 0x4f, 0xb9 },
          { 0x08, 0x00, 0x80, 0x52 }, true },
        { { 0xa9, 0xa3, 0xff, 0xd0, 0x29, 0xf9, 0x4f, 0xb9 },
          { 0x09, 0x00, 0x80, 0x52 }, true },
    };
    static const uint8_t full[8] = { 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff };
    static const uint8_t keep[4] = { 0, 0, 0, 0 };
    uint32_t applied = 0;
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(sites); i++) {
        CKPatcherRange *range = sites[i].ppl ? ppl_text : kernel_text;

        if (range == NULL) {
            continue;
        }
        if (ck_patcher_find_replace(range, "HVF: disable hardware TPRO global",
                                    sites[i].bytes, full, sizeof(sites[i].bytes),
                                    sizeof(uint32_t), sites[i].repl, keep, 4,
                                    sizeof(sites[i].repl))) {
            applied++;
        }
    }
    info_report("HVF: disabled hardware TPRO at %u/%u sites", applied,
                (uint32_t)ARRAY_SIZE(sites));
}

static void ck_kp_hvf_tpro_patch(CKPatcherRange *kernel_text)
{
    // strb w8, [x9, #0x10c] ; ldr x8, [x19, #0x938] ; str x11, [x8, #0x110]
    static const uint8_t pattern[] = { 0x28, 0x31, 0x04, 0x39, 0x68, 0x9e,
                                       0x44, 0xf9, 0x0b, 0x89, 0x00, 0xf9 };
    static const uint8_t mask[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                    0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    // strb wzr, [x9, #0x10c]
    static const uint8_t repl[] = { 0x3f, 0x31, 0x04, 0x39 };
    static const uint8_t repl_mask[] = { 0, 0, 0, 0 }; // keep nothing

    // exec_add_apple_strings: `ldrb w8,[x20,#0x49] ; tbz w8,#6, skip ;
    // adrp x1, "dyld_hw_tpro=1"`  -- force the tbz into an unconditional
    // branch so the `dyld_hw_tpro=1` apple string is never handed to dyld.
    static const uint8_t as_pat[] = { 0x88, 0x26, 0x41, 0x39, 0x48, 0xf9,
                                      0x37, 0x36, 0x81, 0x78, 0xff, 0x90 };
    static const uint8_t as_mask[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    // b <same target as the tbz> (offset -0xd8 from the tbz)
    static const uint8_t as_repl[] = { 0xca, 0xff, 0xff, 0x17 };
    static const uint8_t as_keep[] = { 0, 0, 0, 0 };

    if (kernel_text == NULL) {
        return;
    }
    /*
     * Zeroing the commpage byte is itself a dyld trap trigger (dyld does
     * `cbz -> brk #1` on it), so only do it when explicitly requested.
     */
    if (getenv("INFERNO_TPRO_COMMPAGE") != NULL) {
        ck_patcher_find_replace(kernel_text, "HVF: disable hardware dyld TPRO",
                                pattern, mask, sizeof(pattern),
                                sizeof(uint32_t), repl, repl_mask, 0,
                                sizeof(repl));
    }
    ck_patcher_find_replace(kernel_text,
                            "HVF: suppress dyld_hw_tpro=1 apple string",
                            as_pat, as_mask, sizeof(as_pat), sizeof(uint32_t),
                            as_repl, as_keep, 4, sizeof(as_repl));

    {
        // ldr w8,[x21,#0x80] ; cbz w8, skip ; adrp x8, ...
        // -> force the cbz unconditional so `dyld_hw_tpro_pagers=1` is never
        // added either (dyld enables hw TPRO from that string too).
        static const uint8_t pg_pat[] = { 0xa8, 0x82, 0x40, 0xb9, 0xc8, 0xf7,
                                          0xff, 0x34, 0x08, 0xbc, 0xff, 0xb0 };
        static const uint8_t pg_mask[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                           0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        static const uint8_t pg_repl[] = { 0xbe, 0xff, 0xff, 0x17 }; // b skip
        static const uint8_t pg_keep[] = { 0, 0, 0, 0 };

        ck_patcher_find_replace(kernel_text,
                                "HVF: suppress dyld_hw_tpro_pagers=1 apple string",
                                pg_pat, pg_mask, sizeof(pg_pat),
                                sizeof(uint32_t), pg_repl, pg_keep, 4,
                                sizeof(pg_repl));
    }
}

/*
 * Debug aid (INFERNO_DEBUG_EXEC=1): trap the exec_add_apple_strings exit
 * (stack-guard reload) so hvf.c can log every exec's return value and path.
 */

/*
 * XNU marks a task "user JOP disabled" when its main executable is plain arm64
 * rather than arm64e, and then hands it the *default* JOP key instead of a
 * per-task one; on real hardware that is harmless because pmap_switch also
 * clears SCTLR_EL1.En{IA,DA,DB}, so every PAC instruction at EL0 is a no-op.
 * Under HVF those bits gate EL1 as well, so ck_kp_hvf_pac_pmap_switch_callback
 * has to keep them set -- and a JOP-disabled process then executes real PAC
 * instructions against pointers the kernel never signed for it.
 *
 * On iOS 17 exactly one system binary is affected: /usr/libexec/mobileassetd,
 * the only non-arm64e executable on the system volume. It dies every launch at
 * dyld+0x24b6c (`blraa` on an unsigned lsl::EphemeralAllocator vtable slot)
 * with EXC_BAD_ACCESS code 0x105 = EXC_ARM_PAC_FAIL, crash-loops every 10 s,
 * and takes MobileAsset's XPC with it: duetexpertd deadlocks behind a
 * synchronous MobileAsset call and Proactive stops answering, which is what
 * leaves the iOS 17 wallpaper gallery empty.
 *
 * Give every task a per-task JOP key, so no task is ever "JOP disabled" from the
 * key's point of view.
 *
 * TRIED 2026-09-23, DID NOT FIX IT -- mobileassetd still faults at the same PC
 * with the same unsigned x8, so the pointer is never signed at all rather than
 * signed with the wrong key. The task-level `disable_user_jop` flag (set
 * elsewhere, from the Mach-O cpusubtype) is what the fixup-signing path must be
 * keyed on. Left in, off by default, as a documented dead end:
 * INFERNO_HVF_JOP_ALWAYS=1 to enable.
 */
static bool ck_kp_hvf_jop_always_callback(void *ctx, uint8_t *buffer)
{
    stl_le_p(buffer + 0, 0x6A08011F);   /* tst w8, w21  -> tst w8, w8 */
    stl_le_p(buffer + 20, NOP);         /* tbnz w21, #0, disable_jop  -> nop */
    return true;
}

static void ck_kp_hvf_jop_always(CKPatcherRange *kernel_text)
{
    static const uint8_t pattern[] = {
        0x1F, 0x01, 0x15, 0x6A, // tst  w8, w21
        0xE8, 0x0B, 0x40, 0xF9, // ldr  x8, [sp, #0x10]
        0x08, 0x11, 0x9C, 0x9A, // csel x8, x8, x28, ne
        0xFC, 0x03, 0x14, 0xAA, // mov  x28, x20
        0x09, 0x00, 0x00, 0x34, // cbz  w9, ?
        0x15, 0x00, 0x00, 0x37, // tbnz w21, #0, ?
    };
    static const uint8_t mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x1F, 0x00, 0x00, 0xFF, 0x1F, 0x00, 0xF8, 0xFF,
    };
    const char *mode = getenv("INFERNO_HVF_JOP_ALWAYS");

    if (mode == NULL || strcmp(mode, "0") == 0) {
        return;
    }
    ck_patcher_find_callback(kernel_text, "HVF: every task gets a JOP key",
                             pattern, mask, sizeof(pattern), sizeof(uint32_t),
                             ck_kp_hvf_jop_always_callback);
}

/*
 * XNU refuses to PAC-sign a task's chained fixups when the task is "user JOP
 * disabled" -- correct on real hardware, where EL0 PAC is a no-op for such a
 * task anyway. Under HVF we have to keep SCTLR_EL1.En{IA,DA,DB} set (see
 * ck_kp_hvf_pac_pmap_switch_callback), so the process really does authenticate
 * pointers nobody signed, and /usr/libexec/mobileassetd -- the only non-arm64e
 * binary on the system volume -- dies in dyld every launch with
 * EXC_ARM_PAC_FAIL. Its crash loop hangs MobileAsset's XPC, which deadlocks
 * duetexpertd, which is why the wallpaper gallery comes up empty.
 *
 * Two halves:
 *
 *   1. vm_shared_region_slide_page drops the signing when the task's jop_key
 *      is zero (`cbz x22`) or the slide info says not to (`cbz w8`). Drop both
 *      tests so the fixup is signed either way.
 *   2. pmap_sign_user_ptr brackets the PAC instruction with
 *      ml_enable/disable_user_jop_key(jop_key). With jop_key == 0 that would
 *      install key zero; skipping both calls instead signs with whatever key is
 *      already loaded, which during a kernel operation is the default JOP key
 *      -- exactly the key a JOP-disabled task runs with at EL0.
 *
 * Mutually exclusive with INFERNO_HVF_JOP_ALWAYS, which gives such a task a
 * per-task key instead and would break the match. Off by default:
 * INFERNO_HVF_SIGN_NOJOP=1 to enable.
 *
 * TRIED 2026-09-23, DID NOT FIX IT. All three patches apply cleanly and the
 * guest boots normally, but mobileassetd still takes the identical fault, so
 * neither kernel signer is the one leaving that pointer unsigned -- which is
 * useful negative evidence: the remaining suspect is dyld's own rebaseSelf.
 * Note mobileassetd's crash report says `sharedCache: null`, so the
 * shared-region slide path could not have been involved for it anyway.
 */
static bool ck_kp_hvf_slide_sign_callback(void *ctx, uint8_t *buffer)
{
    stl_le_p(buffer + 8, NOP);      /* cbz x22, skip_signing */
    stl_le_p(buffer + 16, NOP);     /* cbz w8,  skip_signing */
    return true;
}

static bool ck_kp_hvf_sign_user_ptr_callback(void *ctx, uint8_t *buffer)
{
    /* mov x0, x19 -> cbz x19, +8, i.e. jump over the bl that follows */
    stl_le_p(buffer + 8, 0xB4000053);
    stl_le_p(buffer + 48, 0xB4000053);
    return true;
}

/*
 * The second place XNU decides not to sign: the page-in-linking fixup applier,
 * which reads the jop_key out of the map entry and bails when it is zero. Same
 * treatment as the shared-region slide path above.
 */
static bool ck_kp_hvf_pil_sign_callback(void *ctx, uint8_t *buffer)
{
    stl_le_p(buffer + 16, NOP);     /* cbz x3, skip_signing */
    return true;
}

static void ck_kp_hvf_sign_nojop(CKPatcherRange *kernel_text)
{
    static const uint8_t slide_pattern[] = {
        0x3F, 0x03, 0x50, 0xF2, // tst  x25, #0x1000000000000
        0x02, 0x01, 0x89, 0x9A, // csel x2, x8, x9, eq
        0x16, 0x00, 0x00, 0xB4, // cbz  x22, ?
        0xA8, 0x16, 0x40, 0x39, // ldrb w8, [x21, #5]
        0x08, 0x00, 0x00, 0x34, // cbz  w8, ?
        0x21, 0xCB, 0x71, 0xD3, // ubfx x1, x25, #49, #2
        0xE3, 0x03, 0x16, 0xAA, // mov  x3, x22
    };
    static const uint8_t slide_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x1F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x1F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
    };
    static const uint8_t sign_pattern[] = {
        0xDF, 0x4F, 0x03, 0xD5, // msr  DAIFSet, #0xf
        0xE0, 0x03, 0x13, 0xAA, // mov  x0, x19
        0x00, 0x00, 0x00, 0x94, // bl   ml_enable_user_jop_key
        0xE1, 0x03, 0x00, 0xAA, // mov  x1, x0
        0xF1, 0x03, 0x15, 0xAA, // mov  x17, x21
        0xF0, 0x03, 0x14, 0xAA, // mov  x16, x20
        0x16, 0x00, 0x00, 0x34, // cbz  w22, ?
        0x11, 0x0A, 0xC1, 0xDA, // pacda x17, x16
        0x00, 0x00, 0x00, 0x14, // b    ?
        0x1F, 0x21, 0x03, 0xD5, // pacia1716
        0xF4, 0x03, 0x11, 0xAA, // mov  x20, x17
        0xE0, 0x03, 0x13, 0xAA, // mov  x0, x19
        0x00, 0x00, 0x00, 0x94, // bl   ml_disable_user_jop_key
    };
    static const uint8_t sign_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x1F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0xFC,
    };
    const char *mode = getenv("INFERNO_HVF_SIGN_NOJOP");

    if (mode == NULL || strcmp(mode, "0") == 0) {
        return;
    }
    ck_patcher_find_callback(kernel_text, "HVF: sign fixups for JOP-disabled tasks",
                             slide_pattern, slide_mask, sizeof(slide_pattern),
                             sizeof(uint32_t), ck_kp_hvf_slide_sign_callback);
    ck_patcher_find_callback(kernel_text, "HVF: pmap_sign_user_ptr keeps the live key",
                             sign_pattern, sign_mask, sizeof(sign_pattern),
                             sizeof(uint32_t), ck_kp_hvf_sign_user_ptr_callback);

    static const uint8_t pil_pattern[] = {
        0x89, 0x00, 0x1F, 0x32, // orr  w9, w4, #2
        0x3F, 0x09, 0x00, 0x71, // cmp  w9, #2
        0x01, 0x00, 0x00, 0x54, // b.ne ?
        0xA3, 0x60, 0x40, 0xF9, // ldr  x3, [x5, #0xc0]
        0x03, 0x00, 0x00, 0xB4, // cbz  x3, ?
        0xE0, 0x03, 0x08, 0xAA, // mov  x0, x8
        0xE1, 0x03, 0x04, 0xAA, // mov  x1, x4
    };
    static const uint8_t pil_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x0F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x1F, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
    };
    ck_patcher_find_callback(kernel_text, "HVF: sign page-in-linking fixups too",
                             pil_pattern, pil_mask, sizeof(pil_pattern),
                             sizeof(uint32_t), ck_kp_hvf_pil_sign_callback);
}

/*
 * Let plain-arm64 binaries run: never create a pmap with JOP disabled.
 *
 * XNU marks a task "JOP disabled" when its main executable is not arm64e, and
 * then two things follow: pmap_switch() clears SCTLR_EL1.En{IA,DA,DB} so EL0
 * PAC is a no-op, and the shared-region slide stores that task's chained
 * fixups **unsigned**. Both are consistent on hardware.
 *
 * Under HVF they cannot be. Those SCTLR bits gate EL1 as well here, so
 * ck_kp_hvf_pac_pmap_switch_callback has to keep them set -- which leaves PAC
 * genuinely live for a task whose fixups nobody signed. dyld only self-rebases
 * when it is *not* in the dyld cache (`inDyldCache` -> skip, dyld+0x5568), so
 * for these tasks the kernel is the only fixup applier and its unsigned vtable
 * slots reach a real `blraa`:
 *
 *     dyld  aligned_alloc  EXC_BAD_ACCESS (0x105 = EXC_ARM_PAC_FAIL)
 *
 * That is every non-arm64e binary on the system: `mobileassetd` (whose crash
 * loop empties the wallpaper gallery) and the DDI's `dtdebugproxyd` (which is
 * why debugserver never answers).
 *
 * Clearing the flag at the source puts those tasks on the ordinary arm64e
 * path: the slide signs, PAC stays on, and the auth matches. The main
 * executable itself is unaffected -- a plain arm64 image has no auth fixups
 * and executes no PAC instructions.
 *
 * This is a real advance but not yet a fix, so it is opt-in via
 * INFERNO_NEVER_DISABLE_JOP=1. With it on, both `mobileassetd` and
 * `dtdebugproxyd` get all the way past dyld and then die identically one stage
 * later, in libobjc:
 *
 *     libobjc  class_data_bits_t::safe_ro<(Authentication)0>()
 *              readClass() <- map_images_nolock <- map_images
 *
 * which is the same mismatch moved along: a plain-arm64 image's `__objc_data`
 * has only plain rebases, so its class_ro pointers are never signed by anyone,
 * and libobjc still authenticates them. Nothing the kernel does can sign those
 * -- the next lever would be libobjc itself, which Inferno already patches in
 * the shared cache.
 */
static void ck_kp_hvf_never_disable_jop(CKPatcherRange *ppl_text)
{
    if (ppl_text == NULL || getenv("INFERNO_NEVER_DISABLE_JOP") == NULL) {
        return;
    }
    static const uint8_t pattern[] = {
        0x75, 0x12, 0x00, 0xF9, // str   x21, [x19, #0x20]
        0x7F, 0xF2, 0x0B, 0x78, // sturh wzr, [x19, #0xBF]
        0x7F, 0x92, 0x01, 0x79, // strh  wzr, [x19, #0xC8]
        0x76, 0x1E, 0x03, 0x39, // strb  w22, [x19, #0xC7]   ; pmap->disable_jop
    };
    static const uint8_t repl[] = { 0x7F, 0x1E, 0x03, 0x39 }; // strb wzr, [x19, #0xC7]
    ck_patcher_find_replace(ppl_text, "never create a JOP-disabled pmap",
                            pattern, NULL, sizeof(pattern), sizeof(uint32_t),
                            repl, NULL, 12, sizeof(repl));
}

/*
 * Two self-consistent ways to run PAC under HVF, and one that is not.
 *
 * Apple's implementation of SCTLR_EL1.En{IA,IB,DA,DB} gates EL0 only -- the
 * kernel's keys are always live -- and XNU leans on that: pmap_switch() clears
 * the three enables for a "user JOP disabled" pmap (one whose main executable
 * is plain arm64 rather than arm64e) while the kernel carries on signing
 * kstackptr and friends. HVF gives the guest stock ARM semantics instead,
 * where those bits gate EL1 as well, so that arrangement signs in one mode and
 * authenticates in the other and faults. Hence the two consistent choices:
 *
 *   INFERNO_HVF_PAC=on   (default)  force the enables set and never let
 *       pmap_switch clear them, so PAC is live everywhere. The kernel behaves
 *       exactly as on hardware. The cost is that a JOP-disabled task now runs
 *       with PAC genuinely on, and every pointer it never signed -- its objc
 *       class_ro pointers, its block helpers, its C++ vtables -- traps the
 *       first time a system library authenticates one.
 *
 *   INFERNO_HVF_PAC=off             leave the boot value and pmap_switch
 *       alone, so the enables stay clear for the life of the guest and every
 *       PAC instruction at both exception levels is a NOP: nothing signs,
 *       nothing authenticates, `blraa` is `blr`, `retab` is `ret`. That is a
 *       pre-8.3 CPU, and it is consistent for the same reason -- nothing in
 *       the guest has a pre-signed pointer, because Inferno applies the
 *       kernelcache's fixups unsigned and the kernel's own signer is a NOP
 *       too.
 *
 * `off` is what makes plain-arm64 binaries work, and on iOS 17 that is the
 * whole Developer Disk Image (all 22 of its binaries) plus
 * /usr/libexec/mobileassetd, the only non-arm64e binary on the system volume.
 * Under `on` they die the moment they reach a system library, and no amount of
 * patching fixes it: the combined forms (`blraa`, `braa`, `ldraa`) have no
 * one-instruction non-authenticating equivalent, so an authenticator that has
 * to tolerate both signed and unsigned pointers cannot be built.
 */
static void ck_kp_hvf_pac_patches(CKPatcherRange *kernel_text,
                                  CKPatcherRange *ppl_text)
{
    const char *pac = getenv("INFERNO_HVF_PAC");
    // movk xN, #0x7454, lsl #16 ; movk xN, #0x593d  (SCTLR_EL1 boot value)
    static const uint8_t boot_x0[] = { 0x80, 0x8A, 0xAE, 0xF2, 0xA0, 0x27, 0x8B, 0xF2 };
    static const uint8_t boot_x1[] = { 0x81, 0x8A, 0xAE, 0xF2, 0xA1, 0x27, 0x8B, 0xF2 };
    // movk xN, #0xfc54, lsl #16 ; movk xN, #0x793d  (EnIA | EnDA | EnDB added)
    static const uint8_t boot_x0_new[] = { 0x80, 0x8A, 0xBF, 0xF2, 0xA0, 0x27, 0x8F, 0xF2 };
    static const uint8_t boot_x1_new[] = { 0x81, 0x8A, 0xBF, 0xF2, 0xA1, 0x27, 0x8F, 0xF2 };
    static const uint8_t all_ff[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    // replacement mask = bits to KEEP from the original; 0 = full replace
    static const uint8_t keep_none[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    // mov w10, #0x2000 ; movk w10, #0x8800, lsl #16 (pmap_switch JOP mask)
    static const uint8_t jop_mask[] = { 0x0A, 0x00, 0x84, 0x52, 0x0A, 0x00, 0xB1, 0x72 };

    if (pac != NULL && strcmp(pac, "off") == 0) {
        if (ppl_text != NULL) {
            ck_patcher_find_callback(ppl_text,
                                     "HVF: pmap_switch never enables the PAC keys",
                                     jop_mask, all_ff, sizeof(jop_mask),
                                     sizeof(uint32_t),
                                     ck_kp_hvf_pac_never_enable_callback);
        }
        info_report("HVF: PAC keys IA/DA/DB left disabled at both exception "
                    "levels (INFERNO_HVF_PAC=off)");
        return;
    }

    if (kernel_text != NULL) {
        ck_kp_hvf_jop_always(kernel_text);
        ck_kp_hvf_sign_nojop(kernel_text);
        ck_patcher_find_replace(kernel_text, "HVF: SCTLR boot value enables all PAC keys (x0)",
                                boot_x0, all_ff, sizeof(boot_x0), sizeof(uint32_t),
                                boot_x0_new, keep_none, 0, sizeof(boot_x0_new));
        ck_patcher_find_replace(kernel_text, "HVF: SCTLR boot value enables all PAC keys (x1)",
                                boot_x1, all_ff, sizeof(boot_x1), sizeof(uint32_t),
                                boot_x1_new, keep_none, 0, sizeof(boot_x1_new));
    }
    if (ppl_text != NULL) {
        ck_patcher_find_callback(ppl_text, "HVF: pmap_switch never disables PAC keys",
                                 jop_mask, all_ff, sizeof(jop_mask), sizeof(uint32_t),
                                 ck_kp_hvf_pac_pmap_switch_callback);
        ck_kp_hvf_never_disable_jop(ppl_text);
    }
}

/*
 * Debugging aid: stop XNU from panicking when launchd dies.
 *
 * `proc_exit()` compares the exiting proc against initproc and, when they
 * match, panics "initproc exited -- exit reason namespace %d subcode 0x%llx".
 * That panic is instant, so it takes logd down with it and the last second of
 * the unified log -- exactly the second that says *why* launchd died -- is
 * never flushed, and ReportCrash never runs. Turning the `b.ne` that skips the
 * panic into an unconditional branch makes launchd's death an ordinary process
 * exit: the system is unusable afterwards, but logd keeps draining and the
 * crash report gets written.
 *
 * Off unless INFERNO_NO_INITPROC_PANIC=1 -- a guest that silently loses pid 1
 * is far more confusing than one that panics.
 */
static void ck_kp_initproc_panic_patch(CKPatcherRange *range)
{
    if (getenv("INFERNO_NO_INITPROC_PANIC") == NULL) {
        return;
    }
    /*
     * Defuse the panic block itself, not the tests that lead into it. Seven
     * different paths branch to loc_FFFFFFF00817BE18 -- initproc exited,
     * initproc failed to start, "%s[%d] exited", the LTE preinit process --
     * and they all end in the same panic call, so blocking any one entry test
     * just routes the exit through a sibling that panics with a different
     * string. Replacing the block's first instruction with a branch back to
     * the ordinary exit path (loc_FFFFFFF00817B774, where the block's own
     * tests go when they decide not to panic) covers all seven at once.
     */
    static const uint8_t pattern[] = {
        0x60, 0x92, 0x43, 0xF9, // ldr x0, [x19, #0x720]
        0x00, 0x00, 0x00, 0x94, // bl <proc name>
        0xF4, 0x03, 0x00, 0xAA, // mov x20, x0
        0x00, 0xE4, 0x00, 0x6F, // movi v0.2d, #0
    };
    static const uint8_t mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFC,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(pattern) != sizeof(mask));
    static const uint8_t repl[] = { 0x57, 0xFE, 0xFF, 0x17 }; // b -0x6A4
    ck_patcher_find_replace(range, "do not panic when initproc exits", pattern,
                            mask, sizeof(pattern), sizeof(uint32_t), repl, NULL,
                            0, sizeof(repl));
}

/*
 * Bring the guest's USB Ethernet link up when a host attaches to it.
 *
 * `AppleUSBEthernetDevice` sees us perfectly well -- it reports
 * `HostAttached = <our alt setting>` and a 100 Mb medium -- but it sits at
 * `IOLinkStatus = 1` (valid, *not* active), so it never starts its bulk reads
 * and `CARRIER_CHECK` keeps answering 0x03 where ipheth wants 0x04. Nothing
 * ever arrives. The link only goes active from
 * `AppleUSBEthernetDevice::setPropertiesGated`, which runs when guest userspace
 * calls setProperties with {"LinkStatus": 1}; on hardware that caller is the
 * tethering stack, which needs a carrier this machine does not have. There is
 * no device-tree property for it and diagnostics_relay's IORegistry request is
 * read-only, so the host cannot reach it either.
 *
 * Two edits wire it to the event that already happens:
 *
 *   1. setPropertiesGated stops parsing its dictionary and jumps straight into
 *      the LinkStatus == 1 arm with w0 = 1. Nothing else calls this method, so
 *      hard-wiring it costs nothing.
 *   2. The message() handler's "interface activated" case calls that method
 *      instead of only messaging its (nonexistent) clients. The call is guarded
 *      on the flag the handler already wrote to the stack -- 1 for attach, 0 for
 *      detach -- so unplugging still tears the link down normally.
 *
 * Both sites are matched inside the kext's own __text, because the messageClients
 * idiom in edit 2 appears verbatim in the sibling USB function drivers.
 *
 * On by default -- this is what gives the guest a network at all.
 * INFERNO_NO_USB_ETHERNET=1 turns it off.
 */
static void ck_kp_usb_ethernet_linkup_patch(CKPatcherRange *range)
{
    if (range == NULL || getenv("INFERNO_NO_USB_ETHERNET") != NULL) {
        return;
    }

    static const uint8_t gated_pattern[] = {
        0xE8, 0x03, 0x01, 0xAA, // mov x8, x1
        0xF3, 0x03, 0x00, 0xAA, // mov x19, x0        ; x19 = the driver
        0x09, 0x00, 0x00, 0x90, // adrp x9, ?
        0x29, 0x01, 0x40, 0xF9, // ldr x9, [x9, #?]
    };
    static const uint8_t gated_mask[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x1F, 0x00, 0x00, 0x9F, 0xFF, 0x03, 0xC0, 0xFF,
    };
    QEMU_BUILD_BUG_ON(sizeof(gated_pattern) != sizeof(gated_mask));
    static const uint8_t gated_repl[] = {
        0x20, 0x00, 0x80, 0x52, // mov w0, #1         ; the LinkStatus value
        0x22, 0x00, 0x00, 0x14, // b   +0x88          ; into the link-up arm
    };
    ck_patcher_find_replace(range, "USB Ethernet: force LinkStatus = 1",
                            gated_pattern, gated_mask, sizeof(gated_pattern),
                            sizeof(uint32_t), gated_repl, NULL, 8,
                            sizeof(gated_repl));

    static const uint8_t msg_pattern[] = {
        0xE2, 0x33, 0x00, 0x91, // add  x2, sp, #0xc  ; &attached
        0xE0, 0x03, 0x13, 0xAA, // mov  x0, x19
        0x21, 0x40, 0x90, 0x52, // mov  w1, #0x8201
        0xE1, 0x7F, 0xBC, 0x72, // movk w1, #0xE3FF, lsl #16
        0x83, 0x00, 0x80, 0x52, // mov  w3, #4
        0xF1, 0x03, 0x08, 0xAA, // mov  x17, x8
        0x91, 0x78, 0xEC, 0xF2, // movk x17, #0x63C4, lsl #48
        0x31, 0x09, 0x3F, 0xD7, // blraa x9, x17      ; messageClients()
    };
    static const uint8_t msg_repl[] = {
        0xE8, 0x0F, 0x40, 0xB9, // ldr  w8, [sp, #0xc]
        0xE8, 0x00, 0x00, 0x34, // cbz  w8, +0x1c     ; detach: leave it alone
        0xE0, 0x03, 0x13, 0xAA, // mov  x0, x19
        0xE1, 0x03, 0x1F, 0xAA, // mov  x1, xzr       ; the dictionary is ignored
        0x1E, 0x00, 0x00, 0x94, // bl   setPropertiesGated
        0x1F, 0x20, 0x03, 0xD5, // nop
        0x1F, 0x20, 0x03, 0xD5, // nop
        0x1F, 0x20, 0x03, 0xD5, // nop
    };
    ck_patcher_find_replace(range, "USB Ethernet: link up on host attach",
                            msg_pattern, NULL, sizeof(msg_pattern),
                            sizeof(uint32_t), msg_repl, NULL, 0,
                            sizeof(msg_repl));
}

/*
 * Debugging aid: stop the kernel running out of corpses.
 *
 * XNU allows five in-flight corpses at a time; past that it refuses to make
 * one, logs "Corpse failure, too many 5", and ReportCrash never sees the crash
 * -- so **no crash report is written at all**. On this guest something
 * (mediaserverd) crash-loops every few seconds, so the budget is gone within
 * seconds of boot and every later crash is invisible. That is how a crashing
 * DDI daemon looked like a service that simply never answered.
 *
 * Raise the ceiling to 255. Off unless INFERNO_MORE_CORPSES=1: a guest that
 * keeps hundreds of corpses alive is not one you want by default.
 */
static void ck_kp_corpse_limit_patch(CKPatcherRange *range)
{
    if (getenv("INFERNO_MORE_CORPSES") == NULL) {
        return;
    }
    static const uint8_t pattern[] = {
        0x0B, 0x7D, 0x10, 0x53, // lsr  w11, w8, #0x10   ; in-flight corpses
        0x75, 0x05, 0x00, 0x11, // add  w21, w11, #1
        0x7F, 0x15, 0x00, 0x71, // cmp  w11, #5
        0xC2, 0x03, 0x00, 0x54, // b.hs <refuse>
    };
    static const uint8_t repl[] = { 0x7F, 0xFD, 0x03, 0x71 }; // cmp w11, #0xFF
    ck_patcher_find_replace(range, "raise the corpse limit", pattern, NULL,
                            sizeof(pattern), sizeof(uint32_t), repl, NULL, 8,
                            sizeof(repl));
}

void ck_patch_kernel(MachoHeader64 *hdr)
{
    MachoHeader64 *apfs_hdr;
    g_autofree CKPatcherRange *apfs_text;
    g_autofree CKPatcherRange *apfs_cstring;
    g_autofree CKPatcherRange *amfi_text;
    g_autofree CKPatcherRange *sep_mgr_text;
    g_autofree CKPatcherRange *img4_text;
    g_autofree CKPatcherRange *kernel_text;
    g_autofree CKPatcherRange *kernel_const;
    g_autofree CKPatcherRange *kernel_ppltext;

    apfs_hdr = ck_kp_find_image_header(hdr, "com.apple.filesystems.apfs");
    apfs_text = ck_kp_find_section_range(apfs_hdr, "__TEXT_EXEC", "__text");
    ck_kp_apfs_patches(apfs_text);
#ifndef ENABLE_DATA_ENCRYPTION
    ck_kp_apfs_allow_unencrypted_data(apfs_text);
    ck_kp_apfs_allow_class_open(apfs_text);
#endif
    apfs_cstring = ck_kp_find_section_range(apfs_hdr, "__TEXT", "__cstring");
    if (apfs_cstring == NULL) {
        apfs_cstring = ck_kp_find_section_range(hdr, "__TEXT", "__cstring");
    }
    ck_kp_apfs_snapshot_patch(apfs_cstring);

    amfi_text =
        ck_kp_find_image_text(hdr, "com.apple.driver.AppleMobileFileIntegrity");
    ck_kp_amfi_patches(amfi_text);

    sep_mgr_text =
        ck_kp_find_image_text(hdr, "com.apple.driver.AppleSEPManager");
    ck_kp_sep_mgr_patches(sep_mgr_text);

    img4_text = ck_kp_find_image_text(hdr, "com.apple.security.AppleImage4");
    ck_kp_img4_patches(img4_text);

    ck_kp_usb_ethernet_linkup_patch(
        ck_kp_find_image_text(hdr, "com.apple.driver.AppleUSBEthernetDevice"));

    kernel_text = ck_kp_get_kernel_section(hdr, "__TEXT_EXEC", "__text");
    ck_kp_mac_mount_patch(kernel_text);
    ck_kp_initproc_panic_patch(kernel_text);
    ck_kp_corpse_limit_patch(kernel_text);
    ck_kp_kprintf_patch(kernel_text);
    ck_kp_amx_patch(kernel_text);
    ck_kp_cs_patches(kernel_text);
    kernel_const = ck_kp_get_kernel_section(hdr, "__TEXT", "__const");
    ck_kp_hactivation_patch(kernel_const);

    kernel_ppltext = ck_kp_find_section_range(hdr, "__PPLTEXT", "__text");
    if (kernel_ppltext == NULL) {
        warn_report("Failed to find `__PPLTEXT.__text`.");
        ck_kp_tc_patch(kernel_text);
        ck_kp_pmap_cs_enforce_patch(kernel_text);
    } else {
        ck_kp_tc_patch(kernel_ppltext);
        ck_kp_pmap_cs_enforce_patch(kernel_ppltext);
    }

    if (hvf_enabled()) {
        ck_kp_gxf_hvc_patch(hdr);
        if (gxf_hvf_xprr_remap_enabled()) {
            /* pmap lives in __PPLTEXT; fall back to __TEXT_EXEC if absent. */
            ck_kp_hvf_ppl_xprr_patch(kernel_ppltext ?: kernel_text);
        }
        ck_kp_hvf_pac_patches(kernel_text, kernel_ppltext);
        ck_kp_hvf_tpro_patch(kernel_text);
        ck_kp_hvf_disable_hw_tpro(kernel_text, kernel_ppltext);
        }
}
