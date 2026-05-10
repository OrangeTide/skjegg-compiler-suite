/* lower.c : AST to IR lowering
 * Made by a machine.  PUBLIC DOMAIN (CC0-1.0)
 */

#include "tinc.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Convenience wrappers (thread cur_fn to the IR builder API)
 ****************************************************************/

static struct arena *lower_arena;
static struct ir_func *cur_fn;
static struct ir_program *cur_prog;
static int cur_ret_mode;
static int cur_ret_count;
static int ret_buf_slot;
static int ret_cnt_slot;
static int last_retbuf_slot;

static struct ir_insn *
emit(int op)
{
    return ir_emit(cur_fn, op);
}

static int
new_temp(void)
{
    return ir_new_temp(cur_fn);
}

static int
new_label(void)
{
    return ir_new_label(cur_fn);
}

/****************************************************************
 * Global symbol table
 ****************************************************************/

struct gsym {
    char *name;
    struct ir_global *g;
    int is_func;
    int elem;
    int is_array;
    int arr_size;
    int ret_mode;
    int ret_count;
};

static struct gsym *gsyms;
static int ngsyms;
static int gsym_cap;

static void
add_gsym(struct gsym s)
{
    if (ngsyms == gsym_cap) {
        gsym_cap = gsym_cap ? gsym_cap * 2 : 16;
        gsyms = realloc(gsyms, gsym_cap * sizeof(*gsyms));
        if (!gsyms)
            die("oom");
    }
    gsyms[ngsyms++] = s;
}

static struct gsym *
find_gsym(const char *name)
{
    int i;

    for (i = 0; i < ngsyms; i++)
        if (strcmp(gsyms[i].name, name) == 0)
            return &gsyms[i];
    return NULL;
}

/****************************************************************
 * Per-function locals
 ****************************************************************/

struct lsym {
    char *name;
    int slot;
    int elem;
    int is_array;
    int arr_size;
    int is_view;
};

static struct lsym *lsyms;
static int nlsyms;
static int lsym_cap;

static int *slot_sizes;
static int nslots;
static int slot_cap;

static int
alloc_slot(int bytes)
{
    if (nslots == slot_cap) {
        slot_cap = slot_cap ? slot_cap * 2 : 16;
        slot_sizes = realloc(slot_sizes, slot_cap * sizeof(*slot_sizes));
        if (!slot_sizes)
            die("oom");
    }
    slot_sizes[nslots] = bytes;
    return nslots++;
}

static void
add_lsym(struct lsym s)
{
    if (nlsyms == lsym_cap) {
        lsym_cap = lsym_cap ? lsym_cap * 2 : 16;
        lsyms = realloc(lsyms, lsym_cap * sizeof(*lsyms));
        if (!lsyms)
            die("oom");
    }
    lsyms[nlsyms++] = s;
}

static struct lsym *
find_lsym(const char *name)
{
    int i;

    for (i = nlsyms - 1; i >= 0; i--)
        if (strcmp(lsyms[i].name, name) == 0)
            return &lsyms[i];
    return NULL;
}

/****************************************************************
 * Break/continue stack
 ****************************************************************/

struct loop {
    int brk;
    int cont;
};

static struct loop loops[64];
static int nloops;

/****************************************************************
 * Expression value type
 ****************************************************************/

struct val {
    int temp;
    int elem;
    int is_array;
};

static struct val lower_expr(struct node *n);
static void lower_stmt(struct node *n);
static void lower_cond(struct node *n, int ltrue, int lfalse);

static struct val
mkval(int temp, int elem, int is_array)
{
    struct val v;

    v.temp = temp;
    v.elem = elem;
    v.is_array = is_array;
    return v;
}

/****************************************************************
 * Address-of an lvalue
 ****************************************************************/

struct addr {
    int temp;
    int elem;
};

static struct addr
lower_addr(struct node *n)
{
    struct addr a;
    struct lsym *ls;
    struct gsym *gs;
    struct ir_insn *ins;
    struct val av, iv;
    int w;

    if (n->kind == N_NAME) {
        ls = find_lsym(n->name);
        if (ls) {
            if (ls->is_view) {
                ins = emit(IR_LDL);
            } else {
                ins = emit(IR_ADL);
            }
            ins->dst = new_temp();
            ins->slot = ls->slot;
            a.temp = ins->dst;
            a.elem = ls->elem;
            return a;
        }
        gs = find_gsym(n->name);
        if (!gs)
            die("lower:%d: undefined '%s'", n->line, n->name);
        ins = emit(IR_LEA);
        ins->dst = new_temp();
        ins->sym = arena_strdup(lower_arena,n->name);
        a.temp = ins->dst;
        a.elem = gs->elem;
        return a;
    }
    if (n->kind == N_INDEX) {
        av = lower_expr(n->a);
        iv = lower_expr(n->b);
        w = av.elem;
        if (w != 1) {
            int scl;
            struct ir_insn *li;
            scl = new_temp();
            li = emit(IR_LIC);
            li->dst = scl;
            li->imm = w;
            ins = emit(IR_MUL);
            ins->dst = new_temp();
            ins->a = iv.temp;
            ins->b = scl;
            iv.temp = ins->dst;
        }
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = av.temp;
        ins->b = iv.temp;
        a.temp = ins->dst;
        a.elem = av.elem;
        return a;
    }
    die("lower:%d: not an lvalue", n->line);
    a.temp = -1;
    a.elem = ELEM_INT;
    return a;
}

