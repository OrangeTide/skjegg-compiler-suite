/* parse.c : two-pass GAS syntax parser for ColdFire assembler */

#include "as.h"
#include "dwarf.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Register parsing
 ****************************************************************/

static int
parse_dreg(const char *name, int *reg)
{
    if (name[0] == 'd' && name[1] >= '0' && name[1] <= '7' && name[2] == '\0') {
        *reg = name[1] - '0';
        return 1;
    }
    return 0;
}

static int
parse_areg(const char *name, int *reg)
{
    if (name[0] == 'a' && name[1] >= '0' && name[1] <= '7' && name[2] == '\0') {
        *reg = name[1] - '0';
        return 1;
    }
    if (strcmp(name, "sp") == 0) { *reg = 7; return 1; }
    if (strcmp(name, "fp") == 0) { *reg = 6; return 1; }
    return 0;
}

static int
parse_fpreg(const char *name, int *reg)
{
    if (name[0] == 'f' && name[1] == 'p'
        && name[2] >= '0' && name[2] <= '7' && name[3] == '\0') {
        *reg = name[2] - '0';
        return 1;
    }
    return 0;
}

/****************************************************************
 * Mnemonic + size suffix splitting
 ****************************************************************/

static int
is_size_suffix(const char *s)
{
    return s[0] == '.' && s[1] != '\0' && s[2] == '\0' &&
           (s[1] == 'b' || s[1] == 'w' || s[1] == 'l' ||
            s[1] == 's' || s[1] == 'd' || s[1] == 'x');
}

static int
split_mnemonic(const char *tok, char *base, int *size)
{
    int len = (int)strlen(tok);
    int dot_pos = -1;
    int k;

    for (k = 0; k < len; k++) {
        if (tok[k] == '.') {
            dot_pos = k;
            break;
        }
    }

    if (dot_pos < 0) {
        strcpy(base, tok);
        *size = 0;
        return 1;
    }

    memcpy(base, tok, dot_pos);
    base[dot_pos] = '\0';

    if (dot_pos + 1 < len) {
        switch (tok[dot_pos + 1]) {
        case 'b': *size = 1; break;
        case 'w': *size = 2; break;
        case 'l': *size = 4; break;
        case 's': *size = 4; break;
        case 'd': *size = 8; break;
        case 'x': *size = 0; break;
        default: *size = 0; break;
        }
    } else {
        *size = 0;
    }

    return 1;
}

/****************************************************************
 * Register list parsing (for movem)
 ****************************************************************/

static uint16_t
reglist_mask(int type, int lo, int hi)
{
    uint16_t mask = 0;
    int k;

    for (k = lo; k <= hi; k++) {
        if (type == 'd')
            mask |= (uint16_t)(1 << k);
        else
            mask |= (uint16_t)(1 << (k + 8));
    }
    return mask;
}

/****************************************************************
 * Operand parsing
 ****************************************************************/

