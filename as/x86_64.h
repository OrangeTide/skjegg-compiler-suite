/* x86_64.h : the in-tree x86-64 assembler (skj-as-x86-64).
 *
 * The fourth member of the as/ family (after ColdFire, RISC-V, MIPS).  It reads
 * the GNU `.intel_syntax noprefix` subset the toolkit's x86-64 backends emit
 * (backend/mc_text.c + x86_aot.c, backend/x86_emit.c, and the hand-written
 * runtime/start_x86_64*.s) and writes ELF64 little-endian relocatable objects.
 * Linking stays external (GNU ld / gcc -no-pie); there is no x86-64 skj-ld.
 *
 * It reuses the shared lexer (as/asm_lex.c) and the section/symbol/reloc sink
 * (as/asm_obj.c).  Two passes over a rewound lexer: pass 1 sizes and places
 * labels, pass 2 emits bytes and relocations.  x86 instruction length is a
 * function of the operand values alone (all known at parse time), so there is no
 * branch-relaxation fixpoint: every branch uses the rel32 form.
 *
 * The byte recipes (REX / ModRM / SIB / SSE prefix order) mirror the JIT encoder
 * jit/emit_x86.c, which is itself checked byte-for-byte against GNU as, so the
 * assembler, the JIT, and the oracle agree by construction.
 */

#ifndef SKJ_X86_64_H
#define SKJ_X86_64_H

#include <stdint.h>
#include <stdio.h>

#include "asm_lex.h"
#include "asm_obj.h"
#include "arena.h"

/* ELF relocation types (x86-64) and machine id. */
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11
#define EM_X86_64      62

/* Sections.  The bounded input uses only these four; no DWARF (no .loc). */
enum x64_section_id {
    XSEC_TEXT,
    XSEC_RODATA,
    XSEC_DATA,
    XSEC_BSS,
    XSEC_COUNT,
};

/* A register's class and width.  The class separates the integer file from the
   xmm file; the width (in bytes) selects the e-/r-/8-/16-bit view for the REX.W
   and 0x66 decisions. */
enum x64_regclass { RC_NONE, RC_GPR, RC_XMM, RC_RIP };

/* A parsed operand.  x86 allows at most one memory operand per instruction. */
enum x64_op_kind { XO_NONE, XO_REG, XO_IMM, XO_MEM };

struct x64_operand {
    int kind;                   /* XO_* */
    int rclass;                 /* XO_REG: RC_GPR or RC_XMM */
    int reg;                    /* XO_REG: hardware number 0..15 */
    int size;                   /* operand width in bytes: 1/2/4/8/16; 0 unsized */
    /* XO_MEM: [base + index*scale + disp], or [rip + sym], or [sym] */
    int base;                   /* hardware number, or -1 if absent */
    int index;                  /* hardware number, or -1 if absent */
    int scale;                  /* 1/2/4/8 */
    int rip;                    /* 1: [rip + sym] (PC-relative) */
    long disp;                  /* literal displacement / immediate value */
    /* a symbol operand (immediate `offset sym`, memory `[sym]`, data `.quad
       sym`): sym names it, add is the +/-N addend, is_sym marks it present. */
    const char *sym;
    int32_t add;
    int is_sym;
    int mem_size;               /* XO_MEM: byte/word/dword/qword ptr width, or 0 */
};

#define X64_MAXOPS 3

/* GAS numeric local labels (1:, 1f, 1b): a redefinition counter per number, so
   a forward/backward reference resolves to a unique internal name. */
struct x64_numlabel {
    int num;
    int count;
};

struct x86_64_asm {
    struct lexer lex;
    struct section sections[XSEC_COUNT];
    int cur_section;
    struct symtab st;
    int pass;
    struct arena arena;
    struct x64_numlabel *numlabels;
    int n_numlabels;
    int numlabel_cap;
};

/* Lifecycle and the two-pass driver (as/x86_64_parse.c). */
void x86_64_init(struct x86_64_asm *a, const char *src);
void x86_64_free(struct x86_64_asm *a);
void x86_64_assemble(struct x86_64_asm *a);

/* Encode one instruction into the current section (as/x86_64_encode.c).  In
   pass 1 (emit==0) it only advances the section length; in pass 2 (emit==1) it
   writes bytes and records relocations.  Both passes compute the same length. */
void x86_64_encode(struct x86_64_asm *a, const char *mnem,
                   struct x64_operand *ops, int nops, int emit);

/* The ELF64 writer (as/x86_64_elf.c). */
void x86_64_elf_write(struct x86_64_asm *a, FILE *out);

/* Register / condition-code name lookups (as/x86_64_parse.c), shared with the
   encoder.  x64_reg returns the class (RC_*), sets *num and *size, or RC_NONE. */
int x64_reg(const char *name, int *num, int *size);
int x64_cc(const char *name);   /* condition code 0..15, or -1 */

#endif /* SKJ_X86_64_H */