/****************************************************************
 * Expression lowering
 ****************************************************************/

static int
bin_op_for_tok(int op)
{
    switch (op) {
    case T_PLUS:    return IR_ADD;
    case T_MINUS:   return IR_SUB;
    case T_STAR:    return IR_MUL;
    case T_SLASH:   return IR_DIVS;
    case T_PERCENT: return IR_MODS;
    case T_AMP:     return IR_AND;
    case T_PIPE:    return IR_OR;
    case T_CARET:   return IR_XOR;
    case T_SHL:     return IR_SHL;
    case T_SHR:     return IR_SHRS;
    case T_EQ:      return IR_CMPEQ;
    case T_NE:      return IR_CMPNE;
    case T_LT:      return IR_CMPLTS;
    case T_LE:      return IR_CMPLES;
    case T_GT:      return IR_CMPGTS;
    case T_GE:      return IR_CMPGES;
    default:        return -1;
    }
}

static int
lower_const(long v)
{
    struct ir_insn *ins;

    ins = emit(IR_LIC);
    ins->dst = new_temp();
    ins->imm = v;
    return ins->dst;
}

static void
emit_label(int lab)
{
    struct ir_insn *ins;

    ins = emit(IR_LABEL);
    ins->label = lab;
}

static void
emit_jmp(int lab)
{
    struct ir_insn *ins;

    ins = emit(IR_JMP);
    ins->label = lab;
}

static void
emit_bnz(int t, int lab)
{
    struct ir_insn *ins;

    ins = emit(IR_BNZ);
    ins->a = t;
    ins->label = lab;
}

static struct val
lower_shortcircuit(struct node *n)
{
    int t, ltrue, lfalse, lend;
    struct ir_insn *ins;

    ltrue = new_label();
    lfalse = new_label();
    lend = new_label();

    lower_cond(n, ltrue, lfalse);

    emit_label(ltrue);
    t = new_temp();
    ins = emit(IR_LIC);
    ins->dst = t;
    ins->imm = 1;
    emit_jmp(lend);

    emit_label(lfalse);
    ins = emit(IR_LIC);
    ins->dst = t;
    ins->imm = 0;

    emit_label(lend);
    return mkval(t, ELEM_INT, 0);
}

static struct val
lower_call(struct node *n)
{
    struct ir_insn *ins;
    struct node *a;
    int args[16];
    int nargs = 0;
    int t;

    if (n->a->kind == N_NAME &&
        strcmp(n->a->name, "mark") == 0) {
        int slot = alloc_slot(12);
        ins = emit(IR_MARK);
        ins->dst = new_temp();
        ins->slot = slot;
        ins->label = ir_new_label(cur_fn);
        return mkval(ins->dst, ELEM_INT, 0);
    }
    if (n->a->kind == N_NAME &&
        strcmp(n->a->name, "capture") == 0) {
        ins = emit(IR_CAPTURE);
        ins->dst = new_temp();
        return mkval(ins->dst, ELEM_INT, 0);
    }
    if (n->a->kind == N_NAME &&
        strcmp(n->a->name, "resume") == 0) {
        struct val buf, val;
        a = n->b;
        if (!a || !a->next)
            die("resume requires 2 args");
        buf = lower_expr(a);
        val = lower_expr(a->next);
        ins = emit(IR_RESUME);
        ins->a = buf.temp;
        ins->b = val.temp;
        return mkval(0, ELEM_INT, 0);
    }
    if (n->a->kind == N_NAME &&
        strcmp(n->a->name, "alloca") == 0) {
        struct val sz;
        a = n->b;
        if (!a)
            die("alloca requires 1 arg");
        sz = lower_expr(a);
        ins = emit(IR_ALLOCA);
        ins->dst = new_temp();
        ins->a = sz.temp;
        return mkval(ins->dst, ELEM_INT, 0);
    }
    if (n->a->kind == N_NAME &&
        strcmp(n->a->name, "length") == 0) {
        struct lsym *ls;
        struct gsym *gs;
        a = n->b;
        if (!a || a->kind != N_NAME)
            die("length requires a named array argument");
        ls = find_lsym(a->name);
        if (ls && ls->is_array && ls->arr_size > 0)
            return mkval(lower_const(ls->arr_size), ELEM_INT, 0);
        gs = find_gsym(a->name);
        if (gs && gs->is_array && gs->arr_size > 0)
            return mkval(lower_const(gs->arr_size), ELEM_INT, 0);
        die("lower: length() on unsized array '%s'", a->name);
    }

    {
        struct gsym *callee_gs = NULL;
        int need_retbuf = 0;
        int retbuf_local = -1;

        if (n->a->kind == N_NAME)
            callee_gs = find_gsym(n->a->name);
        if (callee_gs &&
            (callee_gs->ret_mode == RET_FIXED ||
             callee_gs->ret_mode == RET_VAR ||
             callee_gs->ret_mode == RET_VAR_MIN)) {
            int buf_size;
            need_retbuf = 1;
            if (callee_gs->ret_mode == RET_FIXED)
                buf_size = callee_gs->ret_count * 4;
            else
                buf_size = 256;
            retbuf_local = alloc_slot(buf_size);
            last_retbuf_slot = retbuf_local;
        }

        for (a = n->b; a; a = a->next) {
            struct val av = lower_expr(a);
            if (nargs >= 16)
                die("lower: too many args");
            args[nargs++] = av.temp;
        }

        if (need_retbuf) {
            int retbuf_t, i;
            ins = emit(IR_ADL);
            ins->dst = new_temp();
            ins->slot = retbuf_local;
            retbuf_t = ins->dst;
            ins = emit(IR_ARG);
            ins->a = retbuf_t;
            ins->imm = 0;
            for (i = 0; i < nargs; i++) {
                ins = emit(IR_ARG);
                ins->a = args[i];
                ins->imm = i + 1;
            }
            nargs++;
        } else {
            int i;
            for (i = 0; i < nargs; i++) {
                ins = emit(IR_ARG);
                ins->a = args[i];
                ins->imm = i;
            }
        }

        if (n->a->kind == N_NAME) {
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena,n->a->name);
            ins->nargs = nargs;
            t = ins->dst;
        } else {
            struct val fv = lower_expr(n->a);
            ins = emit(IR_CALLI);
            ins->dst = new_temp();
            ins->a = fv.temp;
            ins->nargs = nargs;
            t = ins->dst;
        }
    }
    return mkval(t, ELEM_INT, 0);
}

