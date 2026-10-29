/* x86_64_parse.c : register/cc tables, the intel-syntax operand and directive
 * grammar, and the two-pass driver for skj-as-x86-64.
 *
 * The scanner and object bookkeeping are the shared skeleton (asm_lex.c,
 * asm_obj.c); this file supplies the GNU `.intel_syntax noprefix` grammar the
 * toolkit's x86-64 backends emit.  Unlike the RISC-V assembler there is no
 * branch-relaxation fixpoint: every branch is a rel32, so an instruction's
 * length is fixed by its operands.  The encoder emits into the current section
 * on both passes (the sections are rewound between them), so pass 1 lays out
 * labels and pass 2 writes the final bytes and relocations.
 */

#include "x86_64.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Register and condition-code tables
 ****************************************************************/

struct reg_ent {
    const char *name;
    int rclass;
    int num;
    int size;
};

static const struct reg_ent regs[] = {
    /* 64-bit */
    { "rax", RC_GPR, 0, 8 }, { "rcx", RC_GPR, 1, 8 },
    { "rdx", RC_GPR, 2, 8 }, { "rbx", RC_GPR, 3, 8 },
    { "rsp", RC_GPR, 4, 8 }, { "rbp", RC_GPR, 5, 8 },
    { "rsi", RC_GPR, 6, 8 }, { "rdi", RC_GPR, 7, 8 },
    { "r8",  RC_GPR, 8, 8 }, { "r9",  RC_GPR, 9, 8 },
    { "r10", RC_GPR, 10, 8 }, { "r11", RC_GPR, 11, 8 },
    { "r12", RC_GPR, 12, 8 }, { "r13", RC_GPR, 13, 8 },
    { "r14", RC_GPR, 14, 8 }, { "r15", RC_GPR, 15, 8 },
    /* 32-bit */
    { "eax", RC_GPR, 0, 4 }, { "ecx", RC_GPR, 1, 4 },
    { "edx", RC_GPR, 2, 4 }, { "ebx", RC_GPR, 3, 4 },
    { "esp", RC_GPR, 4, 4 }, { "ebp", RC_GPR, 5, 4 },
    { "esi", RC_GPR, 6, 4 }, { "edi", RC_GPR, 7, 4 },
    { "r8d", RC_GPR, 8, 4 }, { "r9d", RC_GPR, 9, 4 },
    { "r10d", RC_GPR, 10, 4 }, { "r11d", RC_GPR, 11, 4 },
    { "r12d", RC_GPR, 12, 4 }, { "r13d", RC_GPR, 13, 4 },
    { "r14d", RC_GPR, 14, 4 }, { "r15d", RC_GPR, 15, 4 },
    /* 16-bit */
    { "ax", RC_GPR, 0, 2 }, { "cx", RC_GPR, 1, 2 },
    { "dx", RC_GPR, 2, 2 }, { "bx", RC_GPR, 3, 2 },
    { "sp", RC_GPR, 4, 2 }, { "bp", RC_GPR, 5, 2 },
    { "si", RC_GPR, 6, 2 }, { "di", RC_GPR, 7, 2 },
    { "r8w", RC_GPR, 8, 2 }, { "r9w", RC_GPR, 9, 2 },
    { "r10w", RC_GPR, 10, 2 }, { "r11w", RC_GPR, 11, 2 },
    { "r12w", RC_GPR, 12, 2 }, { "r13w", RC_GPR, 13, 2 },
    { "r14w", RC_GPR, 14, 2 }, { "r15w", RC_GPR, 15, 2 },
    /* 8-bit low byte (spl/bpl/sil/dil force a REX prefix in the encoder) */
    { "al", RC_GPR, 0, 1 }, { "cl", RC_GPR, 1, 1 },
    { "dl", RC_GPR, 2, 1 }, { "bl", RC_GPR, 3, 1 },
    { "spl", RC_GPR, 4, 1 }, { "bpl", RC_GPR, 5, 1 },
    { "sil", RC_GPR, 6, 1 }, { "dil", RC_GPR, 7, 1 },
    { "r8b", RC_GPR, 8, 1 }, { "r9b", RC_GPR, 9, 1 },
    { "r10b", RC_GPR, 10, 1 }, { "r11b", RC_GPR, 11, 1 },
    { "r12b", RC_GPR, 12, 1 }, { "r13b", RC_GPR, 13, 1 },
    { "r14b", RC_GPR, 14, 1 }, { "r15b", RC_GPR, 15, 1 },
    /* xmm */
    { "xmm0", RC_XMM, 0, 16 }, { "xmm1", RC_XMM, 1, 16 },
    { "xmm2", RC_XMM, 2, 16 }, { "xmm3", RC_XMM, 3, 16 },
    { "xmm4", RC_XMM, 4, 16 }, { "xmm5", RC_XMM, 5, 16 },
    { "xmm6", RC_XMM, 6, 16 }, { "xmm7", RC_XMM, 7, 16 },
    { "xmm8", RC_XMM, 8, 16 }, { "xmm9", RC_XMM, 9, 16 },
    { "xmm10", RC_XMM, 10, 16 }, { "xmm11", RC_XMM, 11, 16 },
    { "xmm12", RC_XMM, 12, 16 }, { "xmm13", RC_XMM, 13, 16 },
    { "xmm14", RC_XMM, 14, 16 }, { "xmm15", RC_XMM, 15, 16 },
    { NULL, RC_NONE, 0, 0 },
};

