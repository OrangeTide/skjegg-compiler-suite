/* as.h : ColdFire/m68k assembler — shared types */

#ifndef AS_H
#define AS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arena.h"
#include "util.h"
#include "asm_lex.h"
#include "asm_obj.h"

/****************************************************************
 * Operands
 ****************************************************************/

enum operand_type {
    OP_NONE,
    OP_DREG,
    OP_AREG,
    OP_FPREG,
    OP_IMM,
    OP_ABS,
    OP_IND,
    OP_DISP,
    OP_PREDEC,
    OP_POSTINC,
    OP_REGLIST,
};

struct operand {
    int type;
    int reg;
    long imm;
    const char *sym;
    uint16_t regmask;
};

/****************************************************************
 * Sections
 ****************************************************************/

enum section_id {
    SEC_TEXT,
    SEC_DATA,
    SEC_RODATA,
    SEC_BSS,
    SEC_DEBUG_LINE,         /* DWARF (-g): non-alloc PROGBITS, built at finalize */
    SEC_DEBUG_INFO,
    SEC_DEBUG_ABBREV,
    SEC_COUNT,
};

/****************************************************************
 * Assembler state
 ****************************************************************/

struct assembler {
    struct lexer lex;
    struct section sections[SEC_COUNT];
    int cur_section;
    struct symtab st;
    int pass;
    int errors;
    int short_branches;         /* force word-form branches; error if a
                                   displacement overflows 16 bits (default:
                                   use the 32-bit form when it does not fit) */
    const char *debug_file;     /* DWARF source name from .file (-g), or NULL */
    struct locrow {
        uint32_t offset;        /* .text byte offset of a .loc */
        int line;
    } *locrows;
    int n_locrows;
    int locrow_cap;
    struct arena arena;
};

/* Build the DWARF .debug_* sections from the .loc rows (after pass 2). */
void cf_emit_debug(struct assembler *a);

void as_init(struct assembler *a, const char *src);
void as_free(struct assembler *a);
void as_pass1(struct assembler *a);
void as_pass2(struct assembler *a);

/****************************************************************
 * Encoder
 ****************************************************************/

int encode_insn(struct assembler *a, const char *mnemonic, int size,
                struct operand *op1, struct operand *op2);

int encode_size(const char *mnemonic, int size,
                struct operand *op1, struct operand *op2, int short_branches);

/****************************************************************
 * ELF writer
 ****************************************************************/

void elf_write(struct assembler *a, FILE *out);

#endif