static struct val
lower_expr(struct node *n)
{
    struct ir_insn *ins;
    struct val l, r, v;
    struct addr ad;
    struct lsym *ls;
    struct gsym *gs;
    int op;

    if (!n)
        die("lower: null expression");

    switch (n->kind) {
    case N_NUM:
    case N_CHARLIT:
        return mkval(lower_const(n->ival), ELEM_INT, 0);

    case N_STR: {
        static int strctr;
        struct ir_global *g;
        char namebuf[32];

        snprintf(namebuf, sizeof(namebuf), "__str_%d", strctr++);
        g = arena_zalloc(lower_arena, sizeof(*g));
        g->name = arena_strdup(lower_arena,namebuf);
        g->base_type = IR_I8;
        g->arr_size = n->slen + 1;
        g->is_local = 1;
        g->init_string = arena_alloc(lower_arena,n->slen + 1);
        memcpy(g->init_string, n->sval, n->slen);
        g->init_string[n->slen] = '\0';
        g->init_strlen = n->slen + 1;
        g->next = cur_prog->globals;
        cur_prog->globals = g;
        {
            struct gsym s;
            s.name = g->name;
            s.g = g;
            s.is_func = 0;
            s.elem = ELEM_BYTE;
            s.is_array = 1;
            s.arr_size = n->slen + 1;
            s.ret_mode = 0;
            s.ret_count = 0;
            add_gsym(s);
        }
        ins = emit(IR_LEA);
        ins->dst = new_temp();
        ins->sym = arena_strdup(lower_arena,namebuf);
        v = mkval(ins->dst, ELEM_BYTE, 1);
        return v;
    }

    case N_NAME:
        ls = find_lsym(n->name);
        if (ls) {
            if (ls->is_array) {
                if (ls->is_view) {
                    ins = emit(IR_LDL);
                } else {
                    ins = emit(IR_ADL);
                }
                ins->dst = new_temp();
                ins->slot = ls->slot;
                v = mkval(ins->dst, ls->elem, 1);
                return v;
            }
            ins = emit(IR_LDL);
            ins->dst = new_temp();
            ins->slot = ls->slot;
            return mkval(ins->dst, ls->elem, 0);
        }
        gs = find_gsym(n->name);
        if (!gs)
            die("lower:%d: undefined '%s'", n->line, n->name);
        if (gs->is_func) {
            ins = emit(IR_LEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena,n->name);
            return mkval(ins->dst, ELEM_INT, 0);
        }
        if (gs->is_array) {
            ins = emit(IR_LEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena,n->name);
            v = mkval(ins->dst, gs->elem, 1);
            return v;
        }
        {
            int addr;
            ins = emit(IR_LEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena,n->name);
            addr = ins->dst;
            ins = emit(IR_LW);
            ins->dst = new_temp();
            ins->a = addr;
            return mkval(ins->dst, gs->elem, 0);
        }

    case N_INDEX:
        ad = lower_addr(n);
        ins = emit(ad.elem == ELEM_BYTE ? IR_LB : IR_LW);
        ins->dst = new_temp();
        ins->a = ad.temp;
        return mkval(ins->dst, ad.elem, 0);

    case N_UNOP:
        if (n->op == T_MINUS) {
            l = lower_expr(n->a);
            ins = emit(IR_NEG);
            ins->dst = new_temp();
            ins->a = l.temp;
            return mkval(ins->dst, ELEM_INT, 0);
        }
        if (n->op == T_TILDE) {
            l = lower_expr(n->a);
            ins = emit(IR_NOT);
            ins->dst = new_temp();
            ins->a = l.temp;
            return mkval(ins->dst, ELEM_INT, 0);
        }
        if (n->op == T_BANG) {
            l = lower_expr(n->a);
            {
                int zero = lower_const(0);
                ins = emit(IR_CMPEQ);
                ins->dst = new_temp();
                ins->a = l.temp;
                ins->b = zero;
            }
            return mkval(ins->dst, ELEM_INT, 0);
        }
        die("lower:%d: bad unop", n->line);
        return mkval(0, ELEM_INT, 0);

    case N_BINOP:
        if (n->op == T_ANDAND || n->op == T_OROR)
            return lower_shortcircuit(n);
        l = lower_expr(n->a);
        r = lower_expr(n->b);
        op = bin_op_for_tok(n->op);
        if (op < 0)
            die("lower:%d: bad binop", n->line);
        ins = emit(op);
        ins->dst = new_temp();
        ins->a = l.temp;
        ins->b = r.temp;
        v = mkval(ins->dst, ELEM_INT, 0);
        if ((n->op == T_PLUS || n->op == T_MINUS) && l.is_array) {
            v.is_array = 1;
            v.elem = l.elem;
        }
        return v;

    case N_ASSIGN:
        if (n->a->kind == N_DISCARD) {
            return lower_expr(n->b);
        }
        r = lower_expr(n->b);
        if (n->a->kind == N_NAME) {
            ls = find_lsym(n->a->name);
            if (ls && !ls->is_array) {
                ins = emit(IR_STL);
                ins->a = r.temp;
                ins->slot = ls->slot;
                return r;
            }
        }
        ad = lower_addr(n->a);
        ins = emit(ad.elem == ELEM_BYTE ? IR_SB : IR_SW);
        ins->a = ad.temp;
        ins->b = r.temp;
        return r;

    case N_COMPOUND_ASSIGN:
        if (n->a->kind == N_NAME) {
            ls = find_lsym(n->a->name);
            if (ls && !ls->is_array) {
                ins = emit(IR_LDL);
                ins->dst = new_temp();
                ins->slot = ls->slot;
                l = mkval(ins->dst, ls->elem, 0);
                r = lower_expr(n->b);
                op = bin_op_for_tok(n->op);
                ins = emit(op);
                ins->dst = new_temp();
                ins->a = l.temp;
                ins->b = r.temp;
                {
                    int result = ins->dst;
                    ins = emit(IR_STL);
                    ins->a = result;
                    ins->slot = ls->slot;
                    return mkval(result, ELEM_INT, 0);
                }
            }
        }
        {
            int addr_t, lval_t, result_t, is_byte;
            ad = lower_addr(n->a);
            addr_t = ad.temp;
            is_byte = (ad.elem == ELEM_BYTE);
            ins = emit(is_byte ? IR_LB : IR_LW);
            ins->dst = new_temp();
            ins->a = addr_t;
            lval_t = ins->dst;
            r = lower_expr(n->b);
            op = bin_op_for_tok(n->op);
            ins = emit(op);
            ins->dst = new_temp();
            ins->a = lval_t;
            ins->b = r.temp;
            result_t = ins->dst;
            ad = lower_addr(n->a);
            ins = emit(is_byte ? IR_SB : IR_SW);
            ins->a = ad.temp;
            ins->b = result_t;
            return mkval(result_t, ELEM_INT, 0);
        }

    case N_CALL:
        return lower_call(n);

    case N_SLICE:
        die("lower:%d: array slices not yet implemented", n->line);
        return mkval(0, ELEM_INT, 0);

    default:
        die("lower: unhandled expr kind %d", n->kind);
    }
    return mkval(0, ELEM_INT, 0);
}