int
x64_reg(const char *name, int *num, int *size)
{
    int i;
    for (i = 0; regs[i].name; i++) {
        if (strcmp(regs[i].name, name) == 0) {
            if (num)  *num = regs[i].num;
            if (size) *size = regs[i].size;
            return regs[i].rclass;
        }
    }
    return RC_NONE;
}

int
x64_cc(const char *name)
{
    static const struct { const char *s; int cc; } tab[] = {
        { "o", 0 }, { "no", 1 },
        { "b", 2 }, { "c", 2 }, { "nae", 2 },
        { "ae", 3 }, { "nb", 3 }, { "nc", 3 },
        { "e", 4 }, { "z", 4 },
        { "ne", 5 }, { "nz", 5 },
        { "be", 6 }, { "na", 6 },
        { "a", 7 }, { "nbe", 7 },
        { "s", 8 }, { "ns", 9 },
        { "p", 10 }, { "pe", 10 }, { "np", 11 }, { "po", 11 },
        { "l", 12 }, { "nge", 12 }, { "ge", 13 }, { "nl", 13 },
        { "le", 14 }, { "ng", 14 }, { "g", 15 }, { "nle", 15 },
        { NULL, 0 },
    };
    int i;
    for (i = 0; tab[i].s; i++)
        if (strcmp(tab[i].s, name) == 0)
            return tab[i].cc;
    return -1;
}

/****************************************************************
 * Numeric local labels (1: / 1f / 1b), as in rv_parse.c
 ****************************************************************/

static int
numeric_ref(const char *str, int *num, char *dir)
{
    size_t n = strlen(str);
    size_t i;
    if (n < 2)
        return 0;
    if (str[n - 1] != 'f' && str[n - 1] != 'b')
        return 0;
    for (i = 0; i < n - 1; i++)
        if (str[i] < '0' || str[i] > '9')
            return 0;
    *num = atoi(str);
    *dir = str[n - 1];
    return 1;
}

static int *
numlabel_count(struct x86_64_asm *a, int num)
{
    int i;
    for (i = 0; i < a->n_numlabels; i++)
        if (a->numlabels[i].num == num)
            return &a->numlabels[i].count;

    if (a->n_numlabels >= a->numlabel_cap) {
        a->numlabel_cap = a->numlabel_cap ? a->numlabel_cap * 2 : 16;
        a->numlabels = realloc(a->numlabels,
            (size_t)a->numlabel_cap * sizeof(*a->numlabels));
        if (!a->numlabels)
            die("out of memory");
    }
    a->numlabels[a->n_numlabels].num = num;
    a->numlabels[a->n_numlabels].count = 0;
    return &a->numlabels[a->n_numlabels++].count;
}

static const char *
numeric_ref_name(struct x86_64_asm *a, int num, char dir)
{
    char buf[32];
    int k = *numlabel_count(a, num);
    if (dir == 'b')
        k -= 1;
    snprintf(buf, sizeof(buf), ".Lnum_%d_%d", num, k < 0 ? 0 : k);
    return arena_strdup(&a->arena, buf);
}

