/* aarch64_parse.c : register/condition tables, the GAS operand and directive
 * grammar, and the two-pass driver for skj-as-aarch64.
 *
 * The scanner and object bookkeeping are the shared skeleton (asm_lex.c,
 * asm_obj.c); this file supplies the AArch64 grammar the arm64 backends emit.
 * Instructions are a fixed 32-bit word, so there is no branch relaxation: the
 * encoder emits into the current section on both passes (rewound between them),
 * pass 1 laying out labels and the literal pools, pass 2 writing final words and
 * relocations.
 */

#include "aarch64.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Register and condition tables
 ****************************************************************/

/* Returns the class (A_RC_*), sets *num (0..31), *size (bytes), *is_sp. */
int
a64_reg(const char *name, int *num, int *size, int *is_sp)
{
    int n;
    char *end;

    *is_sp = 0;
    if (strcmp(name, "sp") == 0)  { *num = 31; *size = 8; *is_sp = 1; return A_RC_GPR; }
    if (strcmp(name, "wsp") == 0) { *num = 31; *size = 4; *is_sp = 1; return A_RC_GPR; }
    if (strcmp(name, "xzr") == 0) { *num = 31; *size = 8; return A_RC_GPR; }
    if (strcmp(name, "wzr") == 0) { *num = 31; *size = 4; return A_RC_GPR; }

    if ((name[0] == 'x' || name[0] == 'w' || name[0] == 'd' ||
         name[0] == 's' || name[0] == 'h') && name[1] >= '0' && name[1] <= '9') {
        n = (int)strtol(name + 1, &end, 10);
        if (*end != '\0')
            return A_RC_NONE;
        if ((name[0] == 'x' || name[0] == 'w') && n >= 0 && n <= 30) {
            *num = n; *size = name[0] == 'x' ? 8 : 4; return A_RC_GPR;
        }
        if (name[0] == 'd' && n >= 0 && n <= 31) { *num = n; *size = 8; return A_RC_FP; }
        if (name[0] == 's' && n >= 0 && n <= 31) { *num = n; *size = 4; return A_RC_FP; }
        if (name[0] == 'h' && n >= 0 && n <= 31) { *num = n; *size = 2; return A_RC_FP; }
    }
    return A_RC_NONE;
}

int
a64_cond(const char *name)
{
    static const struct { const char *s; int cc; } tab[] = {
        { "eq", 0 }, { "ne", 1 }, { "cs", 2 }, { "hs", 2 }, { "cc", 3 },
        { "lo", 3 }, { "mi", 4 }, { "pl", 5 }, { "vs", 6 }, { "vc", 7 },
        { "hi", 8 }, { "ls", 9 }, { "ge", 10 }, { "lt", 11 }, { "gt", 12 },
        { "le", 13 }, { "al", 14 }, { "nv", 15 }, { NULL, 0 },
    };
    int i;
    for (i = 0; tab[i].s; i++)
        if (strcmp(tab[i].s, name) == 0)
            return tab[i].cc;
    return -1;
}

/****************************************************************
 * Numeric local labels (1:/1f/1b), as in the other assemblers
 ****************************************************************/

static int
numeric_ref(const char *str, int *num, char *dir)
{
    size_t n = strlen(str), i;
    if (n < 2 || (str[n - 1] != 'f' && str[n - 1] != 'b'))
        return 0;
    for (i = 0; i < n - 1; i++)
        if (str[i] < '0' || str[i] > '9')
            return 0;
    *num = atoi(str);
    *dir = str[n - 1];
    return 1;
}

static int *
numlabel_count(struct a64_asm *a, int num)
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
numeric_ref_name(struct a64_asm *a, int num, char dir)
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

/* A `#imm` value (with optional leading minus). */
static long
parse_hash_imm(struct lexer *l)
{
    long v;
    lex_next(l);                        /* consume '#' */
    if (l->tok.type == T_MINUS) {
        lex_next(l);
        v = -l->tok.ival;
    } else {
        v = l->tok.ival;
    }
    lex_next(l);
    return v;
}