/****************************************************************
 * Conditional lowering (for && / || / if / while)
 ****************************************************************/

static void
lower_cond(struct node *n, int ltrue, int lfalse)
{
    struct val v;

    if (n->kind == N_BINOP && n->op == T_ANDAND) {
        int lmid = new_label();
        lower_cond(n->a, lmid, lfalse);
        emit_label(lmid);
        lower_cond(n->b, ltrue, lfalse);
        return;
    }
    if (n->kind == N_BINOP && n->op == T_OROR) {
        int lmid = new_label();
        lower_cond(n->a, ltrue, lmid);
        emit_label(lmid);
        lower_cond(n->b, ltrue, lfalse);
        return;
    }
    if (n->kind == N_UNOP && n->op == T_BANG) {
        lower_cond(n->a, lfalse, ltrue);
        return;
    }
    v = lower_expr(n);
    emit_bnz(v.temp, ltrue);
    emit_jmp(lfalse);
}

/****************************************************************
 * Local variable declaration (inline in statement list)
 ****************************************************************/

static void
lower_local_decl(struct node *n)
{
    struct ir_insn *ins;
    struct lsym ls;
    int bytes;

    ls.name = n->name;
    ls.elem = n->elem;
    ls.is_array = (n->arr_size > 0);
    ls.arr_size = n->arr_size;
    ls.is_view = 0;

    if (ls.is_array) {
        bytes = n->arr_size * n->elem;
        if (bytes < 1)
            bytes = 1;
    } else {
        bytes = 4;
    }
    ls.slot = alloc_slot(bytes);
    add_lsym(ls);

    if (!n->a)
        return;

    if (!ls.is_array) {
        struct val v = lower_expr(n->a);
        ins = emit(IR_STL);
        ins->a = v.temp;
        ins->slot = ls.slot;
        return;
    }

    if (n->a->kind == N_STR) {
        static int slctr;
        struct ir_global *g;
        char namebuf[32];

        snprintf(namebuf, sizeof(namebuf), "__lstr_%d", slctr++);
        g = arena_zalloc(lower_arena, sizeof(*g));
        g->name = arena_strdup(lower_arena,namebuf);
        g->base_type = IR_I8;
        g->arr_size = n->a->slen + 1;
        g->is_local = 1;
        g->init_string = arena_alloc(lower_arena,n->a->slen + 1);
        memcpy(g->init_string, n->a->sval, n->a->slen);
        g->init_string[n->a->slen] = '\0';
        g->init_strlen = n->a->slen + 1;
        g->next = cur_prog->globals;
        cur_prog->globals = g;
        {
            struct gsym s;
            s.name = g->name;
            s.g = g;
            s.is_func = 0;
            s.elem = ELEM_BYTE;
            s.is_array = 1;
            s.arr_size = n->a->slen + 1;
            s.ret_mode = 0;
            s.ret_count = 0;
            add_gsym(s);
        }
        ls.is_view = 1;
        lsyms[nlsyms - 1].is_view = 1;
        ins = emit(IR_LEA);
        ins->dst = new_temp();
        ins->sym = arena_strdup(lower_arena,namebuf);
        {
            int addr_t = ins->dst;
            ins = emit(IR_STL);
            ins->a = addr_t;
            ins->slot = ls.slot;
        }
        return;
    }

    if (n->a->kind == N_INIT_LIST) {
        struct node *e;
        int base_t, i;

        ins = emit(IR_ADL);
        ins->dst = new_temp();
        ins->slot = ls.slot;
        base_t = ins->dst;
        i = 0;
        for (e = n->a->a; e; e = e->next, i++) {
            struct val v = lower_expr(e);
            int off = i * n->elem;
            if (off > 0) {
                int off_t = lower_const(off);
                ins = emit(IR_ADD);
                ins->dst = new_temp();
                ins->a = base_t;
                ins->b = off_t;
                ins = emit(n->elem == ELEM_BYTE ? IR_SB : IR_SW);
                ins->a = ins->dst;
                ins->b = v.temp;
            } else {
                ins = emit(n->elem == ELEM_BYTE ? IR_SB : IR_SW);
                ins->a = base_t;
                ins->b = v.temp;
            }
        }
        return;
    }

    die("lower:%d: unsupported local array initializer", n->line);
}

