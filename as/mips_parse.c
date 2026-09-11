/* mips_parse.c : two-pass driver, operand and directive parsing for MIPS I
 *
 * The scanner and object bookkeeping are the shared skeleton (asm_lex.c,
 * asm_obj.c); this file supplies the MIPS operand grammar, directive set and
 * label handling, and drives the sizing and emit passes over mips_encode.
 * Sections carry the little-endian flag so the shared byte emitter produces
 * mipsel byte order.  Macro expansion and the .set reorder delay-slot engine
 * are later steps; here .set operands are accepted and ignored and every
 * mnemonic is a real (non-macro) instruction. */

#include "mips.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Operand parsing
 ****************************************************************/

static int
reloc_op_of(const char *name)
{
    if (strcmp(name, "hi") == 0) return MRELOC_OP_HI;
    if (strcmp(name, "lo") == 0) return MRELOC_OP_LO;
    return -1;
}

/* Parse an optional trailing "+ N" or "- N" addend. */
static long
parse_addend(struct lexer *l)
{
    long add = 0;
    if (l->tok.type == T_PLUS) {
        lex_next(l);
        if (l->tok.type == T_INT) { add = l->tok.ival; lex_next(l); }
    } else if (l->tok.type == T_MINUS) {
        lex_next(l);
        if (l->tok.type == T_INT) { add = -l->tok.ival; lex_next(l); }
    }
    return add;
}

/* Parse "(reg)" trailing a memory operand's offset; fills the base register.
 * Returns 1 if a base was parsed, 0 if there was no '(', -1 on error. */
static int
parse_mem_base(struct lexer *l, int *base)
{
    int reg;
    if (l->tok.type != T_LPAREN)
        return 0;
    lex_next(l);
    if (l->tok.type != T_IDENT)
        return -1;
    reg = mips_ireg(l->tok.str);
    if (reg < 0)
        return -1;
    lex_next(l);
    if (l->tok.type != T_RPAREN)
        return -1;
    lex_next(l);
    *base = reg;
    return 1;
}

/* Parse one operand.  Returns 1 on success, 0 on syntax error. */
static int
parse_operand(struct mips_asm *a, struct mips_operand *op)
{
    struct lexer *l = &a->lex;
    int reg, base;

    memset(op, 0, sizeof(*op));
    op->reloc_op = MRELOC_OP_NONE;

    /* relocation operator: %hi(sym [+/- add]) or %lo(sym)(reg) */
    if (l->tok.type == T_RELOC) {
        int rop = reloc_op_of(l->tok.str);
        if (rop < 0) return 0;
        lex_next(l);
        if (l->tok.type != T_LPAREN) return 0;
        lex_next(l);
        if (l->tok.type != T_IDENT && l->tok.type != T_DOT_IDENT) return 0;
        op->sym = arena_strdup(&a->arena, l->tok.str);
        lex_next(l);
        op->imm = parse_addend(l);
        if (l->tok.type != T_RPAREN) return 0;
        lex_next(l);
        op->reloc_op = rop;
        base = -1;
        {
            int r = parse_mem_base(l, &base);
            if (r < 0) return 0;
            if (r == 1) { op->kind = MO_MEM; op->reg = base; return 1; }
        }
        op->kind = MO_IMM;
        return 1;
    }

    /* (reg) : memory with zero offset */
    if (l->tok.type == T_LPAREN) {
        base = -1;
        if (parse_mem_base(l, &base) != 1) return 0;
        op->kind = MO_MEM;
        op->reg = base;
        op->imm = 0;
        return 1;
    }

    /* signed integer, possibly a memory offset  imm(reg) */
    if (l->tok.type == T_INT || l->tok.type == T_MINUS) {
        long val;
        if (l->tok.type == T_MINUS) {
            lex_next(l);
            if (l->tok.type != T_INT) return 0;
            val = -l->tok.ival;
        } else {
            val = l->tok.ival;
        }
        lex_next(l);
        base = -1;
        {
            int r = parse_mem_base(l, &base);
            if (r < 0) return 0;
            if (r == 1) {
                op->kind = MO_MEM;
                op->reg = base;
                op->imm = val;
                return 1;
            }
        }
        op->kind = MO_IMM;
        op->imm = val;
        return 1;
    }

    /* identifier: a register, or a symbol */
    if (l->tok.type == T_IDENT) {
        reg = mips_ireg(l->tok.str);
        if (reg >= 0) {
            op->kind = MO_REG;
            op->reg = reg;
            op->sym = arena_strdup(&a->arena, l->tok.str);
            lex_next(l);
            return 1;
        }
        reg = mips_freg(l->tok.str);
        if (reg >= 0) {
            op->kind = MO_FREG;
            op->reg = reg;
            op->sym = arena_strdup(&a->arena, l->tok.str);
            lex_next(l);
            return 1;
        }
        op->kind = MO_SYM;
        op->sym = arena_strdup(&a->arena, l->tok.str);
        lex_next(l);
        return 1;
    }

    /* .L-style local label used as a branch/jump target */
    if (l->tok.type == T_DOT_IDENT) {
        op->kind = MO_SYM;
        op->sym = arena_strdup(&a->arena, l->tok.str);
        lex_next(l);
        return 1;
    }

    return 0;
}