/****************************************************************
 * Operand parsing
 ****************************************************************/

static int
size_word(const char *name)
{
    if (strcmp(name, "byte") == 0)  return 1;
    if (strcmp(name, "word") == 0)  return 2;
    if (strcmp(name, "dword") == 0) return 4;
    if (strcmp(name, "qword") == 0) return 8;
    if (strcmp(name, "xmmword") == 0) return 16;
    return 0;
}

static long parse_addend(struct lexer *l);

/* Parse a bracketed memory operand `[ ... ]`, already positioned on `[`.
   Forms: [base], [base+disp], [base+index*scale+disp], [rip+sym(+N)], [sym(+N)]. */
static void
parse_mem(struct x86_64_asm *a, struct x64_operand *op)
{
    struct lexer *l = &a->lex;
    int sign = 1;

    op->kind = XO_MEM;
    op->base = -1;
    op->index = -1;
    op->scale = 1;

    lex_next(l);                            /* consume '[' */
    while (l->tok.type != T_RBRACKET && l->tok.type != T_NEWLINE
           && l->tok.type != T_EOF) {
        if (l->tok.type == T_PLUS)  { sign = 1;  lex_next(l); continue; }
        if (l->tok.type == T_MINUS) { sign = -1; lex_next(l); continue; }

        if (l->tok.type == T_INT) {
            long v = sign * l->tok.ival;
            if (op->is_sym || op->rip)
                op->add += (int32_t)v;
            else
                op->disp += v;
            sign = 1;
            lex_next(l);
            continue;
        }

        if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
            const char *name = l->tok.str;
            int num, sz;
            if (strcmp(name, "rip") == 0) {
                op->rip = 1;
                lex_next(l);
                continue;
            }
            if (x64_reg(name, &num, &sz) == RC_GPR) {
                lex_next(l);
                if (l->tok.type == T_STAR) {    /* index*scale */
                    lex_next(l);
                    if (l->tok.type != T_INT)
                        die("line %d: expected scale after '*'", l->tok.line);
                    op->index = num;
                    op->scale = (int)l->tok.ival;
                    lex_next(l);
                } else if (op->base < 0) {
                    op->base = num;
                } else {
                    op->index = num;            /* second bare register */
                }
                continue;
            }
            /* a symbol inside the brackets: [rip+sym] or absolute [sym] */
            op->sym = arena_strdup(&a->arena, name);
            op->is_sym = 1;
            lex_next(l);
            continue;
        }
        die("line %d: bad memory operand", l->tok.line);
    }
    if (l->tok.type != T_RBRACKET)
        die("line %d: unterminated memory operand", l->tok.line);
    lex_next(l);                            /* consume ']' */
}

/****************************************************************
 * Assembler expressions
 *
 * `+ - *` and parentheses over integer literals and symbols.  An expression
 * classifies by its structure, which is identical in both passes, so an
 * immediate's encoded width never changes between them:
 *   EXPR_CONST  no symbols: a pure integer, known in both passes.
 *   EXPR_RELOC  one symbol with coefficient +1 plus a constant addend: a
 *               relocation, the symbol resolved at link (a branch target,
 *               `sym`, `sym + N`).
 *   EXPR_ABS    any other symbol arithmetic (a label difference, a negated or
 *               scaled symbol): it must fold to a constant, which it does in
 *               pass 2 once every label is placed.
 ****************************************************************/

enum expr_class { EXPR_CONST, EXPR_RELOC, EXPR_ABS };

#define EXPR_MAXSYM 8
struct expr_acc {
    long k;                             /* the constant part */
    const char *sym[EXPR_MAXSYM];
    long coef[EXPR_MAXSYM];             /* each symbol's integer coefficient */
    int nsym;
};

struct expr_val {
    enum expr_class cls;
    long value;                         /* EXPR_CONST / EXPR_ABS */
    const char *sym;                    /* EXPR_RELOC */
    int32_t add;                        /* EXPR_RELOC addend */
};

static void parse_expr_sum(struct x86_64_asm *a, struct expr_acc *out);

static void
acc_add_sym(struct x86_64_asm *a, struct expr_acc *acc, const char *name, long coef)
{
    if (acc->nsym >= EXPR_MAXSYM)
        die("line %d: expression too complex", a->lex.tok.line);
    acc->sym[acc->nsym] = name;
    acc->coef[acc->nsym] = coef;
    acc->nsym++;
}