/****************************************************************
 * Statement lowering
 ****************************************************************/

static void
lower_foreach(struct node *n)
{
    struct ir_insn *ins;
    struct lsym *arr_ls;
    struct gsym *arr_gs;
    int arr_len, arr_elem, arr_base_op, arr_slot_or_sym;
    int counter_slot, len_t, ltop, lbody, lbrk;
    int cnt_t, cmp_t, idx_t, addr_t, elem_t, one_t, next_t;
    struct lsym xls;
    char *arr_sym = NULL;

    if (n->a->kind != N_NAME)
        die("lower:%d: foreach requires a named array", n->line);

    arr_ls = find_lsym(n->a->name);
    if (arr_ls && arr_ls->is_array) {
        arr_len = arr_ls->arr_size;
        arr_elem = arr_ls->elem;
        arr_base_op = arr_ls->is_view ? IR_LDL : IR_ADL;
        arr_slot_or_sym = arr_ls->slot;
    } else {
        arr_gs = find_gsym(n->a->name);
        if (!arr_gs || !arr_gs->is_array)
            die("lower:%d: '%s' is not an array", n->line, n->a->name);
        arr_len = arr_gs->arr_size;
        arr_elem = arr_gs->elem;
        arr_base_op = -1;
        arr_sym = n->a->name;
    }

    if (arr_len <= 0)
        die("lower:%d: foreach requires a sized array", n->line);

    xls.name = n->name;
    xls.elem = ELEM_INT;
    xls.is_array = 0;
    xls.arr_size = 0;
    xls.is_view = 0;
    xls.slot = alloc_slot(4);
    add_lsym(xls);

    counter_slot = alloc_slot(4);
    {
        int zero = lower_const(0);
        ins = emit(IR_STL);
        ins->a = zero;
        ins->slot = counter_slot;
    }

    len_t = lower_const(arr_len);

    ltop = new_label();
    lbody = new_label();
    lbrk = new_label();

    emit_label(ltop);

    ins = emit(IR_LDL);
    ins->dst = new_temp();
    ins->slot = counter_slot;
    cnt_t = ins->dst;

    ins = emit(IR_CMPLTS);
    ins->dst = new_temp();
    ins->a = cnt_t;
    ins->b = len_t;
    cmp_t = ins->dst;
    emit_bnz(cmp_t, lbody);
    emit_jmp(lbrk);

    emit_label(lbody);

    ins = emit(IR_LDL);
    ins->dst = new_temp();
    ins->slot = counter_slot;
    idx_t = ins->dst;

    if (arr_elem != 1) {
        int scl = lower_const(arr_elem);
        ins = emit(IR_MUL);
        ins->dst = new_temp();
        ins->a = idx_t;
        ins->b = scl;
        idx_t = ins->dst;
    }

    if (arr_base_op >= 0) {
        ins = emit(arr_base_op);
        ins->dst = new_temp();
        ins->slot = arr_slot_or_sym;
    } else {
        ins = emit(IR_LEA);
        ins->dst = new_temp();
        ins->sym = arena_strdup(lower_arena,arr_sym);
    }
    {
        int base_t = ins->dst;
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = base_t;
        ins->b = idx_t;
        addr_t = ins->dst;
    }

    ins = emit(arr_elem == ELEM_BYTE ? IR_LB : IR_LW);
    ins->dst = new_temp();
    ins->a = addr_t;
    elem_t = ins->dst;

    ins = emit(IR_STL);
    ins->a = elem_t;
    ins->slot = xls.slot;

    if (nloops >= 64)
        die("lower: nested loops too deep");
    loops[nloops].brk = lbrk;
    loops[nloops].cont = ltop;
    nloops++;

    lower_stmt(n->b);

    nloops--;

    ins = emit(IR_LDL);
    ins->dst = new_temp();
    ins->slot = counter_slot;
    cnt_t = ins->dst;

    one_t = lower_const(1);
    ins = emit(IR_ADD);
    ins->dst = new_temp();
    ins->a = cnt_t;
    ins->b = one_t;
    next_t = ins->dst;

    ins = emit(IR_STL);
    ins->a = next_t;
    ins->slot = counter_slot;

    emit_jmp(ltop);
    emit_label(lbrk);
}