static int
parse_operand(struct lexer *l, struct operand *op, int allow_reglist)
{
    int reg;

    memset(op, 0, sizeof(*op));

    if (l->tok.type == T_HASH) {
        lex_next(l);
        op->type = OP_IMM;
        op->imm = 0;
        op->sym = NULL;
        if (l->tok.type == T_MINUS) {
            lex_next(l);
            if (l->tok.type == T_INT) {
                op->imm = -l->tok.ival;
                lex_next(l);
            } else {
                return 0;
            }
        } else if (l->tok.type == T_INT) {
            op->imm = l->tok.ival;
            lex_next(l);
        } else if (l->tok.type == T_IDENT || l->tok.type == T_DOT_IDENT) {
            op->sym = arena_strdup(l->arena, l->tok.str);
            lex_next(l);
        } else {
            return 0;
        }
        return 1;
    }

    if (l->tok.type == T_MINUS) {
        lex_next(l);
        if (l->tok.type == T_LPAREN) {
            lex_next(l);
            if (l->tok.type == T_IDENT && parse_areg(l->tok.str, &reg)) {
                lex_next(l);
                if (l->tok.type == T_RPAREN) {
                    lex_next(l);
                    op->type = OP_PREDEC;
                    op->reg = reg;
                    return 1;
                }
            }
            return 0;
        }
        if (l->tok.type == T_INT) {
            long val = -l->tok.ival;
            lex_next(l);
            if (l->tok.type == T_LPAREN) {
                lex_next(l);
                if (l->tok.type == T_IDENT && parse_areg(l->tok.str, &reg)) {
                    lex_next(l);
                    if (l->tok.type == T_RPAREN) {
                        lex_next(l);
                        op->type = OP_DISP;
                        op->reg = reg;
                        op->imm = val;
                        return 1;
                    }
                }
            }
            return 0;
        }
        return 0;
    }

    if (l->tok.type == T_LPAREN) {
        lex_next(l);
        if (l->tok.type == T_IDENT && parse_areg(l->tok.str, &reg)) {
            lex_next(l);
            if (l->tok.type == T_RPAREN) {
                lex_next(l);
                if (l->tok.type == T_PLUS) {
                    lex_next(l);
                    op->type = OP_POSTINC;
                    op->reg = reg;
                    return 1;
                }
                op->type = OP_IND;
                op->reg = reg;
                return 1;
            }
        }
        return 0;
    }

    if (l->tok.type == T_INT) {
        long val = l->tok.ival;
        lex_next(l);
        if (l->tok.type == T_LPAREN) {
            lex_next(l);
            if (l->tok.type == T_IDENT && parse_areg(l->tok.str, &reg)) {
                lex_next(l);
                if (l->tok.type == T_RPAREN) {
                    lex_next(l);
                    op->type = OP_DISP;
                    op->reg = reg;
                    op->imm = val;
                    return 1;
                }
            }
            return 0;
        }
        op->type = OP_IMM;
        op->imm = val;
        return 1;
    }

    if (l->tok.type == T_IDENT) {
        if (parse_dreg(l->tok.str, &reg)) {
            lex_next(l);
            if (allow_reglist && l->tok.type == T_MINUS) {
                struct lexer saved = *l;
                lex_next(l);
                if (l->tok.type == T_IDENT) {
                    int hi;
                    if (parse_dreg(l->tok.str, &hi)) {
                        op->type = OP_REGLIST;
                        op->regmask = reglist_mask('d', reg, hi);
                        lex_next(l);
                        return 1;
                    }
                }
                *l = saved;
            }
            op->type = OP_DREG;
            op->reg = reg;
            return 1;
        }
        if (parse_areg(l->tok.str, &reg)) {
            lex_next(l);
            op->type = OP_AREG;
            op->reg = reg;
            return 1;
        }
        if (parse_fpreg(l->tok.str, &reg)) {
            lex_next(l);
            op->type = OP_FPREG;
            op->reg = reg;
            return 1;
        }
        op->type = OP_ABS;
        op->sym = arena_strdup(l->arena, l->tok.str);
        lex_next(l);
        return 1;
    }

    if (l->tok.type == T_DOT_IDENT) {
        op->type = OP_ABS;
        op->sym = arena_strdup(l->arena, l->tok.str);
        lex_next(l);
        return 1;
    }

    return 0;
}

/****************************************************************
 * Directive handling
 ****************************************************************/

static void
skip_to_eol(struct lexer *l)
{
    while (l->tok.type != T_NEWLINE && l->tok.type != T_EOF)
        lex_next(l);
}

/* Append one line-table row (emit pass only; see .loc above). */
static void
cf_add_locrow(struct assembler *a, uint32_t offset, int line)
{
    if (a->n_locrows >= a->locrow_cap) {
        a->locrow_cap = a->locrow_cap ? a->locrow_cap * 2 : 64;
        a->locrows = realloc(a->locrows,
                             (size_t)a->locrow_cap * sizeof *a->locrows);
    }
    a->locrows[a->n_locrows].offset = offset;
    a->locrows[a->n_locrows].line = line;
    a->n_locrows++;
}