/****************************************************************
 * Directives
 ****************************************************************/

static void
skip_to_eol(struct lexer *l)
{
    while (l->tok.type != T_NEWLINE && l->tok.type != T_EOF)
        lex_next(l);
}

/* Emit an integer-list directive (.byte/.short/.word), each element `width`
 * bytes; an identifier in a .word list becomes an R_MIPS_32 relocation. */
static void
emit_int_list(struct mips_asm *a, int width)
{
    struct section *s = &a->sections[a->cur_section];
    struct lexer *l = &a->lex;

    for (;;) {
        lex_next(l);
        if (l->tok.type == T_INT) {
            if (a->pass == 2) {
                if (width == 1) sec_emit8(s, (uint8_t)l->tok.ival);
                else if (width == 2) sec_emit16(s, (uint16_t)l->tok.ival);
                else sec_emit32(s, (uint32_t)l->tok.ival);
            } else {
                s->len += width;
            }
            lex_next(l);
        } else if (l->tok.type == T_MINUS) {
            lex_next(l);
            if (l->tok.type == T_INT) {
                long v = -l->tok.ival;
                if (a->pass == 2) {
                    if (width == 1) sec_emit8(s, (uint8_t)v);
                    else if (width == 2) sec_emit16(s, (uint16_t)v);
                    else sec_emit32(s, (uint32_t)v);
                } else {
                    s->len += width;
                }
                lex_next(l);
            }
        } else if ((l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT)
                   && width == 4) {
            if (a->pass == 2) {
                int idx = sym_lookup(&a->st, l->tok.str);
                if (idx < 0) idx = sym_add(&a->st, l->tok.str);
                sec_add_reloc_t(s, (uint32_t)s->len, idx, 0, R_MIPS_32);
                sec_emit32(s, 0);
            } else {
                s->len += 4;
            }
            lex_next(l);
        } else {
            break;
        }
        if (l->tok.type != T_COMMA) break;
    }
}

static void
emit_string(struct mips_asm *a, int nul)
{
    struct section *s = &a->sections[a->cur_section];
    struct lexer *l = &a->lex;

    lex_next(l);
    if (l->tok.type == T_STRING) {
        if (a->pass == 2) {
            int k;
            for (k = 0; k < l->tok.str_len; k++)
                sec_emit8(s, (uint8_t)l->tok.str[k]);
            if (nul) sec_emit8(s, 0);
        } else {
            s->len += l->tok.str_len + (nul ? 1 : 0);
        }
        lex_next(l);
    }
}

static void
do_align(struct mips_asm *a, int bytes)
{
    struct section *s = &a->sections[a->cur_section];
    if (bytes > s->align)
        s->align = bytes;
    if (a->pass == 2)
        sec_align(s, bytes);
    else
        while (s->len % bytes) s->len++;
}