static void
lower_stmt(struct node *n)
{
    struct ir_insn *ins;
    struct node *s;
    int ltrue, lfalse, lend, ltop, lcont, lbrk;
    struct val v;

    if (!n)
        return;

    switch (n->kind) {
    case N_BLOCK: {
        int saved = nlsyms;
        for (s = n->a; s; s = s->next)
            lower_stmt(s);
        nlsyms = saved;
        return;
    }

    case N_GLOBAL:
        lower_local_decl(n);
        return;

    case N_EXPR_STMT:
        if (n->a)
            (void)lower_expr(n->a);
        return;

    case N_IF:
        ltrue = new_label();
        lfalse = new_label();
        lend = new_label();
        lower_cond(n->a, ltrue, lfalse);
        emit_label(ltrue);
        lower_stmt(n->b);
        if (n->c) {
            emit_jmp(lend);
            emit_label(lfalse);
            lower_stmt(n->c);
            emit_label(lend);
        } else {
            emit_label(lfalse);
        }
        return;

    case N_WHILE:
        ltop = new_label();
        lbrk = new_label();
        lcont = ltop;
        emit_label(ltop);
        if (n->a->kind == N_NUM && n->a->ival != 0) {
            /* while(1): no test */
        } else {
            int lbody = new_label();
            lower_cond(n->a, lbody, lbrk);
            emit_label(lbody);
        }
        if (nloops >= 64)
            die("lower: nested loops too deep");
        loops[nloops].brk = lbrk;
        loops[nloops].cont = lcont;
        nloops++;
        lower_stmt(n->b);
        nloops--;
        emit_jmp(ltop);
        emit_label(lbrk);
        return;

    case N_FOREACH:
        lower_foreach(n);
        return;

    case N_BREAK:
        if (nloops == 0)
            die("lower:%d: break outside loop", n->line);
        emit_jmp(loops[nloops - 1].brk);
        return;

    case N_CONTINUE:
        if (nloops == 0)
            die("lower:%d: continue outside loop", n->line);
        emit_jmp(loops[nloops - 1].cont);
        return;

    case N_RETURN:
        if (n->a && n->a->next) {
            struct node *rv;
            int buf_t, off;

            ins = emit(IR_LDL);
            ins->dst = new_temp();
            ins->slot = ret_buf_slot;
            buf_t = ins->dst;

            off = 0;
            for (rv = n->a; rv; rv = rv->next) {
                int addr_t;
                v = lower_expr(rv);
                if (off > 0) {
                    int off_t = lower_const(off);
                    ins = emit(IR_ADD);
                    ins->dst = new_temp();
                    ins->a = buf_t;
                    ins->b = off_t;
                    addr_t = ins->dst;
                } else {
                    addr_t = buf_t;
                }
                ins = emit(IR_SW);
                ins->a = addr_t;
                ins->b = v.temp;
                off += 4;
            }
            ins = emit(IR_RET);
        } else if (n->a) {
            v = lower_expr(n->a);
            ins = emit(IR_RETV);
            ins->a = v.temp;
        } else if (cur_ret_mode == RET_VAR ||
                   cur_ret_mode == RET_VAR_MIN) {
            int cnt_t;
            ins = emit(IR_LDL);
            ins->dst = new_temp();
            ins->slot = ret_cnt_slot;
            cnt_t = ins->dst;
            ins = emit(IR_RETV);
            ins->a = cnt_t;
        } else {
            ins = emit(IR_RET);
        }
        return;

    case N_RETURN_PUSH: {
        int buf_t, cnt_t, four_t, off_t, addr_t, one_t, next_t;

        v = lower_expr(n->a);

        ins = emit(IR_LDL);
        ins->dst = new_temp();
        ins->slot = ret_cnt_slot;
        cnt_t = ins->dst;

        four_t = lower_const(4);
        ins = emit(IR_MUL);
        ins->dst = new_temp();
        ins->a = cnt_t;
        ins->b = four_t;
        off_t = ins->dst;

        ins = emit(IR_LDL);
        ins->dst = new_temp();
        ins->slot = ret_buf_slot;
        buf_t = ins->dst;

        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = buf_t;
        ins->b = off_t;
        addr_t = ins->dst;

        ins = emit(IR_SW);
        ins->a = addr_t;
        ins->b = v.temp;

        one_t = lower_const(1);
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = cnt_t;
        ins->b = one_t;
        next_t = ins->dst;

        ins = emit(IR_STL);
        ins->a = next_t;
        ins->slot = ret_cnt_slot;
        return;
    }

    case N_DESTRUCT: {
        struct node *target;
        int buf_t, off;

        if (n->b->kind != N_CALL)
            die("lower:%d: destructuring requires a function call",
                n->line);

        (void)lower_call(n->b);

        ins = emit(IR_ADL);
        ins->dst = new_temp();
        ins->slot = last_retbuf_slot;
        buf_t = ins->dst;

        off = 0;
        for (target = n->a; target; target = target->next) {
            if (target->kind == N_DISCARD) {
                off += 4;
                continue;
            }
            if (target->kind == N_NAME) {
                struct lsym *tls;
                int addr_t, val_t;

                if (off > 0) {
                    int off_t = lower_const(off);
                    ins = emit(IR_ADD);
                    ins->dst = new_temp();
                    ins->a = buf_t;
                    ins->b = off_t;
                    addr_t = ins->dst;
                } else {
                    addr_t = buf_t;
                }
                ins = emit(IR_LW);
                ins->dst = new_temp();
                ins->a = addr_t;
                val_t = ins->dst;

                tls = find_lsym(target->name);
                if (!tls)
                    die("lower:%d: undefined '%s'",
                        target->line, target->name);
                ins = emit(IR_STL);
                ins->a = val_t;
                ins->slot = tls->slot;
                off += 4;
                continue;
            }
            if (target->kind == N_REST) {
                die("lower:%d: rest.. destructuring not yet implemented",
                    target->line);
            }
            die("lower:%d: bad destructuring target", target->line);
        }
        return;
    }

    default:
        die("lower: unhandled stmt kind %d", n->kind);
    }
}