static void
handle_directive(struct assembler *a, const char *dir)
{
    struct section *s = &a->sections[a->cur_section];

    if (strcmp(dir, ".text") == 0) {
        a->cur_section = SEC_TEXT;
        return;
    }
    if (strcmp(dir, ".include") == 0) {
        lex_next(&a->lex);
        if (a->lex.tok.type != T_STRING)
            die("line %d: .include needs a \"file\" operand", a->lex.tok.line);
        if (lex_include(&a->lex, a->lex.tok.str) != 0)
            die("line %d: cannot .include \"%s\"", a->lex.tok.line, a->lex.tok.str);
        return;         /* the included tokens come next; the parent resumes on pop */
    }
    if (strcmp(dir, ".data") == 0) {
        a->cur_section = SEC_DATA;
        return;
    }
    if (strcmp(dir, ".bss") == 0) {
        a->cur_section = SEC_BSS;
        return;
    }
    if (strcmp(dir, ".rodata") == 0) {
        a->cur_section = SEC_RODATA;
        return;
    }

    if (strcmp(dir, ".section") == 0) {
        lex_next(&a->lex);
        if (a->lex.tok.type == T_DOT_IDENT || a->lex.tok.type == T_IDENT) {
            const char *n = a->lex.tok.str;
            if (strncmp(n, ".text", 5) == 0)        a->cur_section = SEC_TEXT;
            else if (strncmp(n, ".rodata", 7) == 0) a->cur_section = SEC_RODATA;
            else if (strncmp(n, ".data", 5) == 0)   a->cur_section = SEC_DATA;
            else if (strncmp(n, ".bss", 4) == 0)    a->cur_section = SEC_BSS;
            else a->cur_section = SEC_DATA;
        }
        skip_to_eol(&a->lex);
        return;
    }

    if (strcmp(dir, ".align") == 0) {
        lex_next(&a->lex);
        if (a->lex.tok.type == T_INT) {
            int align = (int)a->lex.tok.ival;
            /* A non-positive alignment (`.align 0`, or a negative value from a
               corrupt source) has no meaning and would divide by zero below;
               ignore it. */
            if (align > 0) {
                /* record the section's strongest alignment so the ELF writer
                   can set sh_addralign and the linker places it aligned */
                if (align > s->align)
                    s->align = align;
                if (a->pass == 2)
                    sec_align(s, align);
                else {
                    while (s->len % align) s->len++;
                }
            }
            lex_next(&a->lex);
        }
        return;
    }

    if (strcmp(dir, ".globl") == 0 || strcmp(dir, ".weak") == 0) {
        int is_weak = (dir[1] == 'w');
        lex_next(&a->lex);
        if (a->lex.tok.type == T_IDENT || a->lex.tok.type == T_DOT_IDENT) {
            int idx = sym_lookup(&a->st, a->lex.tok.str);
            if (idx < 0)
                idx = sym_add(&a->st, a->lex.tok.str);
            if (is_weak)
                sym_set_weak(&a->st, idx);
            else
                sym_set_global(&a->st, idx);
            lex_next(&a->lex);
        }
        return;
    }

    if (strcmp(dir, ".long") == 0) {
        for (;;) {
            lex_next(&a->lex);
            if (a->lex.tok.type == T_INT) {
                if (a->pass == 2)
                    sec_emit32(s, (uint32_t)a->lex.tok.ival);
                else
                    s->len += 4;
                lex_next(&a->lex);
            } else if (a->lex.tok.type == T_MINUS) {
                lex_next(&a->lex);
                if (a->lex.tok.type == T_INT) {
                    if (a->pass == 2)
                        sec_emit32(s, (uint32_t)(-(int32_t)a->lex.tok.ival));
                    else
                        s->len += 4;
                    lex_next(&a->lex);
                }
            } else if (a->lex.tok.type == T_IDENT
                       || a->lex.tok.type == T_DOT_IDENT) {
                /* resolve the symbol while tok.str is still valid, then parse
                   an optional "+ N" / "- N" addend (a symbol-relative data
                   reference like `.long sym + 8`, an address constant) */
                int idx = -1;
                if (a->pass == 2) {
                    idx = sym_lookup(&a->st, a->lex.tok.str);
                    if (idx < 0)
                        idx = sym_add(&a->st, a->lex.tok.str);
                }
                lex_next(&a->lex);
                int32_t addend = 0;
                if (a->lex.tok.type == T_PLUS || a->lex.tok.type == T_MINUS) {
                    int neg = a->lex.tok.type == T_MINUS;
                    lex_next(&a->lex);
                    if (a->lex.tok.type == T_INT) {
                        addend = neg ? -(int32_t)a->lex.tok.ival
                                     : (int32_t)a->lex.tok.ival;
                        lex_next(&a->lex);
                    }
                }
                if (a->pass == 2) {
                    sec_add_reloc(s, s->len, idx, addend);
                    sec_emit32(s, 0);
                } else {
                    s->len += 4;
                }
            } else {
                break;
            }
            if (a->lex.tok.type != T_COMMA) break;
        }
        return;
    }

    if (strcmp(dir, ".short") == 0) {
        for (;;) {
            lex_next(&a->lex);
            if (a->lex.tok.type == T_INT) {
                if (a->pass == 2)
                    sec_emit16(s, (uint16_t)a->lex.tok.ival);
                else
                    s->len += 2;
                lex_next(&a->lex);
            } else if (a->lex.tok.type == T_MINUS) {
                lex_next(&a->lex);
                if (a->lex.tok.type == T_INT) {
                    if (a->pass == 2)
                        sec_emit16(s, (uint16_t)(-(int16_t)a->lex.tok.ival));
                    else
                        s->len += 2;
                    lex_next(&a->lex);
                }
            } else {
                break;
            }
            if (a->lex.tok.type != T_COMMA) break;
        }
        return;
    }

    if (strcmp(dir, ".byte") == 0) {
        for (;;) {
            lex_next(&a->lex);
            if (a->lex.tok.type == T_INT) {
                if (a->pass == 2)
                    sec_emit8(s, (uint8_t)a->lex.tok.ival);
                else
                    s->len += 1;
                lex_next(&a->lex);
            } else if (a->lex.tok.type == T_MINUS) {
                lex_next(&a->lex);
                if (a->lex.tok.type == T_INT) {
                    if (a->pass == 2)
                        sec_emit8(s, (uint8_t)(-(int8_t)a->lex.tok.ival));
                    else
                        s->len += 1;
                    lex_next(&a->lex);
                }
            } else {
                break;
            }
            if (a->lex.tok.type != T_COMMA) break;
        }
        return;
    }

    if (strcmp(dir, ".ascii") == 0) {
        lex_next(&a->lex);
        if (a->lex.tok.type == T_STRING) {
            if (a->pass == 2) {
                int k;
                for (k = 0; k < a->lex.tok.str_len; k++)
                    sec_emit8(s, (uint8_t)a->lex.tok.str[k]);
            } else {
                s->len += a->lex.tok.str_len;
            }
            lex_next(&a->lex);
        }
        return;
    }

    if (strcmp(dir, ".asciz") == 0 || strcmp(dir, ".string") == 0) {
        lex_next(&a->lex);
        if (a->lex.tok.type == T_STRING) {
            if (a->pass == 2) {
                int k;
                for (k = 0; k < a->lex.tok.str_len; k++)
                    sec_emit8(s, (uint8_t)a->lex.tok.str[k]);
                sec_emit8(s, 0);
            } else {
                s->len += a->lex.tok.str_len + 1;
            }
            lex_next(&a->lex);
        }
        return;
    }

    if (strcmp(dir, ".space") == 0) {
        lex_next(&a->lex);
        if (a->lex.tok.type == T_INT) {
            if (a->pass == 2)
                sec_space(s, (int)a->lex.tok.ival);
            else
                s->len += (int)a->lex.tok.ival;
            lex_next(&a->lex);
        }
        return;
    }

    /* .type sym,@function : mark the symbol STT_FUNC so the ELF writer types it,
       which a debugger's address-to-function map and a backtrace read from
       .symtab.  The '@' is skipped by the lexer, so scan the operands for the
       "function" word; any other type is left as the default. */
    if (strcmp(dir, ".type") == 0) {
        lex_next(&a->lex);
        if (a->lex.tok.type == T_IDENT || a->lex.tok.type == T_DOT_IDENT) {
            int idx = sym_lookup(&a->st, a->lex.tok.str);
            if (idx < 0) idx = sym_add(&a->st, a->lex.tok.str);
            for (lex_next(&a->lex); a->lex.tok.type != T_NEWLINE
                                    && a->lex.tok.type != T_EOF; lex_next(&a->lex)) {
                if ((a->lex.tok.type == T_IDENT || a->lex.tok.type == T_DOT_IDENT)
                    && strcmp(a->lex.tok.str, "function") == 0)
                    sym_set_func(&a->st, idx);
            }
        } else {
            skip_to_eol(&a->lex);
        }
        return;
    }

    /* .size sym,expr : the symbol's byte extent, written as st_size.  expr is a
       sum of label and integer terms (the backend emits `.Lfunc_endN - func`);
       resolved symbol values are final by the emit pass, so evaluating the terms
       each pass leaves the authoritative size after it. */
    if (strcmp(dir, ".size") == 0) {
        int target = -1;
        lex_next(&a->lex);
        if (a->lex.tok.type == T_IDENT || a->lex.tok.type == T_DOT_IDENT) {
            target = sym_lookup(&a->st, a->lex.tok.str);
            if (target < 0) target = sym_add(&a->st, a->lex.tok.str);
            lex_next(&a->lex);
        }
        if (target >= 0 && a->lex.tok.type == T_COMMA) {
            int32_t acc = 0;
            int neg = 0;
            for (lex_next(&a->lex); a->lex.tok.type != T_NEWLINE
                                    && a->lex.tok.type != T_EOF; lex_next(&a->lex)) {
                int32_t v;
                if (a->lex.tok.type == T_MINUS) { neg = 1; continue; }
                if (a->lex.tok.type == T_PLUS)  { neg = 0; continue; }
                if (a->lex.tok.type == T_INT) {
                    v = (int32_t)a->lex.tok.ival;
                } else if (a->lex.tok.type == T_IDENT
                           || a->lex.tok.type == T_DOT_IDENT) {
                    int si = sym_lookup(&a->st, a->lex.tok.str);
                    v = si >= 0 ? (int32_t)a->st.syms[si].value : 0;
                } else {
                    break;
                }
                acc += neg ? -v : v;
                neg = 0;
            }
            if (acc < 0) acc = 0;
            sym_set_size(&a->st, target, (uint32_t)acc);
        }
        skip_to_eol(&a->lex);
        return;
    }

    /* .file ["]name["] or .file N "name" : the DWARF source name, last wins. */
    if (strcmp(dir, ".file") == 0) {
        lex_next(&a->lex);
        if (a->lex.tok.type == T_INT)       /* the file number, if present */
            lex_next(&a->lex);
        if (a->lex.tok.type == T_STRING)
            a->debug_file = arena_strndup(&a->arena, a->lex.tok.str,
                                          a->lex.tok.str_len);
        skip_to_eol(&a->lex);
        return;
    }

    /* .loc N line [column] : one line-table row.  The address is the current
       .text offset, known only in the emit pass, so rows are collected then. */
    if (strcmp(dir, ".loc") == 0) {
        int line = 0, seen_line = 0;
        lex_next(&a->lex);
        if (a->lex.tok.type == T_INT) {     /* file number */
            lex_next(&a->lex);
            if (a->lex.tok.type == T_INT) {
                line = (int)a->lex.tok.ival;
                seen_line = 1;
            }
        }
        if (a->pass == 2 && seen_line && a->cur_section == SEC_TEXT)
            cf_add_locrow(a, (uint32_t)a->sections[SEC_TEXT].len, line);
        skip_to_eol(&a->lex);
        return;
    }

    /* metadata directives we accept and ignore */
    if (strcmp(dir, ".ident") == 0 || strncmp(dir, ".cfi_", 5) == 0
        || strcmp(dir, ".local") == 0) {
        skip_to_eol(&a->lex);
        return;
    }

    warn("line %d: unknown directive '%s'", a->lex.tok.line, dir);
    skip_to_eol(&a->lex);
}

