/* aarch64.h : the in-tree AArch64 assembler (skj-as-aarch64).
 *
 * The fifth member of the as/ family (after ColdFire, RISC-V, MIPS, x86-64).  It
 * reads the native GNU `aarch64-linux-gnu-as` GAS syntax the arm64 backends emit
 * (backend/arm64_emit.c stack ABI + backend/arm64_select.c psABI, and the
 * hand-written runtime/start_arm64*.S) and writes ELF64 little-endian EM_AARCH64
 * relocatable objects.  Linking stays external (GNU ld / gcc); there is no arm64
 * skj-ld.
 *
 * AArch64 instructions are one fixed 32-bit little-endian word, so an
 * instruction's length never depends on its operands: the two passes emit the
 * same bytes and there is no branch-relaxation fixpoint.  A branch or adr to a
 * symbol defined in the current section resolves to a direct displacement; any
 * other target becomes a relocation.
 *
 * The bitfield recipes mirror the JIT encoder jit/emit_arm64.c, which is checked
 * byte-for-byte against aarch64-linux-gnu-as (tests/arm64_oracle.c), so the
 * assembler, the JIT, and the oracle agree by construction.
 */

#ifndef SKJ_AARCH64_H
#define SKJ_AARCH64_H

#include <stdint.h>
#include <stdio.h>

#include "asm_lex.h"
#include "asm_obj.h"
#include "arena.h"

/* ELF relocation types (AArch64) and machine id.  The assembler records the
   ELF code as the neutral reloc->type; the ELF writer emits it directly and the
   Mach-O writer maps it to an ARM64_RELOC_* kind.  ADR_PREL_PG_HI21 and
   ADD_ABS_LO12_NC are recorded for the Mach-O adrp/add @PAGE/@PAGEOFF pair; the
   ELF backend never emits adrp, so the ELF writer never sees them. */
#define R_AARCH64_ABS64        257
#define R_AARCH64_ABS32        258
#define R_AARCH64_PREL32       261
#define R_AARCH64_ADR_PREL_LO21 274
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADD_ABS_LO12_NC  277
#define R_AARCH64_CONDBR19     280
#define R_AARCH64_JUMP26       282
#define R_AARCH64_CALL26       283
#define EM_AARCH64             183

/* Sections.  The bounded input uses only these four; no DWARF (no .loc). */
enum a64_section_id {
    ASEC_TEXT,
    ASEC_RODATA,
    ASEC_DATA,
    ASEC_BSS,
    ASEC_COUNT,
};

/* Register files.  A_ZR (31) is the zero register / sp depending on context. */
enum a64_regclass { A_RC_NONE, A_RC_GPR, A_RC_FP };
#define A64_ZR 31
#define A64_LR 30

/* A parsed operand.  AArch64 memory is [base], [base, #imm], or [base, Xm]. */
enum a64_op_kind {
    AO_NONE,
    AO_REG,     /* Wn/Xn/Dn/Sn/Hn */
    AO_IMM,     /* #imm */
    AO_MEM,     /* [base {, #off | , Xm}] */
    AO_SYM,     /* a branch / adr target (bare label or symbol) */
    AO_LIT,     /* =const or =sym : a literal-pool load operand */
    AO_COND,    /* a condition code (cset) */
    AO_SHIFT,   /* lsl #12 modifier on an add/sub immediate */
};

struct a64_operand {
    int kind;                   /* AO_* */
    int rclass;                 /* AO_REG: A_RC_GPR or A_RC_FP */
    int reg;                    /* AO_REG: 0..31 */
    int size;                   /* reg width in bytes: 8/4 (gpr), 8/4/2 (fp) */
    int is_sp;                  /* AO_REG: the token was sp/wsp (not the zero reg) */
    /* AO_MEM */
    int base;                   /* base register, or -1 */
    int index;                  /* index register, or -1 */
    long disp;                  /* AO_MEM offset / AO_IMM value / AO_LIT const */
    int lsl12;                  /* AO_IMM: a trailing `, lsl #12` */
    int cond;                   /* AO_COND: condition code 0..15 */
    /* a symbol (AO_SYM target, AO_LIT `=sym`, or a data symbol): */
    const char *sym;
    int32_t add;
    int is_sym;
    int pageref;                /* Mach-O relocation suffix: 1 = @PAGE, 2 = @PAGEOFF */
};

#define A64_MAXOPS 4

/* A unique pending literal-pool entry, between `ldr =v` and the next `.ltorg`.
   GNU as deduplicates identical literals, so one entry may be referenced by
   several ldr-literal instructions (see a64_litref). */
struct a64_litent {
    int size;                   /* 4 or 8 bytes */
    int is_sym;
    long value;                 /* numeric literal */
    const char *sym;            /* symbol literal (=sym), ABS64 */
    int32_t add;
    uint32_t slot;              /* section offset where the entry lands (flush) */
};

/* One ldr-literal instruction referencing a pending pool entry. */
struct a64_litref {
    uint32_t insn_off;          /* the ldr-literal instruction's section offset */
    int entry;                  /* index into a64_asm.lits */
};

struct a64_asm {
    struct lexer lex;
    struct section sections[ASEC_COUNT];
    int cur_section;
    struct symtab st;
    int pass;
    struct arena arena;
    /* numeric local labels (1:/1f/1b) */
    struct { int num; int count; } *numlabels;
    int n_numlabels;
    int numlabel_cap;
    /* literal pool (flushed at .ltorg and at end of the text section) */
    struct a64_litent *lits;
    int nlits;
    int lit_cap;
    struct a64_litref *litrefs;
    int nlitrefs;
    int litref_cap;
};

/* Lifecycle and the two-pass driver (as/aarch64_parse.c). */
void a64_init(struct a64_asm *a, const char *src);
void a64_free(struct a64_asm *a);
void a64_assemble(struct a64_asm *a);

/* Encode one instruction into the current section (as/aarch64_encode.c).  In
   pass 1 (emit==0) it only lays out bytes (placeholders); in pass 2 (emit==1)
   it writes the final words and records relocations.  Both passes compute the
   same length (one 32-bit word, or a fixed multiple for a pseudo). */
void a64_encode(struct a64_asm *a, const char *mnem,
                struct a64_operand *ops, int nops, int emit);

/* Flush accumulated literal-pool entries into the current section (.ltorg and
   end of text), fixing each referencing instruction's imm19 and recording an
   ABS64 relocation for every `=symbol` entry. */
void a64_flush_litpool(struct a64_asm *a, int emit);

/* Record a `ldr Rt, =v` literal: finds or appends a deduplicated pool entry and
   records this instruction as a reference to it; the encoder has already emitted
   the placeholder LDR-literal word at insn_off. */
void a64_ref_lit(struct a64_asm *a, int size, int is_sym, long value,
                 const char *sym, int32_t add, uint32_t insn_off);

/* The ELF64 writer (as/aarch64_elf.c). */
void a64_elf_write(struct a64_asm *a, FILE *out);

/* The Mach-O (MH_OBJECT) writer (as/aarch64_macho.c), for the Darwin target. */
void a64_macho_write(struct a64_asm *a, FILE *out);

/* Name lookups (as/aarch64_parse.c), shared with the encoder.  a64_reg returns
   the class (A_RC_*), sets num/size/is_sp, or A_RC_NONE.  a64_cond returns a
   condition code 0..15, or -1. */
int a64_reg(const char *name, int *num, int *size, int *is_sp);
int a64_cond(const char *name);

#endif /* SKJ_AARCH64_H */
