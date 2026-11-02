/* aarch64_link.c : AArch64 relocation application over the shared ELF64 layout
 * core.  Layout, symbol resolution and the undefined-symbol check are the
 * shared ld_layout()/ld_check_undefined() from link.c; this file supplies the
 * AArch64 relocation set and the built-in default script.
 *
 * Unlike x86-64, most AArch64 relocations patch a bit field inside a 32-bit
 * instruction (the 26-bit branch, the 19-bit conditional branch, the ADR/ADRP
 * split immediate, the ADD lo12), so each has its own encoder. */

#include "elf64_ld.h"

#include <stdlib.h>
#include <string.h>

static uint32_t
rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void
wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

/* B / BL: imm26 = disp>>2 in bits [25:0]. */
static void
patch_branch26(uint8_t *p, int64_t disp)
{
    uint32_t insn = rd32(p) & 0xfc000000u;
    insn |= (uint32_t)((disp >> 2) & 0x03ffffff);
    wr32(p, insn);
}

/* B.cond / CBZ / TBZ(19): imm19 = disp>>2 in bits [23:5]. */
static void
patch_condbr19(uint8_t *p, int64_t disp)
{
    uint32_t insn = rd32(p) & ~(0x7ffffu << 5);
    insn |= (uint32_t)(((disp >> 2) & 0x7ffff) << 5);
    wr32(p, insn);
}

/* ADR / ADRP: a 21-bit immediate split into immlo (bits [30:29]) and immhi
 * (bits [23:5]).  ADR takes the byte displacement, ADRP the page count. */
static void
patch_adr21(uint8_t *p, int64_t imm)
{
    uint32_t insn = rd32(p) & ~((3u << 29) | (0x7ffffu << 5));
    uint32_t lo = (uint32_t)(imm & 3);
    uint32_t hi = (uint32_t)((imm >> 2) & 0x7ffff);
    insn |= (lo << 29) | (hi << 5);
    wr32(p, insn);
}

/* ADD (immediate): imm12 in bits [21:10]. */
static void
patch_add_lo12(uint8_t *p, uint32_t imm12)
{
    uint32_t insn = rd32(p) & ~(0xfffu << 10);
    insn |= (imm12 & 0xfff) << 10;
    wr32(p, insn);
}

static int64_t
page(int64_t x)
{
    return x & ~(int64_t)0xfff;
}