static void
acc_negate(struct expr_acc *x)
{
    int i;
    x->k = -x->k;
    for (i = 0; i < x->nsym; i++)
        x->coef[i] = -x->coef[i];
}

static void
parse_expr_primary(struct x86_64_asm *a, struct expr_acc *out)
{
    struct lexer *l = &a->lex;

    memset(out, 0, sizeof(*out));
    if (l->tok.type == T_LPAREN) {
        lex_next(l);
        parse_expr_sum(a, out);
        if (l->tok.type != T_RPAREN)
            die("line %d: missing ')' in expression", l->tok.line);
        lex_next(l);
        return;
    }
    if (l->tok.type == T_INT) {
        out->k = l->tok.ival;
        lex_next(l);
        return;
    }
    if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
        int num;
        char dir;
        const char *name;
        if (numeric_ref(l->tok.str, &num, &dir))
            name = numeric_ref_name(a, num, dir);
        else
            name = arena_strdup(&a->arena, l->tok.str);
        acc_add_sym(a, out, name, +1);
        lex_next(l);
        return;
    }
    die("line %d: bad expression", l->tok.line);
}

static void
parse_expr_unary(struct x86_64_asm *a, struct expr_acc *out)
{
    struct lexer *l = &a->lex;
    int neg = 0;

    while (l->tok.type == T_PLUS || l->tok.type == T_MINUS) {
        if (l->tok.type == T_MINUS)
            neg = !neg;
        lex_next(l);
    }
    parse_expr_primary(a, out);
    if (neg)
        acc_negate(out);
}

static void
parse_expr_product(struct x86_64_asm *a, struct expr_acc *out)
{
    struct lexer *l = &a->lex;

    parse_expr_unary(a, out);
    while (l->tok.type == T_STAR) {
        struct expr_acc rhs;
        int i;
        lex_next(l);
        parse_expr_unary(a, &rhs);
        if (out->nsym == 0) {           /* constant * (anything): scale rhs */
            long c = out->k;
            *out = rhs;
            out->k *= c;
            for (i = 0; i < out->nsym; i++)
                out->coef[i] *= c;
        } else if (rhs.nsym == 0) {     /* (symbol expr) * constant: scale out */
            out->k *= rhs.k;
            for (i = 0; i < out->nsym; i++)
                out->coef[i] *= rhs.k;
        } else {
            die("line %d: cannot multiply two symbols", l->tok.line);
        }
    }
}

static void
parse_expr_sum(struct x86_64_asm *a, struct expr_acc *out)
{
    struct lexer *l = &a->lex;

    parse_expr_product(a, out);
    for (;;) {
        struct expr_acc rhs;
        int sub, i;

        if (l->tok.type == T_PLUS)
            sub = 0;
        else if (l->tok.type == T_MINUS)
            sub = 1;
        else
            break;
        lex_next(l);
        parse_expr_product(a, &rhs);
        if (sub)
            acc_negate(&rhs);
        out->k += rhs.k;
        for (i = 0; i < rhs.nsym; i++)
            acc_add_sym(a, out, rhs.sym[i], rhs.coef[i]);
    }
}

/* Parse and classify an expression.  EXPR_ABS folds its symbols by value (every
   label is placed by pass 2; an undefined one is an error there). */
static void
eval_expr(struct x86_64_asm *a, struct expr_val *ev)
{
    struct expr_acc acc;
    long v;
    int i;

    parse_expr_sum(a, &acc);

    if (acc.nsym == 0) {
        ev->cls = EXPR_CONST;
        ev->value = acc.k;
        return;
    }
    if (acc.nsym == 1 && acc.coef[0] == 1) {
        ev->cls = EXPR_RELOC;
        ev->sym = acc.sym[0];
        ev->add = (int32_t)acc.k;
        return;
    }
    v = acc.k;
    for (i = 0; i < acc.nsym; i++) {
        int idx = sym_lookup(&a->st, acc.sym[i]);
        if (idx >= 0 && a->st.syms[idx].defined)
            v += acc.coef[i] * (long)a->st.syms[idx].value;
        else if (a->pass == 2)
            die("line %d: undefined symbol '%s' in expression",
                a->lex.tok.line, acc.sym[i]);
    }
    ev->cls = EXPR_ABS;
    ev->value = v;
}

