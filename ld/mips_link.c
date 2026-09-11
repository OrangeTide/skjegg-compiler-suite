/* mips_link.c : MIPS relocation application over the shared layout core.
 *
 * Uses ld_layout / ld_find_output_sec / ld_check_undefined from link.c, then
 * applies the MIPS o32 relocation set.  MIPS uses REL, so each addend is read
 * from the relocated field rather than the relocation entry.  R_MIPS_HI16 and
 * R_MIPS_LO16 come in pairs whose addends combine (AHL = (HI << 16) +
 * sign-extend(LO)), so a HI16 is resolved together with the next LO16. */

#include "mips_ld.h"

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

/* The output pointer for a relocation's field, or NULL if its section was
 * discarded.  Sets *P to the field's runtime address when P is non-NULL. */
static uint8_t *
reloc_patch(struct linker *ld, struct ld_object *obj, struct ld_reloc *rel,
            uint32_t *P)
{
    struct ld_input_sec *isec = &obj->sections[rel->section];
    struct ld_output_sec *os;
    uint32_t file_off;

    if (!isec->matched)
        return NULL;
    os = ld_find_output_sec(ld, isec);
    if (!os)
        return NULL;
    file_off = (isec->assigned_vaddr - os->vaddr) + rel->offset;
    if (P)
        *P = isec->assigned_vaddr + rel->offset;
    return os->data + file_off;
}

/* The resolved value of a relocation's symbol. */
static uint32_t
reloc_symval(struct linker *ld, struct ld_object *obj, struct ld_reloc *rel)
{
    int gi = obj->sym_map[rel->sym_idx];
    struct ld_symbol *gsym;

    if (gi < 0)
        die("%s: reloc references unmapped symbol %d", obj->path, rel->sym_idx);
    gsym = &ld->syms[gi];
    if (!gsym->defined)
        die("%s: undefined symbol '%s'", obj->path, gsym->name);
    return gsym->value;
}

int
mips_ld_link(struct linker *ld)
{
    ld_layout(ld);

    for (int oi = 0; oi < ld->nobjs; oi++) {
        struct ld_object *obj = &ld->objs[oi];

        for (int ri = 0; ri < obj->nrelocs; ri++) {
            struct ld_reloc *rel = &obj->relocs[ri];
            uint8_t *patch;
            uint32_t S, P, field;
            int32_t A;

            if (rel->type == R_MIPS_NONE)
                continue;
            patch = reloc_patch(ld, obj, rel, &P);
            if (!patch)
                continue;               /* section discarded */
            S = reloc_symval(ld, obj, rel);
            field = rd32(patch);

            switch (rel->type) {
            case R_MIPS_32:
                wr32(patch, S + field);
                break;

            case R_MIPS_16:
                wr32(patch, (field & 0xffff0000)
                    | ((S + (uint32_t)(int16_t)(field & 0xffff)) & 0xffff));
                break;

            case R_MIPS_26: {
                uint32_t target;
                A = (int32_t)((field & 0x3ffffff) << 2);
                if (((S + (uint32_t)A) >> 28) != ((P + 4) >> 28))
                    die("%s: R_MIPS_26 target out of 256MB region", obj->path);
                target = (S + (uint32_t)A) >> 2;
                wr32(patch, (field & ~0x3ffffffu) | (target & 0x3ffffff));
                break;
            }

            case R_MIPS_PC16: {
                int32_t disp;
                A = (int32_t)(int16_t)(field & 0xffff) << 2;
                disp = (int32_t)(S + (uint32_t)A - P);
                if (disp < -0x20000 || disp > 0x1ffff)
                    die("%s: R_MIPS_PC16 out of range (%d)", obj->path, disp);
                wr32(patch, (field & 0xffff0000)
                    | (((uint32_t)(disp >> 2)) & 0xffff));
                break;
            }

            case R_MIPS_HI16: {
                /* AHL = (HI << 16) + sign_extend(LO), where LO is the addend
                 * of the next LO16 in the object; the high half carries the
                 * +0x8000 rounding.  Only the HI16 field is patched here: the
                 * LO16 field's low 16 bits are (S + AHL) & 0xffff, which equal
                 * (S + sign_extend(LO)) & 0xffff (the HI part clears), so each
                 * LO16 resolves itself in the R_MIPS_LO16 case below.  Reading
                 * rather than writing the LO16 lets several HI16 share one LO16
                 * (an o32 HI16 stack), since the LO16 field stays its addend
                 * until its own reloc is processed (it follows every HI16). */
                uint8_t *lo_patch = NULL;
                uint32_t lo_field = 0, value;
                int32_t ahl;

                for (int rj = ri + 1; rj < obj->nrelocs; rj++) {
                    if (obj->relocs[rj].type == R_MIPS_LO16) {
                        lo_patch = reloc_patch(ld, obj, &obj->relocs[rj], NULL);
                        break;
                    }
                }
                if (lo_patch)
                    lo_field = rd32(lo_patch);

                ahl = (int32_t)((field & 0xffff) << 16)
                    + (int32_t)(int16_t)(lo_field & 0xffff);
                value = S + (uint32_t)ahl;

                wr32(patch, (field & 0xffff0000)
                    | (((value + 0x8000) >> 16) & 0xffff));
                break;
            }

            case R_MIPS_LO16: {
                uint32_t value = S + (uint32_t)(int16_t)(field & 0xffff);
                wr32(patch, (field & 0xffff0000) | (value & 0xffff));
                break;
            }

            default:
                die("%s: unsupported relocation type %d", obj->path, rel->type);
            }
        }
    }

    ld_check_undefined(ld);
    return 0;
}