/****************************************************************
 * Local numeric label support (1:, 1b, 1f)
 ****************************************************************/

#define MAX_LOCAL_LABELS 10

struct local_label {
    uint32_t offsets[256];
    int count;
    int section[256];
};

static struct local_label local_labels[MAX_LOCAL_LABELS];

static void
local_label_reset(void)
{
    memset(local_labels, 0, sizeof(local_labels));
}

static void
local_label_define(int n, int section, uint32_t offset)
{
    struct local_label *ll;
    if (n < 0 || n >= MAX_LOCAL_LABELS) return;
    ll = &local_labels[n];
    if (ll->count < 256) {
        ll->section[ll->count] = section;
        ll->offsets[ll->count] = offset;
        ll->count++;
    }
}

static int
local_label_find_back(int n, int section, uint32_t offset, uint32_t *result)
{
    struct local_label *ll;
    int k;

    if (n < 0 || n >= MAX_LOCAL_LABELS) return 0;
    ll = &local_labels[n];
    for (k = ll->count - 1; k >= 0; k--) {
        if (ll->section[k] == section && ll->offsets[k] <= offset) {
            *result = ll->offsets[k];
            return 1;
        }
    }
    return 0;
}

static int
local_label_find_fwd(int n, int section, uint32_t offset, uint32_t *result)
{
    struct local_label *ll;
    int k;

    if (n < 0 || n >= MAX_LOCAL_LABELS) return 0;
    ll = &local_labels[n];
    for (k = 0; k < ll->count; k++) {
        if (ll->section[k] == section && ll->offsets[k] > offset) {
            *result = ll->offsets[k];
            return 1;
        }
    }
    return 0;
}