/* Parse `[ base {, #imm | , Xm} ]`. */
static void
parse_mem(struct a64_asm *a, struct a64_operand *op)
{
    struct lexer *l = &a->lex;
    int num, sz, sp;

    op->kind = AO_MEM;
    op->base = -1;
    op->index = -1;

    lex_next(l);                        /* consume '[' */
    if (l->tok.type != T_IDENT || a64_reg(l->tok.str, &num, &sz, &sp) != A_RC_GPR)
        die("line %d: expected base register in memory operand", l->tok.line);
    op->base = num;
    lex_next(l);
    if (l->tok.type == T_COMMA) {
        lex_next(l);
        if (l->tok.type == T_HASH) {
            op->disp = parse_hash_imm(l);
        } else if (l->tok.type == T_IDENT &&
                   a64_reg(l->tok.str, &num, &sz, &sp) == A_RC_GPR) {
            op->index = num;
            lex_next(l);
        } else {
            die("line %d: bad memory operand", l->tok.line);
        }
    }
    if (l->tok.type != T_RBRACKET)
        die("line %d: expected ']' in memory operand", l->tok.line);
    lex_next(l);
}

static void
parse_operand(struct a64_asm *a, struct a64_operand *op)
{
    struct lexer *l = &a->lex;
    int num, sz, sp, cc;

    memset(op, 0, sizeof(*op));
    op->base = -1;
    op->index = -1;

    if (l->tok.type == T_HASH) {                /* #imm */
        op->kind = AO_IMM;
        op->disp = parse_hash_imm(l);
        return;
    }
    if (l->tok.type == T_EQUAL) {               /* =const or =sym */
        lex_next(l);
        op->kind = AO_LIT;
        if (l->tok.type == T_INT) {
            op->disp = l->tok.ival;
            lex_next(l);
        } else if (l->tok.type == T_MINUS) {
            lex_next(l);
            op->disp = -l->tok.ival;
            lex_next(l);
        } else if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
            op->is_sym = 1;
            op->sym = arena_strdup(&a->arena, l->tok.str);
            lex_next(l);
            op->add = (int32_t)parse_addend(l);
        } else {
            die("line %d: bad =literal operand", l->tok.line);
        }
        return;
    }
    if (l->tok.type == T_LBRACKET) {            /* [mem] */
        parse_mem(a, op);
        return;
    }
    if (l->tok.type == T_IDENT) {
        /* register? */
        int cls = a64_reg(l->tok.str, &num, &sz, &sp);
        if (cls != A_RC_NONE) {
            op->kind = AO_REG;
            op->rclass = cls;
            op->reg = num;
            op->size = sz;
            op->is_sp = sp;
            lex_next(l);
            return;
        }
        /* shift modifier `lsl #n` (only on add/sub immediates here) */
        if (strcmp(l->tok.str, "lsl") == 0 || strcmp(l->tok.str, "lsr") == 0 ||
            strcmp(l->tok.str, "asr") == 0) {
            lex_next(l);
            op->kind = AO_SHIFT;
            if (l->tok.type == T_HASH)
                op->disp = parse_hash_imm(l);
            return;
        }
        /* condition code (cset) */
        cc = a64_cond(l->tok.str);
        if (cc >= 0) {
            op->kind = AO_COND;
            op->cond = cc;
            lex_next(l);
            return;
        }
        /* otherwise a branch-target symbol or numeric local reference */
        {
            int n; char dir;
            op->kind = AO_SYM;
            op->is_sym = 1;
            if (numeric_ref(l->tok.str, &n, &dir))
                op->sym = numeric_ref_name(a, n, dir);
            else
                op->sym = arena_strdup(&a->arena, l->tok.str);
            lex_next(l);
            op->add = (int32_t)parse_addend(l);
            if (l->tok.type == T_AT) {      /* Mach-O `_sym@PAGE` / `@PAGEOFF` */
                lex_next(l);
                if (l->tok.type != T_IDENT)
                    die("line %d: expected PAGE/PAGEOFF after '@'", l->tok.line);
                if (strcmp(l->tok.str, "PAGE") == 0)
                    op->pageref = 1;
                else if (strcmp(l->tok.str, "PAGEOFF") == 0)
                    op->pageref = 2;
                else
                    die("line %d: unknown relocation suffix @%s",
                        l->tok.line, l->tok.str);
                lex_next(l);
            }
            return;
        }
    }
    if (l->tok.type == T_DOT_IDENT) {           /* .L branch target */
        op->kind = AO_SYM;
        op->is_sym = 1;
        op->sym = arena_strdup(&a->arena, l->tok.str);
        lex_next(l);
        op->add = (int32_t)parse_addend(l);
        return;
    }
    die("line %d: bad operand", l->tok.line);
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