/* Parse one operand.  Memory size qualifiers (`qword ptr [...]`) set mem_size. */
static void
parse_operand(struct x86_64_asm *a, struct x64_operand *op)
{
    struct lexer *l = &a->lex;
    int mem_size = 0;

    memset(op, 0, sizeof(*op));
    op->base = -1;
    op->index = -1;
    op->scale = 1;

    /* optional size qualifier: byte|word|dword|qword [ptr] [rip]? */
    if (l->tok.type == T_IDENT) {
        int sz = size_word(l->tok.str);
        if (sz) {
            struct lexer saved = *l;
            lex_next(l);
            if (l->tok.type == T_IDENT && strcmp(l->tok.str, "ptr") == 0) {
                mem_size = sz;
                lex_next(l);
            } else if (l->tok.type == T_LBRACKET) {
                mem_size = sz;              /* `qword [mem]` without `ptr` */
            } else {
                lex_restore(l, &saved);     /* not a qualifier; a plain symbol */
            }
        }
    }

    if (l->tok.type == T_LBRACKET) {
        parse_mem(a, op);
        op->mem_size = mem_size;
        op->size = mem_size;
        return;
    }

    /* `offset sym` : an address immediate (R_X86_64_32 / _32S). */
    if (l->tok.type == T_IDENT && strcmp(l->tok.str, "offset") == 0) {
        lex_next(l);
        if (l->tok.type != T_IDENT && l->tok.type != T_DOT_IDENT)
            die("line %d: expected symbol after 'offset'", l->tok.line);
        op->kind = XO_IMM;
        op->is_sym = 1;
        op->sym = arena_strdup(&a->arena, l->tok.str);
        lex_next(l);
        op->add += (int32_t)parse_addend(l);
        return;
    }

    if (l->tok.type == T_IDENT) {
        int num, sz;
        int cls = x64_reg(l->tok.str, &num, &sz);
        if (cls != RC_NONE) {
            op->kind = XO_REG;
            op->rclass = cls;
            op->reg = num;
            op->size = sz;
            lex_next(l);
            return;
        }
    }

    /* An integer or symbol expression: a literal, a branch target (call/jmp/
       jCC), `offset`-free symbol references, and folded arithmetic such as a
       label difference `end - start`. */
    if (l->tok.type == T_INT || l->tok.type == T_MINUS || l->tok.type == T_PLUS
        || l->tok.type == T_LPAREN || l->tok.type == T_IDENT
        || l->tok.type == T_DOT_IDENT) {
        struct expr_val ev;

        eval_expr(a, &ev);
        op->kind = XO_IMM;
        if (ev.cls == EXPR_RELOC) {
            op->is_sym = 1;
            op->sym = ev.sym;
            op->add = ev.add;
        } else {
            op->disp = ev.value;
            if (ev.cls == EXPR_ABS)
                op->wide_imm = 1;
        }
        return;
    }

    die("line %d: bad operand", l->tok.line);
}

/* A trailing "+ N" / "- N" addend. */
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

/****************************************************************
 * Directives
 ****************************************************************/

static void
skip_to_eol(struct lexer *l)
{
    while (l->tok.type != T_NEWLINE && l->tok.type != T_EOF)
        lex_next(l);
}

/* An integer-list directive (.byte/.word/.long/.quad): each element `width`
   bytes; a symbol element becomes an absolute relocation (R_X86_64_32 for a
   4-byte element, R_X86_64_64 for 8). */