/****************************************************************
 * Function lowering
 ****************************************************************/

static struct ir_func *
lower_function(struct node *fn_ast)
{
    struct ir_func *fn;
    struct node *p;
    struct ir_insn *ins;
    int nparams = 0;

    fn = ir_new_func(lower_arena, fn_ast->name);
    cur_fn = fn;
    cur_ret_mode = fn_ast->ret_mode;
    cur_ret_count = fn_ast->ret_count;
    ret_buf_slot = -1;
    ret_cnt_slot = -1;
    nloops = 0;
    nlsyms = 0;
    nslots = 0;

    if (cur_ret_mode == RET_FIXED ||
        cur_ret_mode == RET_VAR ||
        cur_ret_mode == RET_VAR_MIN) {
        ret_buf_slot = alloc_slot(4);
        nparams++;
    }

    for (p = fn_ast->a; p; p = p->next) {
        struct lsym ls;

        ls.name = p->name;
        ls.elem = p->elem;
        if (p->arr_size == -1) {
            ls.is_array = 1;
            ls.arr_size = 0;
            ls.is_view = 1;
        } else {
            ls.is_array = 0;
            ls.arr_size = 0;
            ls.is_view = 0;
        }
        ls.slot = alloc_slot(4);
        add_lsym(ls);
        nparams++;
    }
    fn->nparams = nparams;

    ins = emit(IR_FUNC);
    ins->sym = arena_strdup(lower_arena,fn_ast->name);
    ins->nargs = nparams;

    if (cur_ret_mode == RET_VAR || cur_ret_mode == RET_VAR_MIN) {
        int zero;
        ret_cnt_slot = alloc_slot(4);
        zero = lower_const(0);
        ins = emit(IR_STL);
        ins->a = zero;
        ins->slot = ret_cnt_slot;
    }

    lower_stmt(fn_ast->b);

    if (!fn->tail ||
        (fn->tail->op != IR_RET && fn->tail->op != IR_RETV)) {
        if (cur_ret_mode == RET_VAR ||
            cur_ret_mode == RET_VAR_MIN) {
            int cnt_t;
            ins = emit(IR_LDL);
            ins->dst = new_temp();
            ins->slot = ret_cnt_slot;
            cnt_t = ins->dst;
            ins = emit(IR_RETV);
            ins->a = cnt_t;
        } else if (cur_ret_mode == RET_FIXED) {
            ins = emit(IR_RET);
        } else {
            int z = lower_const(0);
            ins = emit(IR_RETV);
            ins->a = z;
        }
    }

    emit(IR_ENDF);

    fn->nslots = nslots;
    fn->slot_size = arena_alloc(lower_arena, nslots * sizeof(int));
    memcpy(fn->slot_size, slot_sizes, nslots * sizeof(int));
    return fn;
}

