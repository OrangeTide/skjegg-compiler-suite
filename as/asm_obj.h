/* asm_obj.h : arch-neutral object bookkeeping shared by the assemblers.
 *
 * Sections, symbols and relocations, plus the byte-emission and symbol-table
 * helpers.  The section emitter is endianness-aware (a per-section flag) so
 * the same helpers serve the big-endian m68k output and the little-endian
 * RISC-V output.  A relocation carries a target-specific type code; the m68k
 * ELF writer ignores it (it emits a single R_68K_32), the RISC-V writer keys
 * on it. */

#ifndef ASM_OBJ_H
#define ASM_OBJ_H

#include <stdint.h>

#include "arena.h"

/****************************************************************
 * Sections
 ****************************************************************/

struct reloc {
    uint32_t offset;
    int sym_idx;
    int32_t addend;
    int type;               /* target relocation type (0 = default) */
};

/* Section class, for the per-function sections an assembler emits under
   -ffunction-sections (.text.<fn>, .rodata.<fn>, ...): it fixes the ELF flags
   and type (text = AX PROGBITS, rodata = A PROGBITS, data = WA PROGBITS, bss =
   WA NOBITS) for a dynamically named section, the way the fixed base sections
   are hardcoded in each ELF writer. */
enum sec_kind {
    SECK_TEXT,
    SECK_RODATA,
    SECK_DATA,
    SECK_BSS,
};

struct section {
    uint8_t *data;
    int len;
    int cap;
    int align;              /* max byte alignment requested (0 = default) */
    int little_endian;      /* byte order for sec_emit16/32 (0 = big) */
    struct reloc *relocs;
    int nrelocs;
    int reloc_cap;
    const char *name;       /* ELF name of an extra named section (.text.<fn>);
                               NULL for a fixed base section the writer names */
    int kind;               /* enum sec_kind, for a named section's ELF flags */
};

void sec_emit8(struct section *s, uint8_t val);
void sec_emit16(struct section *s, uint16_t val);
void sec_emit32(struct section *s, uint32_t val);
void sec_align(struct section *s, int alignment);
void sec_space(struct section *s, int nbytes);
void sec_add_reloc(struct section *s, uint32_t offset, int sym_idx,
                   int32_t addend);
void sec_add_reloc_t(struct section *s, uint32_t offset, int sym_idx,
                     int32_t addend, int type);

/****************************************************************
 * Symbols
 ****************************************************************/

struct symbol {
    const char *name;
    int section;
    uint32_t value;
    int global;
    int weak;           /* STB_WEAK binding (a weak definition or reference) */
    int defined;
    int is_func;        /* STT_FUNC type (set by .type sym,@function) */
    uint32_t size;      /* st_size (set by .size); 0 if unset */
};

struct symtab {
    struct symbol *syms;
    int nsyms;
    int cap;
    int *hash;          /* name index: open-addressing bucket -> sym index + 1
                           (0 marks an empty bucket); NULL until the first add */
    int hash_cap;       /* bucket count, a power of two, 0 until the first add */
    struct arena *arena;
};

int sym_lookup(struct symtab *st, const char *name);
int sym_add(struct symtab *st, const char *name);
void sym_define(struct symtab *st, int idx, int section, uint32_t value);
void sym_set_global(struct symtab *st, int idx);
void sym_set_weak(struct symtab *st, int idx);
void sym_set_func(struct symtab *st, int idx);
void sym_set_size(struct symtab *st, int idx, uint32_t size);

#endif