/****************************************************************
 * Built-in linker script: qemu-mipsel user mode, text base 0x00400000
 ****************************************************************/

void
mips_default_script(struct arena *a, struct ld_script *script)
{
    /* Two program headers: a PT_MIPS_ABIFLAGS (the writer overrides the first
     * entry's type) followed by the loadable image.  The block is declared so
     * ld_layout reserves file/address space for both, keeping the file-offset
     * to vaddr congruence the single-segment writer relies on. */
    static const char default_script[] =
        "OUTPUT_FORMAT(\"elf32-tradlittlemips\")\n"
        "OUTPUT_ARCH(mips)\n"
        "ENTRY(_start)\n"
        "MEMORY { RAM (rwx) : ORIGIN = 0x00400000, LENGTH = 0x4000000 }\n"
        "PHDRS {\n"
        "    abiflags PT_LOAD FLAGS(4);\n"
        "    load PT_LOAD FLAGS(7);\n"
        "}\n"
        "SECTIONS {\n"
        "    .text : { *(.text .text.*) } > RAM\n"
        "    .rodata : { *(.rodata .rodata.*) } > RAM\n"
        "    .data : { *(.data .data.*) } > RAM\n"
        "    .bss : { __bss_start = .; *(.sbss .sbss.*) *(.bss .bss.*)"
        " *(COMMON) __bss_end = .; } > RAM\n"
        "    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr)"
        " *(.gnu.attributes) *(.mdebug.*) *(.note .note.*) *(.comment)"
        " *(.eh_frame) }\n"
        "}\n";

    ld_parse_script(a, script, default_script);
}

/****************************************************************
 * Built-in linker script for the PlayStation PS-EXE output
 *
 * Main RAM base is 0x80010000 (KSEG0, the address BIOS-loaded programs use;
 * the 0x10000 leaves the BIOS scratch and kernel area below it untouched).
 * No PHDRS or abiflags: the PS-EXE writer serializes a flat image, not ELF.
 ****************************************************************/

void
mips_default_psx_script(struct arena *a, struct ld_script *script)
{
    static const char default_script[] =
        "OUTPUT_ARCH(mips)\n"
        "ENTRY(_start)\n"
        "MEMORY { RAM (rwx) : ORIGIN = 0x80010000, LENGTH = 0x1f0000 }\n"
        "SECTIONS {\n"
        "    .text : { *(.text .text.*) } > RAM\n"
        "    .rodata : { *(.rodata .rodata.*) } > RAM\n"
        "    .data : { *(.data .data.*) } > RAM\n"
        "    .bss : { __bss_start = .; *(.sbss .sbss.*) *(.bss .bss.*)"
        " *(COMMON) __bss_end = .; } > RAM\n"
        "    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr)"
        " *(.gnu.attributes) *(.mdebug.*) *(.note .note.*) *(.comment)"
        " *(.eh_frame) }\n"
        "}\n";

    ld_parse_script(a, script, default_script);
}