static void
emit_int_list(struct a64_asm *a, int width)
{
    struct section *s = &a->sections[a->cur_section];
    struct lexer *l = &a->lex;

    for (;;) {
        lex_next(l);
        if (l->tok.type == T_INT) {
            long v = l->tok.ival;
            if (width == 1) sec_emit8(s, (uint8_t)v);
            else if (width == 2) sec_emit16(s, (uint16_t)v);
            else if (width == 4) sec_emit32(s, (uint32_t)v);
            else { sec_emit32(s, (uint32_t)v); sec_emit32(s, (uint32_t)((uint64_t)v >> 32)); }
            lex_next(l);
        } else if (l->tok.type == T_MINUS) {
            lex_next(l);
            if (l->tok.type == T_INT) {
                long v = -l->tok.ival;
                if (width == 1) sec_emit8(s, (uint8_t)v);
                else if (width == 2) sec_emit16(s, (uint16_t)v);
                else if (width == 4) sec_emit32(s, (uint32_t)v);
                else { sec_emit32(s, (uint32_t)v); sec_emit32(s, (uint32_t)((uint64_t)v >> 32)); }
                lex_next(l);
            }
        } else if ((l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT)
                   && (width == 4 || width == 8)) {
            int idx = -1;
            uint32_t off = (uint32_t)s->len;
            if (a->pass == 2) {
                idx = sym_lookup(&a->st, l->tok.str);
                if (idx < 0) idx = sym_add(&a->st, l->tok.str);
            }
            lex_next(l);
            int32_t addend = (int32_t)parse_addend(l);
            if (a->pass == 2)
                sec_add_reloc_t(s, off, idx, addend,
                                width == 8 ? R_AARCH64_ABS64 : R_AARCH64_ABS32);
            if (width == 8) { sec_emit32(s, 0); sec_emit32(s, 0); }
            else sec_emit32(s, 0);
        } else {
            break;
        }
        if (l->tok.type != T_COMMA) break;
    }
}

static void
emit_string(struct a64_asm *a, int nul)
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

/* Switch sections, flushing any pending literal pool into the old section
   first (the pool must live beside the ldr-literal instructions that read it). */
static void
set_section(struct a64_asm *a, int sec)
{
    if (a->cur_section == ASEC_TEXT && a->nlits > 0)
        a64_flush_litpool(a, a->pass == 2);
    a->cur_section = sec;
}

