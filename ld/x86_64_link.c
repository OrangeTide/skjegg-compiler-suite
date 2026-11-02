/* x86_64_link.c : x86-64 relocation application over the shared ELF64 layout
 * core.  Layout, symbol resolution and the undefined-symbol check are the
 * shared ld_layout()/ld_check_undefined() from link.c; this file supplies the
 * x86-64 relocation set and the built-in default script.
 *
 * All x86-64 relocations emitted by skj-as-x86-64 are plain byte patches (no
 * instruction-field packing): a 4- or 8-byte little-endian store of an
 * absolute or PC-relative value. */

#include "elf64_ld.h"

#include <stdlib.h>
#include <string.h>

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

int
x86_64_ld_link(struct linker *ld)
{
    ld_layout(ld);

    for (int oi = 0; oi < ld->nobjs; oi++) {
        struct ld_object *obj = &ld->objs[oi];

        for (int ri = 0; ri < obj->nrelocs; ri++) {
            struct ld_reloc *rel = &obj->relocs[ri];

            if (rel->type == R_X86_64_NONE)
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
            case R_X86_64_64:
                wr64(patch, S + (uint64_t)A);
                break;
            case R_X86_64_32:
                wr32(patch, (uint32_t)(S + (uint64_t)A));
                break;
            case R_X86_64_32S: {
                int64_t v = (int64_t)S + A;
                if (v < -2147483648LL || v > 2147483647LL)
                    die("%s: R_X86_64_32S overflow for '%s' (%lld)",
                        obj->path, gsym->name, (long long)v);
                wr32(patch, (uint32_t)(int32_t)v);
                break;
            }
            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                int64_t v = (int64_t)(S + (uint64_t)A) - (int64_t)P;
                if (v < -2147483648LL || v > 2147483647LL)
                    die("%s: %s out of range for '%s' (%lld)", obj->path,
                        rel->type == R_X86_64_PC32 ? "R_X86_64_PC32"
                                                   : "R_X86_64_PLT32",
                        gsym->name, (long long)v);
                wr32(patch, (uint32_t)(int32_t)v);
                break;
            }
            default:
                die("%s: unsupported relocation type %d", obj->path,
                    rel->type);
            }
        }
    }

    /* Debug sections are non-alloc, so the loop above skipped them.  Merge them
       and apply the absolute address relocations (DWARF low_pc / set_address,
       R_X86_64_64 or R_X86_64_32 against the now-resolved text symbol) into the
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

            if (rel->type == R_X86_64_NONE)
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
            case R_X86_64_64:
                wr64(patch, (uint64_t)gsym->value + (uint64_t)rel->addend);
                break;
            case R_X86_64_32:
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
 * Built-in x86-64 user-mode linker script
 ****************************************************************/

void
x86_64_default_script(struct arena *a, struct ld_script *script)
{
    static const char default_script[] =
        "OUTPUT_FORMAT(\"elf64-x86-64\")\n"
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
