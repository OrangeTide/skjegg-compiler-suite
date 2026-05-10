/* as.h : ColdFire/m68k assembler — shared types */

#ifndef AS_H
#define AS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arena.h"
#include "util.h"

/****************************************************************
 * Tokens
 ****************************************************************/

enum token_type {
    T_IDENT,
    T_DOT_IDENT,
    T_INT,
    T_STRING,
    T_HASH,
    T_COMMA,
    T_COLON,
    T_LPAREN,
    T_RPAREN,
    T_MINUS,
    T_PLUS,
    T_NEWLINE,
    T_EOF,
};

struct token {
    int type;
    int line;
    const char *str;
    int str_len;
    long ival;
};

/****************************************************************
 * Lexer
 ****************************************************************/

struct lexer {
    const char *src;
    const char *pos;
    int line;
    struct token tok;
    char *str_buf;
    int str_cap;
    struct arena *arena;
};

void lex_init(struct lexer *l, const char *src);
void lex_next(struct lexer *l);

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
    SEC_BSS,
    SEC_COUNT,
};

struct reloc {
    uint32_t offset;
    int sym_idx;
    int32_t addend;
};

struct section {
    uint8_t *data;
    int len;
    int cap;
    struct reloc *relocs;
    int nrelocs;
    int reloc_cap;
};

/****************************************************************
 * Symbols
 ****************************************************************/

struct symbol {
    const char *name;
    int section;
    uint32_t value;
    int global;
    int defined;
};

/****************************************************************
 * Assembler state
 ****************************************************************/

struct assembler {
    struct lexer lex;
    struct section sections[SEC_COUNT];
    int cur_section;
    struct symbol *syms;
    int nsyms;
    int sym_cap;
    int pass;
    int errors;
    struct arena arena;
};

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
                struct operand *op1, struct operand *op2);

/****************************************************************
 * ELF writer
 ****************************************************************/

void elf_write(struct assembler *a, FILE *out);

/****************************************************************
 * Symbol table helpers
 ****************************************************************/

int sym_lookup(struct assembler *a, const char *name);
int sym_add(struct assembler *a, const char *name);
void sym_define(struct assembler *a, int idx, int section, uint32_t value);
void sym_set_global(struct assembler *a, int idx);

/****************************************************************
 * Section helpers
 ****************************************************************/

void sec_emit8(struct section *s, uint8_t val);
void sec_emit16(struct section *s, uint16_t val);
void sec_emit32(struct section *s, uint32_t val);
void sec_align(struct section *s, int alignment);
void sec_space(struct section *s, int nbytes);
void sec_add_reloc(struct section *s, uint32_t offset, int sym_idx,
                   int32_t addend);

#endif