/****************************************************************
 * Global lowering
 ****************************************************************/

static int
elem_to_irtype(int elem)
{
    return elem == ELEM_BYTE ? IR_I8 : IR_I32;
}

static void
lower_global(struct node *gn)
{
    struct ir_global *g;
    struct gsym s;

    g = arena_zalloc(lower_arena, sizeof(*g));
    g->name = arena_strdup(lower_arena,gn->name);
    g->base_type = elem_to_irtype(gn->elem);
    g->arr_size = gn->arr_size;

    if (gn->a) {
        struct node *init = gn->a;

        if (init->kind == N_STR) {
            g->init_string = arena_alloc(lower_arena,init->slen + 1);
            memcpy(g->init_string, init->sval, init->slen);
            g->init_string[init->slen] = '\0';
            g->init_strlen = init->slen + 1;
        } else if (init->kind == N_INIT_LIST) {
            struct node *e;
            int n = 0, i;

            for (e = init->a; e; e = e->next)
                n++;
            g->init_ivals = arena_alloc(lower_arena,n * sizeof(*g->init_ivals));
            i = 0;
            for (e = init->a; e; e = e->next) {
                if (e->kind == N_NUM ||
                    e->kind == N_CHARLIT)
                    g->init_ivals[i++] = e->ival;
                else if (e->kind == N_UNOP &&
                         e->op == T_MINUS && e->a &&
                         (e->a->kind == N_NUM ||
                          e->a->kind == N_CHARLIT))
                    g->init_ivals[i++] = -e->a->ival;
                else
                    die("lower:%d: non-constant initializer",
                        gn->line);
            }
            g->init_count = n;
            if (g->arr_size == 0)
                g->arr_size = n;
        } else if (init->kind == N_NUM ||
                   init->kind == N_CHARLIT) {
            g->init_ivals = arena_alloc(lower_arena,sizeof(*g->init_ivals));
            g->init_ivals[0] = init->ival;
            g->init_count = 1;
        } else {
            die("lower:%d: non-constant initializer", gn->line);
        }
    }

    g->next = cur_prog->globals;
    cur_prog->globals = g;

    s.name = g->name;
    s.g = g;
    s.is_func = 0;
    s.elem = gn->elem;
    s.is_array = (g->arr_size > 0);
    s.arr_size = g->arr_size;
    s.ret_mode = 0;
    s.ret_count = 0;
    add_gsym(s);
}

static void
register_function(struct node *fn_ast)
{
    struct gsym s;

    s.name = arena_strdup(lower_arena, fn_ast->name);
    s.g = NULL;
    s.is_func = 1;
    s.elem = ELEM_INT;
    s.is_array = 0;
    s.arr_size = 0;
    s.ret_mode = fn_ast->ret_mode;
    s.ret_count = fn_ast->ret_count;
    add_gsym(s);
}

/****************************************************************
 * Entry point
 ****************************************************************/

struct ir_program *
lower_program(struct arena *a, struct node *ast)
{
    struct ir_program *p;
    struct node *d;
    struct ir_func *fn, **ftail;

    lower_arena = a;
    p = arena_zalloc(lower_arena, sizeof(*p));
    cur_prog = p;
    ngsyms = 0;

    for (d = ast ? ast->a : NULL; d; d = d->next) {
        if (d->kind == N_GLOBAL)
            lower_global(d);
        else if (d->kind == N_FUNC)
            register_function(d);
    }

    {
        struct gsym s;
        const char *names[] = {
            "write", "read", "exit", NULL,
        };
        int i;

        for (i = 0; names[i]; i++) {
            if (find_gsym(names[i]))
                continue;
            s.name = arena_strdup(lower_arena, names[i]);
            s.g = NULL;
            s.is_func = 1;
            s.elem = ELEM_INT;
            s.is_array = 0;
            s.arr_size = 0;
            s.ret_mode = 0;
            s.ret_count = 0;
            add_gsym(s);
        }
    }

    ftail = &p->funcs;
    for (d = ast ? ast->a : NULL; d; d = d->next) {
        if (d->kind != N_FUNC)
            continue;
        fn = lower_function(d);
        *ftail = fn;
        ftail = &fn->next;
    }

    free(gsyms);
    gsyms = NULL;
    gsym_cap = 0;
    free(lsyms);
    lsyms = NULL;
    lsym_cap = 0;
    free(slot_sizes);
    slot_sizes = NULL;
    slot_cap = 0;

    return p;
}