static void
emit_int_list(struct x86_64_asm *a, int width)
{
    struct section *s = &a->sections[a->cur_section];
    struct lexer *l = &a->lex;

    for (;;) {
        struct expr_val ev;
        uint32_t off;

        lex_next(l);                    /* the directive keyword, then each comma */
        if (l->tok.type == T_NEWLINE || l->tok.type == T_EOF)
            break;
        off = (uint32_t)s->len;
        eval_expr(a, &ev);
        if (ev.cls == EXPR_RELOC) {
            int idx = -1;
            if (width != 4 && width != 8)
                die("line %d: a symbol needs a 4- or 8-byte data element",
                    l->tok.line);
            if (a->pass == 2) {
                idx = sym_lookup(&a->st, ev.sym);
                if (idx < 0) idx = sym_add(&a->st, ev.sym);
                sec_add_reloc_t(s, off, idx, ev.add,
                                width == 8 ? R_X86_64_64 : R_X86_64_32);
            }
            if (width == 8) { sec_emit32(s, 0); sec_emit32(s, 0); }
            else sec_emit32(s, 0);
        } else {
            long v = ev.value;          /* EXPR_CONST, or EXPR_ABS folded */
            if (width == 1) sec_emit8(s, (uint8_t)v);
            else if (width == 2) sec_emit16(s, (uint16_t)v);
            else if (width == 4) sec_emit32(s, (uint32_t)v);
            else { sec_emit32(s, (uint32_t)v);
                   sec_emit32(s, (uint32_t)((uint64_t)v >> 32)); }
        }
        if (l->tok.type != T_COMMA) break;
    }
}

static void
emit_string(struct x86_64_asm *a, int nul)
{
    struct section *s = &a->sections[a->cur_section];
    struct lexer *l = &a->lex;

    lex_next(l);
    if (l->tok.type == T_STRING) {
        int k;
        for (k = 0; k < l->tok.str_len; k++)
            sec_emit8(s, (uint8_t)l->tok.str[k]);
        if (nul) sec_emit8(s, 0);
        lex_next(l);
    }
}