/****************************************************************
 * Instruction line parsing + emission
 ****************************************************************/

static void
parse_instruction(struct assembler *a, const char *premnemonic)
{
    char mnemonic[32];
    int size;
    struct operand op1, op2;
    int nops = 0;
    int result;
    int is_movem;

    memset(&op1, 0, sizeof(op1));
    memset(&op2, 0, sizeof(op2));

    if (premnemonic) {
        if (strchr(premnemonic, '.') != NULL) {
            split_mnemonic(premnemonic, mnemonic, &size);
        } else {
            strncpy(mnemonic, premnemonic, sizeof(mnemonic) - 1);
            mnemonic[sizeof(mnemonic) - 1] = '\0';
            size = 0;
            if (a->lex.tok.type == T_DOT_IDENT &&
                is_size_suffix(a->lex.tok.str)) {
                char combined[64];
                snprintf(combined, sizeof(combined), "%s%s", mnemonic,
                         a->lex.tok.str);
                split_mnemonic(combined, mnemonic, &size);
                lex_next(&a->lex);
            }
        }
    } else if (a->lex.tok.type == T_IDENT
               && strchr(a->lex.tok.str, '.') != NULL) {
        split_mnemonic(a->lex.tok.str, mnemonic, &size);
        lex_next(&a->lex);
    } else if (a->lex.tok.type == T_IDENT) {
        strncpy(mnemonic, a->lex.tok.str, sizeof(mnemonic) - 1);
        mnemonic[sizeof(mnemonic) - 1] = '\0';
        size = 0;
        lex_next(&a->lex);
        if (a->lex.tok.type == T_DOT_IDENT &&
            is_size_suffix(a->lex.tok.str)) {
            char combined[64];
            snprintf(combined, sizeof(combined), "%s%s", mnemonic,
                     a->lex.tok.str);
            split_mnemonic(combined, mnemonic, &size);
            lex_next(&a->lex);
        }
    } else {
        skip_to_eol(&a->lex);
        return;
    }

    is_movem = (strcmp(mnemonic, "movem") == 0);

    if (a->lex.tok.type != T_NEWLINE && a->lex.tok.type != T_EOF) {
        if (!parse_operand(&a->lex, &op1, is_movem)) {
            die("line %d: bad operand for '%s'",
                a->lex.tok.line, mnemonic);
        }
        nops = 1;

        if (a->lex.tok.type == T_COMMA) {
            lex_next(&a->lex);
            if (!parse_operand(&a->lex, &op2, is_movem)) {
                die("line %d: bad second operand for '%s'",
                    a->lex.tok.line, mnemonic);
            }
            nops = 2;
        }
    }

    if (op1.type == OP_ABS && op1.sym) {
        int slen = (int)strlen(op1.sym);
        if (slen >= 2 && (op1.sym[slen - 1] == 'b' || op1.sym[slen - 1] == 'f')
            && op1.sym[0] >= '0' && op1.sym[0] <= '9' && slen <= 3) {
            int n = op1.sym[0] - '0';
            char dir = op1.sym[slen - 1];
            struct section *s = &a->sections[a->cur_section];
            uint32_t target;

            if (dir == 'b') {
                if (local_label_find_back(n, a->cur_section, s->len, &target)) {
                    op1.type = OP_ABS;
                    op1.imm = (long)target - (long)s->len - 2;
                    op1.sym = NULL;
                }
            } else {
                if (local_label_find_fwd(n, a->cur_section, s->len, &target)) {
                    op1.type = OP_ABS;
                    op1.imm = (long)target - (long)s->len - 2;
                    op1.sym = NULL;
                }
            }
        }
    }

    if (a->pass == 1) {
        struct section *s = &a->sections[a->cur_section];
        result = encode_size(mnemonic, size,
                             nops >= 1 ? &op1 : NULL,
                             nops >= 2 ? &op2 : NULL, a->short_branches);
        if (result < 0) {
            die("line %d: cannot encode '%s'", a->lex.tok.line, mnemonic);
        }
        s->len += result;
    } else {
        result = encode_insn(a, mnemonic, size,
                             nops >= 1 ? &op1 : NULL,
                             nops >= 2 ? &op2 : NULL);
        if (result < 0) {
            die("line %d: cannot encode '%s'", a->lex.tok.line, mnemonic);
        }
    }

    skip_to_eol(&a->lex);
}