static void
handle_directive(struct mips_asm *a, const char *dir)
{
    struct lexer *l = &a->lex;
    struct section *s = &a->sections[a->cur_section];

    if (strcmp(dir, ".text") == 0)   { a->cur_section = MSEC_TEXT; return; }
    if (strcmp(dir, ".data") == 0)   { a->cur_section = MSEC_DATA; return; }
    if (strcmp(dir, ".rodata") == 0) { a->cur_section = MSEC_RODATA; return; }
    if (strcmp(dir, ".bss") == 0 || strcmp(dir, ".sbss") == 0) {
        a->cur_section = MSEC_BSS; return;
    }

    if (strcmp(dir, ".section") == 0) {
        lex_next(l);
        if (l->tok.type == T_DOT_IDENT || l->tok.type == T_IDENT) {
            const char *n = l->tok.str;
            if (strncmp(n, ".text", 5) == 0)        a->cur_section = MSEC_TEXT;
            else if (strncmp(n, ".rodata", 7) == 0) a->cur_section = MSEC_RODATA;
            else if (strncmp(n, ".data", 5) == 0)   a->cur_section = MSEC_DATA;
            else if (strncmp(n, ".bss", 4) == 0)    a->cur_section = MSEC_BSS;
            else a->cur_section = MSEC_DATA;
        }
        skip_to_eol(l);
        return;
    }

    if (strcmp(dir, ".globl") == 0 || strcmp(dir, ".global") == 0) {
        lex_next(l);
        if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
            int idx = sym_lookup(&a->st, l->tok.str);
            if (idx < 0) idx = sym_add(&a->st, l->tok.str);
            sym_set_global(&a->st, idx);
            lex_next(l);
        }
        return;
    }

    /* .align N aligns to 2^N (GAS MIPS). */
    if (strcmp(dir, ".align") == 0 || strcmp(dir, ".p2align") == 0) {
        lex_next(l);
        if (l->tok.type == T_INT) {
            int n = (int)l->tok.ival;
            int bytes = 1 << (n < 0 ? 0 : n > 12 ? 12 : n);
            lex_next(l);
            do_align(a, bytes);
        }
        skip_to_eol(l);
        return;
    }

    if (strcmp(dir, ".word") == 0 || strcmp(dir, ".long") == 0) {
        emit_int_list(a, 4); return;
    }
    if (strcmp(dir, ".short") == 0 || strcmp(dir, ".half") == 0
        || strcmp(dir, ".2byte") == 0) {
        emit_int_list(a, 2); return;
    }
    if (strcmp(dir, ".byte") == 0) { emit_int_list(a, 1); return; }

    if (strcmp(dir, ".ascii") == 0)  { emit_string(a, 0); return; }
    if (strcmp(dir, ".asciiz") == 0 || strcmp(dir, ".asciz") == 0
        || strcmp(dir, ".string") == 0) {
        emit_string(a, 1); return;
    }

    if (strcmp(dir, ".space") == 0 || strcmp(dir, ".zero") == 0
        || strcmp(dir, ".skip") == 0) {
        lex_next(l);
        if (l->tok.type == T_INT) {
            if (a->pass == 2) sec_space(s, (int)l->tok.ival);
            else s->len += (int)l->tok.ival;
            lex_next(l);
        }
        skip_to_eol(l);
        return;
    }

    /* Accepted and ignored: assembler-mode and metadata directives.  The .set
     * reorder engine is a later step, so .set operands are consumed here. */
    if (strcmp(dir, ".set") == 0 || strcmp(dir, ".type") == 0
        || strcmp(dir, ".size") == 0 || strcmp(dir, ".ent") == 0
        || strcmp(dir, ".end") == 0 || strcmp(dir, ".frame") == 0
        || strcmp(dir, ".mask") == 0 || strcmp(dir, ".fmask") == 0
        || strcmp(dir, ".option") == 0 || strcmp(dir, ".file") == 0
        || strcmp(dir, ".ident") == 0 || strcmp(dir, ".local") == 0
        || strcmp(dir, ".gpword") == 0 || strcmp(dir, ".cprestore") == 0
        || strncmp(dir, ".cfi_", 5) == 0) {
        skip_to_eol(l);
        return;
    }

    warn("line %d: unknown directive '%s'", l->tok.line, dir);
    skip_to_eol(l);
}

/****************************************************************
 * Instruction lines
 ****************************************************************/

#define MAX_OPS 4