static void
handle_directive(struct x86_64_asm *a, const char *dir)
{
    struct lexer *l = &a->lex;
    struct section *s = &a->sections[a->cur_section];

    if (strcmp(dir, ".text") == 0)   { a->cur_section = XSEC_TEXT; return; }
    if (strcmp(dir, ".data") == 0)   { a->cur_section = XSEC_DATA; return; }
    if (strcmp(dir, ".rodata") == 0) { a->cur_section = XSEC_RODATA; return; }
    if (strcmp(dir, ".bss") == 0)    { a->cur_section = XSEC_BSS; return; }

    if (strcmp(dir, ".intel_syntax") == 0) { skip_to_eol(l); return; }

    if (strcmp(dir, ".section") == 0) {
        lex_next(l);
        if (l->tok.type == T_DOT_IDENT || l->tok.type == T_IDENT) {
            const char *n = l->tok.str;
            if (strncmp(n, ".text", 5) == 0)        a->cur_section = XSEC_TEXT;
            else if (strncmp(n, ".rodata", 7) == 0) a->cur_section = XSEC_RODATA;
            else if (strncmp(n, ".data", 5) == 0)   a->cur_section = XSEC_DATA;
            else if (strncmp(n, ".bss", 4) == 0)    a->cur_section = XSEC_BSS;
            /* .note.GNU-stack and the like carry no content: leave the section
               unchanged (nothing is emitted after them). */
        }
        skip_to_eol(l);
        return;
    }

    if (strcmp(dir, ".globl") == 0 || strcmp(dir, ".global") == 0 ||
        strcmp(dir, ".weak") == 0) {
        int is_weak = (dir[1] == 'w');
        lex_next(l);
        if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
            int idx = sym_lookup(&a->st, l->tok.str);
            if (idx < 0) idx = sym_add(&a->st, l->tok.str);
            if (is_weak) sym_set_weak(&a->st, idx);
            else sym_set_global(&a->st, idx);
            lex_next(l);
        }
        return;
    }

    /* x86 ELF: .align / .balign take a byte count; .p2align an exponent. */
    if (strcmp(dir, ".align") == 0 || strcmp(dir, ".balign") == 0) {
        lex_next(l);
        if (l->tok.type == T_INT) {
            int bytes = (int)l->tok.ival;
            lex_next(l);
            if (bytes > 0) {
                if (bytes > s->align) s->align = bytes;
                sec_align(s, bytes);
            }
        }
        skip_to_eol(l);
        return;
    }
    if (strcmp(dir, ".p2align") == 0) {
        lex_next(l);
        if (l->tok.type == T_INT) {
            int n = (int)l->tok.ival;
            int bytes = 1 << (n < 0 ? 0 : n > 12 ? 12 : n);
            lex_next(l);
            if (bytes > s->align) s->align = bytes;
            sec_align(s, bytes);
        }
        skip_to_eol(l);
        return;
    }

    if (strcmp(dir, ".quad") == 0)  { emit_int_list(a, 8); return; }
    if (strcmp(dir, ".long") == 0 || strcmp(dir, ".int") == 0) {
        emit_int_list(a, 4); return;
    }
    if (strcmp(dir, ".word") == 0 || strcmp(dir, ".short") == 0
        || strcmp(dir, ".value") == 0 || strcmp(dir, ".2byte") == 0) {
        emit_int_list(a, 2); return;
    }
    if (strcmp(dir, ".byte") == 0) { emit_int_list(a, 1); return; }

    if (strcmp(dir, ".ascii") == 0)  { emit_string(a, 0); return; }
    if (strcmp(dir, ".asciz") == 0 || strcmp(dir, ".string") == 0) {
        emit_string(a, 1); return;
    }

    if (strcmp(dir, ".space") == 0 || strcmp(dir, ".zero") == 0
        || strcmp(dir, ".skip") == 0) {
        lex_next(l);
        if (l->tok.type == T_INT) {
            sec_space(s, (int)l->tok.ival);
            lex_next(l);
        }
        skip_to_eol(l);
        return;
    }

    if (strcmp(dir, ".type") == 0) {
        lex_next(l);
        if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
            int idx = sym_lookup(&a->st, l->tok.str);
            if (idx < 0) idx = sym_add(&a->st, l->tok.str);
            for (lex_next(l); l->tok.type != T_NEWLINE
                              && l->tok.type != T_EOF; lex_next(l)) {
                if ((l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT)
                    && strcmp(l->tok.str, "function") == 0)
                    sym_set_func(&a->st, idx);
            }
        } else {
            skip_to_eol(l);
        }
        return;
    }

    if (strcmp(dir, ".size") == 0) {
        int target = -1;
        lex_next(l);
        if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
            target = sym_lookup(&a->st, l->tok.str);
            if (target < 0) target = sym_add(&a->st, l->tok.str);
            lex_next(l);
        }
        if (target >= 0 && l->tok.type == T_COMMA) {
            int32_t acc = 0;
            int neg = 0;
            for (lex_next(l); l->tok.type != T_NEWLINE
                              && l->tok.type != T_EOF; lex_next(l)) {
                int32_t v;
                if (l->tok.type == T_MINUS) { neg = 1; continue; }
                if (l->tok.type == T_PLUS)  { neg = 0; continue; }
                if (l->tok.type == T_INT) {
                    v = (int32_t)l->tok.ival;
                } else if (l->tok.type == T_IDENT
                           || l->tok.type == T_DOT_IDENT) {
                    if (strcmp(l->tok.str, ".") == 0) {
                        v = (int32_t)a->sections[a->cur_section].len;
                    } else {
                        int si = sym_lookup(&a->st, l->tok.str);
                        v = si >= 0 ? (int32_t)a->st.syms[si].value : 0;
                    }
                } else {
                    break;
                }
                acc += neg ? -v : v;
                neg = 0;
            }
            if (acc < 0) acc = 0;
            sym_set_size(&a->st, target, (uint32_t)acc);
        }
        skip_to_eol(l);
        return;
    }

    if (strcmp(dir, ".include") == 0) {
        lex_next(l);
        if (l->tok.type != T_STRING)
            die("line %d: .include needs a \"file\" operand", l->tok.line);
        if (lex_include(l, l->tok.str) != 0)
            die("line %d: cannot .include \"%s\"", l->tok.line, l->tok.str);
        return;
    }

    /* metadata we accept and ignore */
    if (strcmp(dir, ".file") == 0 || strcmp(dir, ".loc") == 0
        || strcmp(dir, ".ident") == 0 || strcmp(dir, ".local") == 0
        || strcmp(dir, ".set") == 0 || strncmp(dir, ".cfi_", 5) == 0) {
        skip_to_eol(l);
        return;
    }

    warn("line %d: unknown directive '%s'", l->tok.line, dir);
    skip_to_eol(l);
}

/****************************************************************
 * Instruction and label lines
 ****************************************************************/