int
aarch64_ld_link(struct linker *ld)
{
    ld_layout(ld);

    for (int oi = 0; oi < ld->nobjs; oi++) {
        struct ld_object *obj = &ld->objs[oi];

        for (int ri = 0; ri < obj->nrelocs; ri++) {
            struct ld_reloc *rel = &obj->relocs[ri];

            if (rel->type == R_AARCH64_NONE)
                continue;

            struct ld_input_sec *isec = &obj->sections[rel->section];
            if (!isec->matched)
                continue;
            struct ld_output_sec *os = ld_find_output_sec(ld, isec);
            if (!os)
                continue;   /* the section was discarded; its relocs are moot */

            int gi = obj->sym_map[rel->sym_idx];
            if (gi < 0)
                die("%s: reloc references unmapped symbol %d",
                    obj->path, rel->sym_idx);
            struct ld_symbol *gsym = &ld->syms[gi];
            if (!gsym->defined && !gsym->weak)
                die("%s: undefined symbol '%s'", obj->path, gsym->name);
            /* a weak-undefined symbol has value 0, so the reloc resolves to 0 */

            uint32_t file_off = (isec->assigned_vaddr - os->vaddr) +
                                rel->offset;
            uint8_t *patch = os->data + file_off;

            uint64_t S = gsym->value;
            int64_t A = rel->addend;
            uint64_t P = isec->assigned_vaddr + rel->offset;

            switch (rel->type) {
            case R_AARCH64_ABS64:
                wr64(patch, S + (uint64_t)A);
                break;
            case R_AARCH64_ABS32: {
                uint64_t v = S + (uint64_t)A;
                if (v > 0xffffffffULL)
                    die("%s: R_AARCH64_ABS32 overflow for '%s'",
                        obj->path, gsym->name);
                wr32(patch, (uint32_t)v);
                break;
            }
            case R_AARCH64_PREL32: {
                int64_t v = (int64_t)(S + (uint64_t)A) - (int64_t)P;
                if (v < -2147483648LL || v > 2147483647LL)
                    die("%s: R_AARCH64_PREL32 out of range for '%s'",
                        obj->path, gsym->name);
                wr32(patch, (uint32_t)(int32_t)v);
                break;
            }
            case R_AARCH64_CALL26:
            case R_AARCH64_JUMP26: {
                int64_t disp = (int64_t)(S + (uint64_t)A) - (int64_t)P;
                if (disp < -(1LL << 27) || disp >= (1LL << 27))
                    die("%s: %s out of range for '%s' (%lld)", obj->path,
                        rel->type == R_AARCH64_CALL26 ? "R_AARCH64_CALL26"
                                                      : "R_AARCH64_JUMP26",
                        gsym->name, (long long)disp);
                patch_branch26(patch, disp);
                break;
            }
            case R_AARCH64_CONDBR19: {
                int64_t disp = (int64_t)(S + (uint64_t)A) - (int64_t)P;
                if (disp < -(1LL << 20) || disp >= (1LL << 20))
                    die("%s: R_AARCH64_CONDBR19 out of range for '%s' (%lld)",
                        obj->path, gsym->name, (long long)disp);
                patch_condbr19(patch, disp);
                break;
            }
            case R_AARCH64_ADR_PREL_LO21: {
                int64_t disp = (int64_t)(S + (uint64_t)A) - (int64_t)P;
                if (disp < -(1LL << 20) || disp >= (1LL << 20))
                    die("%s: R_AARCH64_ADR_PREL_LO21 out of range for '%s'",
                        obj->path, gsym->name);
                patch_adr21(patch, disp);
                break;
            }
            case R_AARCH64_ADR_PREL_PG_HI21: {
                int64_t x = page((int64_t)(S + (uint64_t)A)) - page((int64_t)P);
                int64_t imm = x >> 12;
                if (imm < -(1LL << 20) || imm >= (1LL << 20))
                    die("%s: R_AARCH64_ADR_PREL_PG_HI21 out of range for '%s'",
                        obj->path, gsym->name);
                patch_adr21(patch, imm);
                break;
            }
            case R_AARCH64_ADD_ABS_LO12_NC:
                patch_add_lo12(patch, (uint32_t)((S + (uint64_t)A) & 0xfff));
                break;
            default:
                die("%s: unsupported relocation type %d", obj->path,
                    rel->type);
            }
        }
    }

    /* Debug sections are non-alloc, so the loop above skipped them.  Merge them
       and apply the absolute address relocations (DWARF low_pc / set_address,
       R_AARCH64_ABS64 or ABS32 against the now-resolved text symbol) into the
       merged buffers, so the executable carries final addresses. */
    ld_build_debug(ld);
    for (int oi = 0; oi < ld->nobjs; oi++) {
        struct ld_object *obj = &ld->objs[oi];

        for (int ri = 0; ri < obj->nrelocs; ri++) {
            struct ld_reloc *rel = &obj->relocs[ri];
            struct ld_input_sec *isec = &obj->sections[rel->section];
            struct ld_debug_sec *d = NULL;
            struct ld_symbol *gsym;
            uint8_t *patch;
            int gi;

            if (rel->type == R_AARCH64_NONE)
                continue;
            if (isec->type != SHT_PROGBITS
                || strncmp(isec->name, ".debug_", 7) != 0)
                continue;
            for (int k = 0; k < ld->ndebug; k++) {
                if (strcmp(ld->debug[k].name, isec->name) == 0) {
                    d = &ld->debug[k];
                    break;
                }
            }
            if (!d)
                continue;

            gi = obj->sym_map[rel->sym_idx];
            if (gi < 0)
                die("%s: debug reloc references unmapped symbol", obj->path);
            gsym = &ld->syms[gi];
            patch = d->data + isec->assigned_vaddr + rel->offset;

            switch (rel->type) {
            case R_AARCH64_ABS64:
                wr64(patch, (uint64_t)gsym->value + (uint64_t)rel->addend);
                break;
            case R_AARCH64_ABS32:
                wr32(patch, (uint32_t)((uint64_t)gsym->value +
                                       (uint64_t)rel->addend));
                break;
            default:
                die("%s: unsupported debug relocation type %d in %s",
                    obj->path, rel->type, isec->name);
            }
        }
    }

    ld_check_undefined(ld);
    return 0;
}

/****************************************************************
 * Built-in AArch64 user-mode linker script
 ****************************************************************/

void
aarch64_default_script(struct arena *a, struct ld_script *script)
{
    static const char default_script[] =
        "OUTPUT_FORMAT(\"elf64-littleaarch64\")\n"
        "OUTPUT_ARCH(aarch64)\n"
        "ENTRY(_start)\n"
        "MEMORY { RAM (rwx) : ORIGIN = 0x00400000, LENGTH = 0x4000000 }\n"
        "SECTIONS {\n"
        "    .text : { *(.text .text.*) } > RAM\n"
        "    .rodata : { *(.rodata .rodata.*) } > RAM\n"
        "    .data : { *(.data .data.*) } > RAM\n"
        "    .bss : { __bss_start = .; *(.bss .bss.*)"
        " *(COMMON) __bss_end = .; } > RAM\n"
        "    /DISCARD/ : { *(.note .note.*) *(.comment)"
        " *(.eh_frame) *(.gnu.hash) }\n"
        "}\n";

    ld_parse_script(a, script, default_script);
}