static void
parse_instruction(struct mips_asm *a, const char *mnemonic)
{
    struct lexer *l = &a->lex;
    struct mips_operand ops[MAX_OPS];
    int nops = 0;
    int result;

    memset(ops, 0, sizeof(ops));

    if (l->tok.type != T_NEWLINE && l->tok.type != T_EOF) {
        for (;;) {
            if (nops >= MAX_OPS)
                die("line %d: too many operands for '%s'",
                    l->tok.line, mnemonic);
            if (!parse_operand(a, &ops[nops]))
                die("line %d: bad operand for '%s'", l->tok.line, mnemonic);
            nops++;
            if (l->tok.type != T_COMMA) break;
            lex_next(l);
        }
    }

    result = mips_encode(a, mnemonic, ops, nops, a->pass == 2);
    if (result < 0)
        die("line %d: cannot encode '%s'", l->tok.line, mnemonic);
    if (a->pass == 1)
        a->sections[a->cur_section].len += result;
    skip_to_eol(l);
}

/****************************************************************
 * Pass driver
 ****************************************************************/

static void
define_label(struct mips_asm *a, const char *name)
{
    if (a->pass == 1) {
        int idx = sym_lookup(&a->st, name);
        if (idx < 0) idx = sym_add(&a->st, name);
        sym_define(&a->st, idx, a->cur_section,
                   (uint32_t)a->sections[a->cur_section].len);
    }
}

static void
run_pass(struct mips_asm *a)
{
    struct lexer *l = &a->lex;

    a->cur_section = MSEC_TEXT;
    lex_next(l);

    while (l->tok.type != T_EOF) {
        if (l->tok.type == T_NEWLINE) {
            lex_next(l);
            continue;
        }

        /* dotted token: a directive, or a dotted label (.L1:).  Peek one token
         * for a ':' to tell them apart, restoring the lexer to the name if it
         * is a directive. */
        if (l->tok.type == T_DOT_IDENT) {
            const char *name = l->tok.str;
            struct lexer saved = *l;
            lex_next(l);
            if (l->tok.type == T_COLON) {
                lex_next(l);
                define_label(a, name);
                continue;
            }
            *l = saved;
            handle_directive(a, name);
            skip_to_eol(l);
            if (l->tok.type == T_NEWLINE) lex_next(l);
            continue;
        }

        if (l->tok.type == T_IDENT) {
            const char *name = l->tok.str;
            lex_next(l);
            if (l->tok.type == T_COLON) {
                lex_next(l);
                define_label(a, name);
                continue;
            }
            parse_instruction(a, name);
            if (l->tok.type == T_NEWLINE) lex_next(l);
            continue;
        }

        skip_to_eol(l);
        if (l->tok.type == T_NEWLINE) lex_next(l);
    }
}

static void
reset_lexer(struct mips_asm *a)
{
    a->lex.pos = a->lex.src;
    a->lex.line = 1;
    a->lex.tok.type = T_NEWLINE;
}

/****************************************************************
 * Setup and teardown
 ****************************************************************/

void
mips_init(struct mips_asm *a, const char *src)
{
    int k;

    memset(a, 0, sizeof(*a));
    arena_init(&a->arena);
    a->lex.arena = &a->arena;
    a->st.arena = &a->arena;
    a->cur_section = MSEC_TEXT;
    for (k = 0; k < MSEC_COUNT; k++)
        a->sections[k].little_endian = 1;
    /* fill the delay slots in the .set reorder regions before lexing */
    src = mips_schedule(&a->arena, src);
    a->sched_src = src;
    lex_init(&a->lex, src, '#', 1);
    a->lex.dollar_reg = 1;
}

void
mips_free(struct mips_asm *a)
{
    int k;

    for (k = 0; k < MSEC_COUNT; k++) {
        free(a->sections[k].data);
        free(a->sections[k].relocs);
    }
    free(a->st.syms);
    free(a->lex.str_buf);
    arena_free(&a->arena);
}

/****************************************************************
 * Two-pass assemble: sizing (label addresses), then emit (real bytes).
 ****************************************************************/

void
mips_assemble(struct mips_asm *a)
{
    int k;

    /* pass 1: sizing, to place labels */
    for (k = 0; k < MSEC_COUNT; k++)
        a->sections[k].len = 0;
    a->pass = 1;
    reset_lexer(a);
    run_pass(a);

    /* pass 2: emit real bytes and relocations */
    for (k = 0; k < MSEC_COUNT; k++)
        a->sections[k].len = 0;
    a->pass = 2;
    reset_lexer(a);
    run_pass(a);
}