static void
parse_instruction(struct x86_64_asm *a, const char *mnemonic)
{
    struct lexer *l = &a->lex;
    struct x64_operand ops[X64_MAXOPS];
    int nops = 0;

    memset(ops, 0, sizeof(ops));

    if (l->tok.type != T_NEWLINE && l->tok.type != T_EOF) {
        for (;;) {
            if (nops >= X64_MAXOPS)
                die("line %d: too many operands for '%s'",
                    l->tok.line, mnemonic);
            parse_operand(a, &ops[nops]);
            nops++;
            if (l->tok.type != T_COMMA) break;
            lex_next(l);
        }
    }

    x86_64_encode(a, mnemonic, ops, nops, a->pass == 2);
    skip_to_eol(l);
}

static void
define_label(struct x86_64_asm *a, const char *name)
{
    if (a->pass == 1) {
        int idx = sym_lookup(&a->st, name);
        if (idx < 0) idx = sym_add(&a->st, name);
        sym_define(&a->st, idx, a->cur_section,
                   (uint32_t)a->sections[a->cur_section].len);
    }
}

static void
run_pass(struct x86_64_asm *a)
{
    struct lexer *l = &a->lex;
    int k;

    a->cur_section = XSEC_TEXT;
    for (k = 0; k < a->n_numlabels; k++)
        a->numlabels[k].count = 0;      /* same numbers each pass, recount */
    lex_next(l);

    while (l->tok.type != T_EOF) {
        if (l->tok.type == T_NEWLINE) {
            lex_next(l);
            continue;
        }

        /* numeric local label definition: N: */
        if (l->tok.type == T_INT) {
            int num = (int)l->tok.ival;
            struct lexer saved = *l;
            lex_next(l);
            if (l->tok.type == T_COLON && num >= 0) {
                char buf[32];
                int *cnt = numlabel_count(a, num);
                lex_next(l);
                if (a->pass == 1) {
                    int idx;
                    snprintf(buf, sizeof(buf), ".Lnum_%d_%d", num, *cnt);
                    idx = sym_lookup(&a->st, buf);
                    if (idx < 0) idx = sym_add(&a->st, buf);
                    sym_define(&a->st, idx, a->cur_section,
                               (uint32_t)a->sections[a->cur_section].len);
                }
                (*cnt)++;
                continue;
            }
            lex_restore(l, &saved);
            skip_to_eol(l);
            if (l->tok.type == T_NEWLINE) lex_next(l);
            continue;
        }

        /* dotted token: a directive or a dotted label (.L1:). */
        if (l->tok.type == T_DOT_IDENT) {
            const char *name = l->tok.str;
            struct lexer saved = *l;
            lex_next(l);
            if (l->tok.type == T_COLON) {
                lex_next(l);
                define_label(a, name);
                continue;
            }
            lex_restore(l, &saved);
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

/****************************************************************
 * Public interface
 ****************************************************************/

void
x86_64_init(struct x86_64_asm *a, const char *src)
{
    int k;

    memset(a, 0, sizeof(*a));
    arena_init(&a->arena);
    a->lex.arena = &a->arena;
    a->st.arena = &a->arena;
    for (k = 0; k < XSEC_COUNT; k++)
        a->sections[k].little_endian = 1;
    lex_init(&a->lex, src, '#', 0);
}

void
x86_64_free(struct x86_64_asm *a)
{
    int k;
    for (k = 0; k < XSEC_COUNT; k++) {
        free(a->sections[k].data);
        free(a->sections[k].relocs);
    }
    free(a->st.syms);
    free(a->numlabels);
    free(a->lex.str_buf);
    arena_free(&a->arena);
}

void
x86_64_assemble(struct x86_64_asm *a)
{
    int k;

    /* pass 1: lay out labels (bytes emitted are placeholders, discarded). */
    a->pass = 1;
    for (k = 0; k < XSEC_COUNT; k++) {
        a->sections[k].len = 0;
        a->sections[k].nrelocs = 0;
    }
    lex_rewind(&a->lex);
    run_pass(a);

    /* pass 2: real bytes and relocations, same layout. */
    a->pass = 2;
    for (k = 0; k < XSEC_COUNT; k++) {
        a->sections[k].len = 0;
        a->sections[k].nrelocs = 0;
    }
    lex_rewind(&a->lex);
    run_pass(a);
}