/****************************************************************
 * Main pass logic
 ****************************************************************/

static void
run_pass(struct assembler *a)
{
    a->cur_section = SEC_TEXT;

    lex_next(&a->lex);

    while (a->lex.tok.type != T_EOF) {
        if (a->lex.tok.type == T_NEWLINE) {
            lex_next(&a->lex);
            continue;
        }

        if (a->lex.tok.type == T_DOT_IDENT) {
            const char *dir = a->lex.tok.str;

            if (dir[1] == 'L' || (dir[1] >= '0' && dir[1] <= '9')) {
                const char *label = dir;
                lex_next(&a->lex);
                if (a->lex.tok.type == T_COLON) {
                    lex_next(&a->lex);
                    if (a->pass == 1) {
                        int idx = sym_lookup(&a->st, label);
                        if (idx < 0)
                            idx = sym_add(&a->st, label);
                        sym_define(&a->st, idx, a->cur_section,
                                   a->sections[a->cur_section].len);
                    }
                    continue;
                }
                handle_directive(a, dir);
                skip_to_eol(&a->lex);
                if (a->lex.tok.type == T_NEWLINE)
                    lex_next(&a->lex);
                continue;
            }

            handle_directive(a, dir);
            skip_to_eol(&a->lex);
            if (a->lex.tok.type == T_NEWLINE)
                lex_next(&a->lex);
            continue;
        }

        if (a->lex.tok.type == T_IDENT) {
            const char *name = a->lex.tok.str;
            lex_next(&a->lex);

            if (a->lex.tok.type == T_COLON) {
                lex_next(&a->lex);
                if (a->pass == 1) {
                    int idx = sym_lookup(&a->st, name);
                    if (idx < 0)
                        idx = sym_add(&a->st, name);
                    sym_define(&a->st, idx, a->cur_section,
                               a->sections[a->cur_section].len);
                }
                continue;
            }

            parse_instruction(a, name);
            if (a->lex.tok.type == T_NEWLINE)
                lex_next(&a->lex);
            continue;
        }

        if (a->lex.tok.type == T_INT) {
            long n = a->lex.tok.ival;
            lex_next(&a->lex);
            if (a->lex.tok.type == T_COLON) {
                lex_next(&a->lex);
                if (a->pass == 1 && n >= 0 && n < MAX_LOCAL_LABELS) {
                    local_label_define((int)n, a->cur_section,
                                      a->sections[a->cur_section].len);
                }
                continue;
            }
            skip_to_eol(&a->lex);
            if (a->lex.tok.type == T_NEWLINE)
                lex_next(&a->lex);
            continue;
        }

        skip_to_eol(&a->lex);
        if (a->lex.tok.type == T_NEWLINE)
            lex_next(&a->lex);
    }
}