static void
handle_directive(struct a64_asm *a, const char *dir)
{
    struct lexer *l = &a->lex;
    struct section *s = &a->sections[a->cur_section];

    if (strcmp(dir, ".text") == 0)   { set_section(a, ASEC_TEXT); return; }
    if (strcmp(dir, ".data") == 0)   { set_section(a, ASEC_DATA); return; }
    if (strcmp(dir, ".rodata") == 0) { set_section(a, ASEC_RODATA); return; }
    if (strcmp(dir, ".bss") == 0)    { set_section(a, ASEC_BSS); return; }

    if (strcmp(dir, ".ltorg") == 0 || strcmp(dir, ".pool") == 0) {
        a64_flush_litpool(a, a->pass == 2);
        skip_to_eol(l);
        return;
    }

    if (strcmp(dir, ".section") == 0) {
        lex_next(l);
        if (l->tok.type == T_DOT_IDENT || l->tok.type == T_IDENT) {
            const char *n = l->tok.str;
            if (n[0] == '.') {              /* ELF section name */
                if (strncmp(n, ".text", 5) == 0)        set_section(a, ASEC_TEXT);
                else if (strncmp(n, ".rodata", 7) == 0) set_section(a, ASEC_RODATA);
                else if (strncmp(n, ".data", 5) == 0)   set_section(a, ASEC_DATA);
                else if (strncmp(n, ".bss", 4) == 0)    set_section(a, ASEC_BSS);
            } else {                        /* Mach-O `__SEG,__sect` */
                lex_next(l);
                if (l->tok.type == T_COMMA)
                    lex_next(l);
                if (l->tok.type == T_IDENT) {
                    const char *sn = l->tok.str;
                    if (strcmp(sn, "__text") == 0)         set_section(a, ASEC_TEXT);
                    else if (strcmp(sn, "__const") == 0)   set_section(a, ASEC_RODATA);
                    else if (strcmp(sn, "__cstring") == 0) set_section(a, ASEC_RODATA);
                    else if (strcmp(sn, "__data") == 0)    set_section(a, ASEC_DATA);
                    else if (strcmp(sn, "__bss") == 0)     set_section(a, ASEC_BSS);
                }
            }
        }
        skip_to_eol(l);
        return;
    }

    if (strcmp(dir, ".zerofill") == 0) {
        /* Mach-O bss declaration: .zerofill __DATA,__bss,symbol,size[,align] */
        const char *name;
        long size = 0, align = 0;
        int idx;
        lex_next(l);                            /* segment name */
        if (l->tok.type == T_IDENT) lex_next(l);
        if (l->tok.type == T_COMMA)  lex_next(l);
        if (l->tok.type == T_IDENT) lex_next(l);    /* section name */
        if (l->tok.type == T_COMMA)  lex_next(l);
        if (l->tok.type != T_IDENT)
            die("line %d: .zerofill expects a symbol", l->tok.line);
        name = arena_strdup(&a->arena, l->tok.str);
        lex_next(l);
        if (l->tok.type == T_COMMA)  lex_next(l);
        if (l->tok.type == T_INT) { size = l->tok.ival; lex_next(l); }
        if (l->tok.type == T_COMMA) {
            lex_next(l);
            if (l->tok.type == T_INT) { align = l->tok.ival; lex_next(l); }
        }
        set_section(a, ASEC_BSS);
        if (align > 0)
            sec_align(&a->sections[ASEC_BSS], 1 << align);
        idx = sym_lookup(&a->st, name);
        if (idx < 0) idx = sym_add(&a->st, name);
        sym_define(&a->st, idx, ASEC_BSS, (uint32_t)a->sections[ASEC_BSS].len);
        sec_space(&a->sections[ASEC_BSS], (int)size);
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

    /* ELF arm64 as: .align / .p2align take a power-of-two exponent; .balign a
       byte count. */
    if (strcmp(dir, ".align") == 0 || strcmp(dir, ".p2align") == 0) {
        lex_next(l);
        if (l->tok.type == T_INT) {
            int n = (int)l->tok.ival;
            int bytes = 1 << (n < 0 ? 0 : n > 16 ? 16 : n);
            lex_next(l);
            if (bytes > s->align) s->align = bytes;
            sec_align(s, bytes);
        }
        skip_to_eol(l);
        return;
    }
    if (strcmp(dir, ".balign") == 0) {
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

    if (strcmp(dir, ".quad") == 0 || strcmp(dir, ".xword") == 0) { emit_int_list(a, 8); return; }
    if (strcmp(dir, ".word") == 0 || strcmp(dir, ".long") == 0 || strcmp(dir, ".int") == 0) {
        emit_int_list(a, 4); return;
    }
    if (strcmp(dir, ".short") == 0 || strcmp(dir, ".hword") == 0 || strcmp(dir, ".2byte") == 0) {
        emit_int_list(a, 2); return;
    }
    if (strcmp(dir, ".byte") == 0) { emit_int_list(a, 1); return; }

    if (strcmp(dir, ".ascii") == 0)  { emit_string(a, 0); return; }
    if (strcmp(dir, ".asciz") == 0 || strcmp(dir, ".string") == 0) {
        emit_string(a, 1); return;
    }

    if (strcmp(dir, ".space") == 0 || strcmp(dir, ".zero") == 0 || strcmp(dir, ".skip") == 0) {
        lex_next(l);
        if (l->tok.type == T_INT) { sec_space(s, (int)l->tok.ival); lex_next(l); }
        skip_to_eol(l);
        return;
    }

    if (strcmp(dir, ".type") == 0) {
        lex_next(l);
        if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
            int idx = sym_lookup(&a->st, l->tok.str);
            if (idx < 0) idx = sym_add(&a->st, l->tok.str);
            for (lex_next(l); l->tok.type != T_NEWLINE && l->tok.type != T_EOF; lex_next(l))
                if ((l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT)
                    && strcmp(l->tok.str, "function") == 0)
                    sym_set_func(&a->st, idx);
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
            int32_t acc = 0, neg = 0;
            for (lex_next(l); l->tok.type != T_NEWLINE && l->tok.type != T_EOF; lex_next(l)) {
                int32_t v;
                if (l->tok.type == T_MINUS) { neg = 1; continue; }
                if (l->tok.type == T_PLUS)  { neg = 0; continue; }
                if (l->tok.type == T_INT) v = (int32_t)l->tok.ival;
                else if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
                    if (strcmp(l->tok.str, ".") == 0)
                        v = (int32_t)a->sections[a->cur_section].len;
                    else {
                        int si = sym_lookup(&a->st, l->tok.str);
                        v = si >= 0 ? (int32_t)a->st.syms[si].value : 0;
                    }
                } else break;
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

    /* metadata accepted and ignored */
    if (strcmp(dir, ".file") == 0 || strcmp(dir, ".loc") == 0 ||
        strcmp(dir, ".ident") == 0 || strcmp(dir, ".local") == 0 ||
        strcmp(dir, ".arch") == 0 || strcmp(dir, ".arch_extension") == 0 ||
        strcmp(dir, ".cpu") == 0 || strncmp(dir, ".cfi_", 5) == 0) {
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
parse_instruction(struct a64_asm *a, const char *mnemonic)
{
    struct lexer *l = &a->lex;
    struct a64_operand ops[A64_MAXOPS];
    int nops = 0;

    memset(ops, 0, sizeof(ops));
    if (l->tok.type != T_NEWLINE && l->tok.type != T_EOF) {
        for (;;) {
            if (nops >= A64_MAXOPS)
                die("line %d: too many operands for '%s'", l->tok.line, mnemonic);
            parse_operand(a, &ops[nops]);
            nops++;
            if (l->tok.type != T_COMMA) break;
            lex_next(l);
        }
    }
    a64_encode(a, mnemonic, ops, nops, a->pass == 2);
    skip_to_eol(l);
}

static void
define_label(struct a64_asm *a, const char *name)
{
    if (a->pass == 1) {
        int idx = sym_lookup(&a->st, name);
        if (idx < 0) idx = sym_add(&a->st, name);
        sym_define(&a->st, idx, a->cur_section,
                   (uint32_t)a->sections[a->cur_section].len);
    }
}

static void
run_pass(struct a64_asm *a)
{
    struct lexer *l = &a->lex;
    int k;

    a->cur_section = ASEC_TEXT;
    a->nlits = 0;
    a->nlitrefs = 0;
    for (k = 0; k < a->n_numlabels; k++)
        a->numlabels[k].count = 0;
    lex_next(l);

    while (l->tok.type != T_EOF) {
        if (l->tok.type == T_NEWLINE) { lex_next(l); continue; }

        if (l->tok.type == T_INT) {             /* numeric local label N: */
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

    /* flush any trailing literal pool at end of the text section */
    if (a->nlits > 0) {
        int save = a->cur_section;
        a->cur_section = ASEC_TEXT;
        a64_flush_litpool(a, a->pass == 2);
        a->cur_section = save;
    }
}

/****************************************************************
 * Public interface
 ****************************************************************/

void
a64_init(struct a64_asm *a, const char *src)
{
    int k;
    memset(a, 0, sizeof(*a));
    arena_init(&a->arena);
    a->lex.arena = &a->arena;
    a->st.arena = &a->arena;
    for (k = 0; k < ASEC_COUNT; k++)
        a->sections[k].little_endian = 1;
    /* AArch64 GAS has no single-char line comment (# is the immediate sigil);
       comments are the two-slash line form (handled in the lexer) and the
       C-style block form.  A NUL comment_ch disables the single-char path (the
       EOF check shields it). */
    lex_init(&a->lex, src, '\0', 0);
}

void
a64_free(struct a64_asm *a)
{
    int k;
    for (k = 0; k < ASEC_COUNT; k++) {
        free(a->sections[k].data);
        free(a->sections[k].relocs);
    }
    free(a->st.syms);
    free(a->numlabels);
    free(a->lits);
    free(a->litrefs);
    free(a->lex.str_buf);
    arena_free(&a->arena);
}

void
a64_assemble(struct a64_asm *a)
{
    int k;

    a->pass = 1;
    for (k = 0; k < ASEC_COUNT; k++) { a->sections[k].len = 0; a->sections[k].nrelocs = 0; }
    lex_rewind(&a->lex);
    run_pass(a);

    a->pass = 2;
    for (k = 0; k < ASEC_COUNT; k++) { a->sections[k].len = 0; a->sections[k].nrelocs = 0; }
    lex_rewind(&a->lex);
    run_pass(a);
}