/****************************************************************
 * Public interface
 ****************************************************************/

void
as_init(struct assembler *a, const char *src)
{
    memset(a, 0, sizeof(*a));
    arena_init(&a->arena);
    a->lex.arena = &a->arena;
    a->st.arena = &a->arena;
    lex_init(&a->lex, src, '|', 0);
}

void
as_free(struct assembler *a)
{
    int k;

    for (k = 0; k < SEC_COUNT; k++) {
        free(a->sections[k].data);
        free(a->sections[k].relocs);
    }
    free(a->locrows);
    free(a->st.syms);
    free(a->lex.str_buf);
    arena_free(&a->arena);
}

void
as_pass1(struct assembler *a)
{
    local_label_reset();
    a->pass = 1;
    lex_rewind(&a->lex);
    run_pass(a);
}

void
as_pass2(struct assembler *a)
{
    int k;

    for (k = 0; k < SEC_COUNT; k++)
        a->sections[k].len = 0;

    a->pass = 2;
    lex_rewind(&a->lex);
    run_pass(a);
}

/* Build the DWARF debug sections from the .loc rows collected in the emit pass,
   the big-endian ColdFire twin of as/rv_parse.c:rv_emit_debug.  The three
   sections are non-alloc, so they do not perturb the text/data layout; the only
   link-time unknowns are the absolute address fields, each an R_68K_32 against a
   synthesized .text base symbol (value 0, which the ELF writer folds into the
   .text section symbol plus the addend). */
void
cf_emit_debug(struct assembler *a)
{
    struct arena *ar = &a->arena;
    struct section *sl = &a->sections[SEC_DEBUG_LINE];
    struct section *si = &a->sections[SEC_DEBUG_INFO];
    struct section *sa = &a->sections[SEC_DEBUG_ABBREV];
    const char *name = a->debug_file ? a->debug_file : "?";
    struct dw_line_row *rows;
    uint8_t *bytes;
    size_t len, set_addr_off, low_off, high_off, b;
    uint32_t text_end;
    int sym, i;

    if (a->n_locrows == 0)
        return;                 /* no .loc seen (no -g): emit nothing */

    sym = sym_lookup(&a->st, ".Ldebug_text0");
    if (sym < 0)
        sym = sym_add(&a->st, ".Ldebug_text0");
    sym_define(&a->st, sym, SEC_TEXT, 0);

    rows = arena_alloc(ar, (size_t)a->n_locrows * sizeof *rows);
    for (i = 0; i < a->n_locrows; i++) {
        rows[i].addr = a->locrows[i].offset;
        rows[i].line = a->locrows[i].line;
    }
    text_end = (uint32_t)a->sections[SEC_TEXT].len;

    /* .debug_line: the line program, set_address fixed to .text + first row. */
    len = dw_line_build(ar, name, rows, a->n_locrows, text_end, &bytes,
                        &set_addr_off, 1);
    for (b = 0; b < len; b++)
        sec_emit8(sl, bytes[b]);
    sec_add_reloc(sl, (uint32_t)set_addr_off, sym, (int32_t)rows[0].addr);

    /* .debug_abbrev: fixed, no relocations. */
    len = dw_abbrev_build(ar, &bytes);
    for (b = 0; b < len; b++)
        sec_emit8(sa, bytes[b]);

    /* .debug_info: a CU spanning all of .text, stmt_list at line program 0. */
    len = dw_info_build(ar, "skjegg", name, ".", 0, text_end, 0, &bytes,
                        &low_off, &high_off, 1);
    for (b = 0; b < len; b++)
        sec_emit8(si, bytes[b]);
    sec_add_reloc(si, (uint32_t)low_off, sym, 0);
    sec_add_reloc(si, (uint32_t)high_off, sym, (int32_t)text_end);
}
