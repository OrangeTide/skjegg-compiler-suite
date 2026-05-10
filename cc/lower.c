/* lower.c : C AST to IR lowering */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "cc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct arena *lower_arena;
static struct ir_func *cur_fn;
static struct ir_program *cur_prog;
static const char *cur_fn_name;
static int cur_fn_returns_float;
static int cur_fn_returns_i64;

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

static int
lower_const(long v)
{
    struct ir_insn *ins = emit(IR_LIC);
    ins->dst = new_temp();
    ins->imm = v;
    return ins->dst;
}

static int
lower_const64(long v)
{
    struct ir_insn *ins = emit(IR_LIC64);
    ins->dst = new_temp();
    ins->imm = v;
    return ins->dst;
}

static void
emit_label(int lab)
{
    struct ir_insn *ins = emit(IR_LABEL);
    ins->label = lab;
}

static void
emit_jmp(int lab)
{
    struct ir_insn *ins = emit(IR_JMP);
    ins->label = lab;
}

static void
emit_bnz(int t, int lab)
{
    struct ir_insn *ins = emit(IR_BNZ);
    ins->a = t;
    ins->label = lab;
}


/****************************************************************
 * Local slots
 ****************************************************************/

static int *slot_sizes;
static int nslots;
static int slot_cap;

static int
alloc_slot(int bytes)
{
    if (nslots == slot_cap) {
        slot_cap = slot_cap ? slot_cap * 2 : 32;
        slot_sizes = realloc(slot_sizes, slot_cap * sizeof *slot_sizes);
    }
    slot_sizes[nslots] = bytes;
    return nslots++;
}

struct local {
    char *name;
    int slot;
    struct cc_type *type;
    char *static_name;
};

static struct local *locals;
static int nlocals;
static int local_cap;

static void
add_local(const char *name, int slot, struct cc_type *type)
{
    if (nlocals == local_cap) {
        local_cap = local_cap ? local_cap * 2 : 32;
        locals = realloc(locals, local_cap * sizeof *locals);
    }
    locals[nlocals].name = arena_strdup(lower_arena,name);
    locals[nlocals].slot = slot;
    locals[nlocals].type = type;
    locals[nlocals].static_name = NULL;
    nlocals++;
}

static struct local *
find_local(const char *name)
{
    for (int i = nlocals - 1; i >= 0; i--)
        if (strcmp(locals[i].name, name) == 0)
            return &locals[i];
    return NULL;
}

/****************************************************************
 * Global tracking
 ****************************************************************/

struct global {
    char *name;
    struct cc_type *type;
    int is_func;
};

static struct global *globals;
static int nglobals;
static int global_cap;

static void
add_global(const char *name, struct cc_type *type, int is_func)
{
    if (nglobals == global_cap) {
        global_cap = global_cap ? global_cap * 2 : 32;
        globals = realloc(globals, global_cap * sizeof *globals);
    }
    globals[nglobals].name = arena_strdup(lower_arena,name);
    globals[nglobals].type = type;
    globals[nglobals].is_func = is_func;
    nglobals++;
}

static struct global *
find_global(const char *name)
{
    for (int i = 0; i < nglobals; i++)
        if (strcmp(globals[i].name, name) == 0)
            return &globals[i];
    return NULL;
}

/****************************************************************
 * Break/continue/switch context
 ****************************************************************/

struct loop_ctx {
    int brk;
    int cont;
};

static struct loop_ctx loop_stack[64];
static int nloops;

struct switch_ctx {
    int end_label;
    int default_label;
    int ncases;
    long case_vals[256];
    int case_labels[256];
};

static struct switch_ctx *cur_switch;

/****************************************************************
 * Goto/label table
 ****************************************************************/

struct named_label {
    char *name;
    int label;
};

static struct named_label named_labels[256];
static int nnamed_labels;

static int
get_named_label(const char *name)
{
    for (int i = 0; i < nnamed_labels; i++)
        if (strcmp(named_labels[i].name, name) == 0)
            return named_labels[i].label;
    int lab = new_label();
    named_labels[nnamed_labels].name = arena_strdup(lower_arena,name);
    named_labels[nnamed_labels].label = lab;
    nnamed_labels++;
    return lab;
}

/****************************************************************
 * Type helpers for lowering
 ****************************************************************/

static int
type_to_ir(struct cc_type *t)
{
    if (!t)
        return IR_I32;
    switch (t->kind) {
    case TY_CHAR:      return IR_I8;
    case TY_SHORT:     return IR_I16;
    case TY_LONG_LONG: return IR_I64;
    case TY_FLOAT:     return IR_F64;
    case TY_DOUBLE:    return IR_F64;
    default:           return IR_I32;
    }
}

static int
pointee_size(struct cc_type *t)
{
    if (!t)
        return 4;
    struct cc_type *base = NULL;
    if (t->kind == TY_PTR)
        base = t->base;
    else if (t->kind == TY_ARRAY)
        base = t->base;
    if (!base)
        return 4;
    return cc_type_size(base);
}

static int
is_float_type(struct cc_type *t)
{
    return t && (t->kind == TY_FLOAT || t->kind == TY_DOUBLE);
}

static int
is_i64_type(struct cc_type *t)
{
    return t && t->kind == TY_LONG_LONG;
}

static int
widen_to_i64(int val, struct cc_type *src_type)
{
    if (src_type && is_i64_type(src_type))
        return val;
    int op = (src_type && src_type->is_unsigned) ? IR_ZEXT64 : IR_SEXT64;
    struct ir_insn *ins = emit(op);
    ins->dst = new_temp();
    ins->a = val;
    return ins->dst;
}

/****************************************************************
 * Forward declarations
 ****************************************************************/

static int lower_expr(struct cc_node *n);
static int lower_addr(struct cc_node *n);
static void lower_stmt(struct cc_node *n);
static void lower_cond(struct cc_node *n, int ltrue, int lfalse);
static struct cc_type *lvalue_type(struct cc_node *n);

/****************************************************************
 * Emit a typed load from an address
 ****************************************************************/

static int
emit_load(int addr_temp, struct cc_type *t)
{
    struct ir_insn *ins;
    if (is_float_type(t)) {
        ins = emit(t->kind == TY_FLOAT ? IR_FLS : IR_FLD);
        ins->dst = new_temp();
        ins->a = addr_temp;
        return ins->dst;
    }
    if (is_i64_type(t)) {
        ins = emit(IR_LD64);
        ins->dst = new_temp();
        ins->a = addr_temp;
        return ins->dst;
    }
    int sz = cc_type_size(t);
    if (sz == 1) {
        ins = emit(t->is_unsigned ? IR_LB : IR_LBS);
    } else if (sz == 2) {
        ins = emit(t->is_unsigned ? IR_LH : IR_LHS);
    } else {
        ins = emit(IR_LW);
    }
    ins->dst = new_temp();
    ins->a = addr_temp;
    return ins->dst;
}

static void
emit_store(int addr_temp, int val_temp, struct cc_type *t)
{
    struct ir_insn *ins;
    if (is_float_type(t)) {
        ins = emit(t->kind == TY_FLOAT ? IR_FSS : IR_FSD);
        ins->a = addr_temp;
        ins->b = val_temp;
        return;
    }
    if (is_i64_type(t)) {
        ins = emit(IR_ST64);
        ins->a = addr_temp;
        ins->b = val_temp;
        return;
    }
    int sz = cc_type_size(t);
    if (sz == 1)
        ins = emit(IR_SB);
    else if (sz == 2)
        ins = emit(IR_SH);
    else
        ins = emit(IR_SW);
    ins->a = addr_temp;
    ins->b = val_temp;
}

/****************************************************************
 * String literal pool
 ****************************************************************/

static int str_counter;

static int
emit_string_literal(const char *s, int len)
{
    char namebuf[32];
    snprintf(namebuf, sizeof namebuf, "__str_%d", str_counter++);

    struct ir_global *g = arena_zalloc(lower_arena, sizeof *g);
    g->name = arena_strdup(lower_arena, namebuf);
    g->base_type = IR_I8;
    g->arr_size = len + 1;
    g->is_local = 1;
    g->init_string = arena_alloc(lower_arena, len + 1);
    memcpy(g->init_string, s, len);
    g->init_string[len] = '\0';
    g->init_strlen = len + 1;
    g->next = cur_prog->globals;
    cur_prog->globals = g;

    struct ir_insn *ins = emit(IR_LEA);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, namebuf);
    return ins->dst;
}

/****************************************************************
 * Static initializer flattening
 ****************************************************************/

static int
count_init_flat(struct cc_node *n)
{
    int count = 0;
    for (struct cc_node *e = n->body; e; e = e->next) {
        if (e->kind == ND_INIT_LIST)
            count += count_init_flat(e);
        else
            count++;
    }
    return count;
}

static char *
emit_string_global(const char *s, int len)
{
    char namebuf[32];
    snprintf(namebuf, sizeof namebuf, "__str_%d", str_counter++);

    struct ir_global *g = arena_zalloc(lower_arena, sizeof *g);
    g->name = arena_strdup(lower_arena, namebuf);
    g->base_type = IR_I8;
    g->arr_size = len + 1;
    g->is_local = 1;
    g->init_string = arena_alloc(lower_arena, len + 1);
    memcpy(g->init_string, s, len);
    g->init_string[len] = '\0';
    g->init_strlen = len + 1;
    g->next = cur_prog->globals;
    cur_prog->globals = g;

    return arena_strdup(lower_arena, namebuf);
}

static void
flatten_init(struct cc_node *n, int64_t *ivals, char **syms, int *pos)
{
    for (struct cc_node *e = n->body; e; e = e->next) {
        if (e->kind == ND_INIT_LIST) {
            flatten_init(e, ivals, syms, pos);
        } else if (e->kind == ND_INTLIT) {
            ivals[*pos] = e->ival;
            (*pos)++;
        } else if (e->kind == ND_UNOP && e->op == TOK_MINUS &&
                   e->a && e->a->kind == ND_INTLIT) {
            ivals[*pos] = -e->a->ival;
            (*pos)++;
        } else if (e->kind == ND_STRLIT) {
            syms[*pos] = emit_string_global(e->sval, e->slen);
            (*pos)++;
        } else if (e->kind == ND_VAR) {
            syms[*pos] = arena_strdup(lower_arena, e->name);
            (*pos)++;
        } else if (e->kind == ND_ADDR && e->a &&
                   e->a->kind == ND_VAR) {
            syms[*pos] = arena_strdup(lower_arena, e->a->name);
            (*pos)++;
        } else {
            die("lower:%d: non-constant initializer", e->line);
        }
    }
}

/****************************************************************
 * Address of lvalue (returns temp holding the address)
 ****************************************************************/

static int
lower_addr(struct cc_node *n)
{
    struct ir_insn *ins;

    switch (n->kind) {
    case ND_VAR: {
        struct local *lc = find_local(n->name);
        if (lc) {
            if (lc->static_name) {
                ins = emit(IR_LEA);
                ins->dst = new_temp();
                ins->sym = arena_strdup(lower_arena, lc->static_name);
                return ins->dst;
            }
            ins = emit(IR_ADL);
            ins->dst = new_temp();
            ins->slot = lc->slot;
            return ins->dst;
        }
        struct global *gl = find_global(n->name);
        if (!gl)
            die("lower:%d: undefined '%s'", n->line, n->name);
        ins = emit(IR_LEA);
        ins->dst = new_temp();
        ins->sym = arena_strdup(lower_arena, n->name);
        return ins->dst;
    }
    case ND_DEREF:
        return lower_expr(n->a);

    case ND_INDEX: {
        int base = lower_expr(n->a);
        int idx = lower_expr(n->b);
        struct cc_type *arr_type = lvalue_type(n->a);
        int elem_sz = pointee_size(arr_type);
        if (elem_sz != 1) {
            int sc = lower_const(elem_sz);
            ins = emit(IR_MUL);
            ins->dst = new_temp();
            ins->a = idx;
            ins->b = sc;
            idx = ins->dst;
        }
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = base;
        ins->b = idx;
        return ins->dst;
    }
    case ND_MEMBER: {
        struct cc_type *obj_type = lvalue_type(n->a);
        int obj_addr;
        if (obj_type && (obj_type->kind == TY_STRUCT || obj_type->kind == TY_UNION)) {
            obj_addr = lower_addr(n->a);
        } else {
            obj_addr = lower_expr(n->a);
            if (obj_type && obj_type->kind == TY_PTR)
                obj_type = obj_type->base;
        }
        if (!obj_type || (obj_type->kind != TY_STRUCT && obj_type->kind != TY_UNION))
            die("lower:%d: member access on non-struct", n->line);
        struct cc_field *f;
        for (f = obj_type->fields; f; f = f->next)
            if (strcmp(f->name, n->name) == 0)
                break;
        if (!f)
            die("lower:%d: no field '%s'", n->line, n->name);
        if (f->offset == 0)
            return obj_addr;
        int off = lower_const(f->offset);
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = obj_addr;
        ins->b = off;
        return ins->dst;
    }
    default:
        die("lower:%d: not an lvalue", n->line);
        return -1;
    }
}

/****************************************************************
 * Type of an lvalue (for load/store sizing)
 ****************************************************************/

static struct cc_type *
lvalue_type(struct cc_node *n)
{
    switch (n->kind) {
    case ND_VAR: {
        struct local *lc = find_local(n->name);
        if (lc) return lc->type;
        struct global *gl = find_global(n->name);
        if (gl) return gl->type;
        return cc_type_int();
    }
    case ND_DEREF: {
        struct cc_type *pt = lvalue_type(n->a);
        if (pt && pt->kind == TY_PTR)
            return pt->base;
        return cc_type_int();
    }
    case ND_INDEX: {
        struct cc_type *at = lvalue_type(n->a);
        if (at && (at->kind == TY_PTR || at->kind == TY_ARRAY))
            return at->base;
        return cc_type_int();
    }
    case ND_MEMBER: {
        struct cc_type *obj = lvalue_type(n->a);
        if (obj && obj->kind == TY_PTR)
            obj = obj->base;
        if (obj && (obj->kind == TY_STRUCT || obj->kind == TY_UNION)) {
            for (struct cc_field *f = obj->fields; f; f = f->next)
                if (strcmp(f->name, n->name) == 0)
                    return f->type;
        }
        return cc_type_int();
    }
    case ND_CAST:
        if (n->decl_type)
            return n->decl_type;
        return cc_type_int();
    case ND_ADDR: {
        struct cc_type *base = lvalue_type(n->a);
        return cc_type_ptr(lower_arena,base);
    }
    case ND_FLOATLIT:
        return n->type ? n->type : cc_type_int();
    case ND_INTLIT:
        return n->type ? n->type : cc_type_int();
    case ND_UNOP:
        return lvalue_type(n->a);
    case ND_BINOP: {
        struct cc_type *lt = lvalue_type(n->a);
        struct cc_type *rt = lvalue_type(n->b);
        if (is_float_type(lt) || is_float_type(rt)) {
            static struct cc_type ty_double = { .kind = TY_DOUBLE };
            return &ty_double;
        }
        if (is_i64_type(lt) || is_i64_type(rt))
            return cc_type_long_long();
        return lt;
    }
    case ND_CALL: {
        if (n->a->kind == ND_VAR) {
            struct global *gl = find_global(n->a->name);
            if (gl && gl->type && gl->type->kind == TY_FUNC && gl->type->base)
                return gl->type->base;
        }
        return cc_type_int();
    }
    default:
        return cc_type_int();
    }
}

/****************************************************************
 * Expression lowering (returns temp holding rvalue)
 ****************************************************************/

static int
lower_expr(struct cc_node *n)
{
    struct ir_insn *ins;

    if (!n)
        die("lower: null expression");

    switch (n->kind) {
    case ND_INTLIT:
        if (n->type && is_i64_type(n->type))
            return lower_const64(n->ival);
        return lower_const(n->ival);

    case ND_FLOATLIT: {
        char namebuf[32];
        snprintf(namebuf, sizeof namebuf, "__flt_%d", str_counter++);
        struct ir_global *fg = arena_zalloc(lower_arena, sizeof *fg);
        fg->name = arena_strdup(lower_arena, namebuf);
        fg->is_local = 1;
        fg->init_ivals = arena_alloc(lower_arena, sizeof(int64_t));
        int use_single = n->type && n->type->kind == TY_FLOAT;
        if (use_single) {
            union { float f; int32_t i; } u;
            u.f = (float)n->fval;
            fg->base_type = IR_I32;
            fg->init_ivals[0] = u.i;
        } else {
            union { double d; int64_t i; } u;
            u.d = n->fval;
            fg->base_type = IR_F64;
            fg->init_ivals[0] = u.i;
        }
        fg->init_count = 1;
        fg->next = cur_prog->globals;
        cur_prog->globals = fg;
        int addr = new_temp();
        ins = emit(IR_LEA);
        ins->dst = addr;
        ins->sym = arena_strdup(lower_arena, namebuf);
        ins = emit(use_single ? IR_FLS : IR_FLD);
        ins->dst = new_temp();
        ins->a = addr;
        return ins->dst;
    }

    case ND_STRLIT:
        return emit_string_literal(n->sval, n->slen);

    case ND_VAR: {
        struct local *lc = find_local(n->name);
        if (lc) {
            if (lc->static_name) {
                struct cc_type *t = lc->type;
                int addr = new_temp();
                ins = emit(IR_LEA);
                ins->dst = addr;
                ins->sym = arena_strdup(lower_arena, lc->static_name);
                if (t && (t->kind == TY_ARRAY || t->kind == TY_STRUCT ||
                          t->kind == TY_UNION))
                    return addr;
                return emit_load(addr, t ? t : cc_type_int());
            }
            struct cc_type *t = lc->type;
            if (t && (t->kind == TY_ARRAY || t->kind == TY_STRUCT ||
                      t->kind == TY_UNION)) {
                ins = emit(IR_ADL);
                ins->dst = new_temp();
                ins->slot = lc->slot;
                return ins->dst;
            }
            if (is_float_type(t)) {
                ins = emit(IR_FLDL);
                ins->dst = new_temp();
                ins->slot = lc->slot;
                return ins->dst;
            }
            if (is_i64_type(t)) {
                ins = emit(IR_LDL64);
                ins->dst = new_temp();
                ins->slot = lc->slot;
                return ins->dst;
            }
            ins = emit(IR_LDL);
            ins->dst = new_temp();
            ins->slot = lc->slot;
            return ins->dst;
        }
        struct global *gl = find_global(n->name);
        if (!gl) {
            add_global(n->name, cc_type_int(), 1);
            gl = find_global(n->name);
        }
        if (gl->is_func || (gl->type &&
            (gl->type->kind == TY_ARRAY || gl->type->kind == TY_FUNC))) {
            ins = emit(IR_LEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena, n->name);
            return ins->dst;
        }
        int addr = new_temp();
        ins = emit(IR_LEA);
        ins->dst = addr;
        ins->sym = arena_strdup(lower_arena, n->name);
        return emit_load(addr, gl->type ? gl->type : cc_type_int());
    }

    case ND_DEREF: {
        int ptr = lower_expr(n->a);
        struct cc_type *pt = lvalue_type(n->a);
        struct cc_type *target = (pt && pt->kind == TY_PTR) ? pt->base : cc_type_int();
        if (target && (target->kind == TY_STRUCT || target->kind == TY_UNION ||
                       target->kind == TY_ARRAY))
            return ptr;
        return emit_load(ptr, target);
    }

    case ND_ADDR:
        return lower_addr(n->a);

    case ND_INDEX: {
        int addr = lower_addr(n);
        struct cc_type *elem = lvalue_type(n);
        if (elem && (elem->kind == TY_STRUCT || elem->kind == TY_UNION ||
                     elem->kind == TY_ARRAY))
            return addr;
        return emit_load(addr, elem);
    }

    case ND_MEMBER: {
        int addr = lower_addr(n);
        struct cc_type *ft = lvalue_type(n);
        if (ft && (ft->kind == TY_STRUCT || ft->kind == TY_UNION ||
                   ft->kind == TY_ARRAY))
            return addr;
        return emit_load(addr, ft);
    }

    case ND_UNOP: {
        int val = lower_expr(n->a);
        struct cc_type *ot = lvalue_type(n->a);
        switch (n->op) {
        case TOK_MINUS:
            if (is_float_type(ot)) {
                ins = emit(IR_FNEG);
                ins->dst = new_temp();
                ins->a = val;
                return ins->dst;
            }
            if (is_i64_type(ot)) {
                ins = emit(IR_NEG64);
                ins->dst = new_temp();
                ins->a = val;
                return ins->dst;
            }
            ins = emit(IR_NEG);
            ins->dst = new_temp();
            ins->a = val;
            return ins->dst;
        case TOK_TILDE:
            if (is_i64_type(ot)) {
                int mask = lower_const64(-1);
                ins = emit(IR_XOR64);
                ins->dst = new_temp();
                ins->a = val;
                ins->b = mask;
                return ins->dst;
            }
            ins = emit(IR_NOT);
            ins->dst = new_temp();
            ins->a = val;
            return ins->dst;
        case TOK_BANG:
            if (is_i64_type(ot)) {
                int zero = lower_const64(0);
                ins = emit(IR_CMP64EQ);
                ins->dst = new_temp();
                ins->a = val;
                ins->b = zero;
                return ins->dst;
            } else {
                int zero = lower_const(0);
                ins = emit(IR_CMPEQ);
                ins->dst = new_temp();
                ins->a = val;
                ins->b = zero;
                return ins->dst;
            }
        default:
            die("lower:%d: unknown unop %d", n->line, n->op);
            return -1;
        }
    }

    case ND_BINOP: {
        if (n->op == TOK_ANDAND || n->op == TOK_OROR) {
            int ltrue = new_label();
            int lfalse = new_label();
            int lend = new_label();
            int result = new_temp();

            lower_cond(n, ltrue, lfalse);

            emit_label(ltrue);
            ins = emit(IR_LIC);
            ins->dst = result;
            ins->imm = 1;
            emit_jmp(lend);

            emit_label(lfalse);
            ins = emit(IR_LIC);
            ins->dst = result;
            ins->imm = 0;

            emit_label(lend);
            return result;
        }

        int lhs = lower_expr(n->a);
        int rhs = lower_expr(n->b);

        /* pointer arithmetic scaling */
        if (n->op == TOK_PLUS || n->op == TOK_MINUS) {
            struct cc_type *lt = lvalue_type(n->a);
            struct cc_type *rt = lvalue_type(n->b);
            if (lt && cc_type_is_ptr(lt) && !(rt && cc_type_is_ptr(rt))) {
                int sz = pointee_size(lt);
                if (sz > 1) {
                    int sc = lower_const(sz);
                    ins = emit(IR_MUL);
                    ins->dst = new_temp();
                    ins->a = rhs;
                    ins->b = sc;
                    rhs = ins->dst;
                }
            }
            /* ptr - ptr → divide by element size */
            if (n->op == TOK_MINUS && lt && cc_type_is_ptr(lt) &&
                rt && cc_type_is_ptr(rt)) {
                ins = emit(IR_SUB);
                ins->dst = new_temp();
                ins->a = lhs;
                ins->b = rhs;
                int diff = ins->dst;
                int sz = pointee_size(lt);
                if (sz > 1) {
                    int sc = lower_const(sz);
                    ins = emit(IR_DIVS);
                    ins->dst = new_temp();
                    ins->a = diff;
                    ins->b = sc;
                    return ins->dst;
                }
                return diff;
            }
        }

        struct cc_type *lt = lvalue_type(n->a);
        struct cc_type *rt = lvalue_type(n->b);
        int use_float = is_float_type(lt) || is_float_type(rt);

        if (use_float) {
            if (!is_float_type(lt)) {
                ins = emit(IR_ITOF);
                ins->dst = new_temp();
                ins->a = lhs;
                lhs = ins->dst;
            }
            if (!is_float_type(rt)) {
                ins = emit(IR_ITOF);
                ins->dst = new_temp();
                ins->a = rhs;
                rhs = ins->dst;
            }
            int ir_op;
            switch (n->op) {
            case TOK_PLUS:  ir_op = IR_FADD; break;
            case TOK_MINUS: ir_op = IR_FSUB; break;
            case TOK_STAR:  ir_op = IR_FMUL; break;
            case TOK_SLASH: ir_op = IR_FDIV; break;
            case TOK_EQ:    ir_op = IR_FCMPEQ; break;
            case TOK_LT:    ir_op = IR_FCMPLT; break;
            case TOK_LE:    ir_op = IR_FCMPLE; break;
            case TOK_GT: {
                int tmp = lhs; lhs = rhs; rhs = tmp;
                ir_op = IR_FCMPLT;
                break;
            }
            case TOK_GE: {
                int tmp = lhs; lhs = rhs; rhs = tmp;
                ir_op = IR_FCMPLE;
                break;
            }
            case TOK_NE: {
                ins = emit(IR_FCMPEQ);
                ins->dst = new_temp();
                ins->a = lhs;
                ins->b = rhs;
                int eq = ins->dst;
                int zero = lower_const(0);
                ins = emit(IR_CMPEQ);
                ins->dst = new_temp();
                ins->a = eq;
                ins->b = zero;
                return ins->dst;
            }
            default:
                die("lower:%d: float binop %d not supported", n->line, n->op);
                return -1;
            }
            ins = emit(ir_op);
            ins->dst = new_temp();
            ins->a = lhs;
            ins->b = rhs;
            return ins->dst;
        }

        int use_i64 = is_i64_type(lt) || is_i64_type(rt);
        int is_shift = (n->op == TOK_SHL || n->op == TOK_SHR);
        if (use_i64) {
            if (!is_i64_type(lt)) {
                ins = emit(lt && lt->is_unsigned ? IR_ZEXT64 : IR_SEXT64);
                ins->dst = new_temp();
                ins->a = lhs;
                lhs = ins->dst;
            }
            if (!is_shift && !is_i64_type(rt)) {
                ins = emit(rt && rt->is_unsigned ? IR_ZEXT64 : IR_SEXT64);
                ins->dst = new_temp();
                ins->a = rhs;
                rhs = ins->dst;
            }
            int is_uns = (lt && lt->is_unsigned) || (rt && rt->is_unsigned);
            int ir_op;
            switch (n->op) {
            case TOK_PLUS:    ir_op = IR_ADD64; break;
            case TOK_MINUS:   ir_op = IR_SUB64; break;
            case TOK_STAR:    ir_op = IR_MUL64; break;
            case TOK_SLASH: {
                const char *helper = is_uns ? "__udivdi3" : "__divdi3";
                ins = emit(IR_ARG64); ins->a = lhs; ins->imm = 0;
                ins = emit(IR_ARG64); ins->a = rhs; ins->imm = 1;
                ins = emit(IR_CALL64);
                ins->dst = new_temp();
                ins->sym = arena_strdup(lower_arena, helper);
                ins->nargs = 2;
                return ins->dst;
            }
            case TOK_PERCENT: {
                const char *helper = is_uns ? "__umoddi3" : "__moddi3";
                ins = emit(IR_ARG64); ins->a = lhs; ins->imm = 0;
                ins = emit(IR_ARG64); ins->a = rhs; ins->imm = 1;
                ins = emit(IR_CALL64);
                ins->dst = new_temp();
                ins->sym = arena_strdup(lower_arena, helper);
                ins->nargs = 2;
                return ins->dst;
            }
            case TOK_AMP:     ir_op = IR_AND64; break;
            case TOK_PIPE:    ir_op = IR_OR64; break;
            case TOK_CARET:   ir_op = IR_XOR64; break;
            case TOK_SHL:     ir_op = IR_SHL64; break;
            case TOK_SHR:     ir_op = is_uns ? IR_SHRU64 : IR_SHRS64; break;
            case TOK_EQ:      ir_op = IR_CMP64EQ; break;
            case TOK_NE:      ir_op = IR_CMP64NE; break;
            case TOK_LT:      ir_op = is_uns ? IR_CMP64LTU : IR_CMP64LTS; break;
            case TOK_LE:      ir_op = is_uns ? IR_CMP64LEU : IR_CMP64LES; break;
            case TOK_GT:      ir_op = is_uns ? IR_CMP64GTU : IR_CMP64GTS; break;
            case TOK_GE:      ir_op = is_uns ? IR_CMP64GEU : IR_CMP64GES; break;
            default:
                die("lower:%d: unknown i64 binop %d", n->line, n->op);
                return -1;
            }
            ins = emit(ir_op);
            ins->dst = new_temp();
            ins->a = lhs;
            ins->b = rhs;
            return ins->dst;
        }

        int ir_op;
        switch (n->op) {
        case TOK_PLUS:    ir_op = IR_ADD; break;
        case TOK_MINUS:   ir_op = IR_SUB; break;
        case TOK_STAR:    ir_op = IR_MUL; break;
        case TOK_SLASH:   ir_op = IR_DIVS; break;
        case TOK_PERCENT: ir_op = IR_MODS; break;
        case TOK_AMP:     ir_op = IR_AND; break;
        case TOK_PIPE:    ir_op = IR_OR; break;
        case TOK_CARET:   ir_op = IR_XOR; break;
        case TOK_SHL:     ir_op = IR_SHL; break;
        case TOK_SHR:     ir_op = IR_SHRS; break;
        case TOK_EQ:      ir_op = IR_CMPEQ; break;
        case TOK_NE:      ir_op = IR_CMPNE; break;
        case TOK_LT:      ir_op = IR_CMPLTS; break;
        case TOK_LE:      ir_op = IR_CMPLES; break;
        case TOK_GT:      ir_op = IR_CMPGTS; break;
        case TOK_GE:      ir_op = IR_CMPGES; break;
        default:
            die("lower:%d: unknown binop %d", n->line, n->op);
            return -1;
        }
        ins = emit(ir_op);
        ins->dst = new_temp();
        ins->a = lhs;
        ins->b = rhs;
        return ins->dst;
    }

    case ND_ASSIGN: {
        int val = lower_expr(n->b);
        if (n->a->kind == ND_VAR) {
            struct local *lc = find_local(n->a->name);
            if (lc && !lc->static_name && lc->type &&
                lc->type->kind != TY_ARRAY &&
                lc->type->kind != TY_STRUCT && lc->type->kind != TY_UNION) {
                int op;
                if (is_float_type(lc->type))
                    op = IR_FSTL;
                else if (is_i64_type(lc->type)) {
                    op = IR_STL64;
                    val = widen_to_i64(val, lvalue_type(n->b));
                } else
                    op = IR_STL;
                ins = emit(op);
                ins->a = val;
                ins->slot = lc->slot;
                return val;
            }
        }
        int addr = lower_addr(n->a);
        struct cc_type *t = lvalue_type(n->a);
        if (is_i64_type(t))
            val = widen_to_i64(val, lvalue_type(n->b));
        emit_store(addr, val, t);
        return val;
    }

    case ND_COMPOUND_ASSIGN: {
        int addr = lower_addr(n->a);
        struct cc_type *t = lvalue_type(n->a);
        int old_val = emit_load(addr, t);
        int rhs = lower_expr(n->b);

        /* scale for pointer += / -= */
        if ((n->op == TOK_PLUS_EQ || n->op == TOK_MINUS_EQ) &&
            t && cc_type_is_ptr(t)) {
            int sz = pointee_size(t);
            if (sz > 1) {
                int sc = lower_const(sz);
                ins = emit(IR_MUL);
                ins->dst = new_temp();
                ins->a = rhs;
                ins->b = sc;
                rhs = ins->dst;
            }
        }

        int ir_op;
        if (is_float_type(t)) {
            if (!is_float_type(lvalue_type(n->b))) {
                ins = emit(IR_ITOF);
                ins->dst = new_temp();
                ins->a = rhs;
                rhs = ins->dst;
            }
            switch (n->op) {
            case TOK_PLUS_EQ:  ir_op = IR_FADD; break;
            case TOK_MINUS_EQ: ir_op = IR_FSUB; break;
            case TOK_STAR_EQ:  ir_op = IR_FMUL; break;
            case TOK_SLASH_EQ: ir_op = IR_FDIV; break;
            default:
                die("lower:%d: float compound assign %d not supported",
                    n->line, n->op);
                return -1;
            }
        } else if (is_i64_type(t)) {
            if (!is_i64_type(lvalue_type(n->b))) {
                struct cc_type *rt = lvalue_type(n->b);
                ins = emit(rt && rt->is_unsigned ? IR_ZEXT64 : IR_SEXT64);
                ins->dst = new_temp();
                ins->a = rhs;
                rhs = ins->dst;
            }
            switch (n->op) {
            case TOK_PLUS_EQ:    ir_op = IR_ADD64; break;
            case TOK_MINUS_EQ:   ir_op = IR_SUB64; break;
            case TOK_STAR_EQ:    ir_op = IR_MUL64; break;
            case TOK_SLASH_EQ: {
                const char *h = t->is_unsigned ? "__udivdi3" : "__divdi3";
                ins = emit(IR_ARG64); ins->a = old_val; ins->imm = 0;
                ins = emit(IR_ARG64); ins->a = rhs; ins->imm = 1;
                ins = emit(IR_CALL64);
                ins->dst = new_temp();
                ins->sym = arena_strdup(lower_arena, h);
                ins->nargs = 2;
                int new_val = ins->dst;
                if (n->a->kind == ND_VAR) {
                    struct local *lc = find_local(n->a->name);
                    if (lc && !lc->static_name && lc->type &&
                        lc->type->kind != TY_ARRAY) {
                        ins = emit(IR_STL64);
                        ins->a = new_val;
                        ins->slot = lc->slot;
                        return new_val;
                    }
                }
                emit_store(addr, new_val, t);
                return new_val;
            }
            case TOK_PERCENT_EQ: {
                const char *h = t->is_unsigned ? "__umoddi3" : "__moddi3";
                ins = emit(IR_ARG64); ins->a = old_val; ins->imm = 0;
                ins = emit(IR_ARG64); ins->a = rhs; ins->imm = 1;
                ins = emit(IR_CALL64);
                ins->dst = new_temp();
                ins->sym = arena_strdup(lower_arena, h);
                ins->nargs = 2;
                int new_val = ins->dst;
                if (n->a->kind == ND_VAR) {
                    struct local *lc = find_local(n->a->name);
                    if (lc && !lc->static_name && lc->type &&
                        lc->type->kind != TY_ARRAY) {
                        ins = emit(IR_STL64);
                        ins->a = new_val;
                        ins->slot = lc->slot;
                        return new_val;
                    }
                }
                emit_store(addr, new_val, t);
                return new_val;
            }
            case TOK_AMP_EQ:     ir_op = IR_AND64; break;
            case TOK_PIPE_EQ:    ir_op = IR_OR64; break;
            case TOK_CARET_EQ:   ir_op = IR_XOR64; break;
            case TOK_SHL_EQ:     ir_op = IR_SHL64; break;
            case TOK_SHR_EQ:     ir_op = t->is_unsigned ? IR_SHRU64 : IR_SHRS64; break;
            default:
                die("lower:%d: unknown i64 compound assign", n->line);
                return -1;
            }
        } else {
            switch (n->op) {
            case TOK_PLUS_EQ:    ir_op = IR_ADD; break;
            case TOK_MINUS_EQ:   ir_op = IR_SUB; break;
            case TOK_STAR_EQ:    ir_op = IR_MUL; break;
            case TOK_SLASH_EQ:   ir_op = IR_DIVS; break;
            case TOK_PERCENT_EQ: ir_op = IR_MODS; break;
            case TOK_AMP_EQ:     ir_op = IR_AND; break;
            case TOK_PIPE_EQ:    ir_op = IR_OR; break;
            case TOK_CARET_EQ:   ir_op = IR_XOR; break;
            case TOK_SHL_EQ:     ir_op = IR_SHL; break;
            case TOK_SHR_EQ:     ir_op = IR_SHRS; break;
            default:
                die("lower:%d: unknown compound assign", n->line);
                return -1;
            }
        }
        ins = emit(ir_op);
        ins->dst = new_temp();
        ins->a = old_val;
        ins->b = rhs;
        int new_val = ins->dst;

        /* store back */
        if (n->a->kind == ND_VAR) {
            struct local *lc = find_local(n->a->name);
            if (lc && !lc->static_name && lc->type && lc->type->kind != TY_ARRAY) {
                int op;
                if (is_float_type(lc->type))
                    op = IR_FSTL;
                else if (is_i64_type(lc->type))
                    op = IR_STL64;
                else
                    op = IR_STL;
                ins = emit(op);
                ins->a = new_val;
                ins->slot = lc->slot;
                return new_val;
            }
        }
        emit_store(addr, new_val, t);
        return new_val;
    }

    case ND_PRE_INC:
    case ND_PRE_DEC: {
        int addr = lower_addr(n->a);
        struct cc_type *t = lvalue_type(n->a);
        int old_val = emit_load(addr, t);
        int delta = 1;
        if (t && cc_type_is_ptr(t))
            delta = pointee_size(t);
        int dc;
        int add_op;
        int stl_op;
        if (is_i64_type(t)) {
            dc = lower_const64(n->kind == ND_PRE_INC ? delta : -delta);
            add_op = IR_ADD64;
            stl_op = IR_STL64;
        } else {
            dc = lower_const(n->kind == ND_PRE_INC ? delta : -delta);
            add_op = IR_ADD;
            stl_op = IR_STL;
        }
        ins = emit(add_op);
        ins->dst = new_temp();
        ins->a = old_val;
        ins->b = dc;
        int new_val = ins->dst;
        if (n->a->kind == ND_VAR) {
            struct local *lc = find_local(n->a->name);
            if (lc && !lc->static_name && lc->type && lc->type->kind != TY_ARRAY) {
                ins = emit(stl_op);
                ins->a = new_val;
                ins->slot = lc->slot;
                return new_val;
            }
        }
        emit_store(addr, new_val, t);
        return new_val;
    }

    case ND_POST_INC:
    case ND_POST_DEC: {
        int addr = lower_addr(n->a);
        struct cc_type *t = lvalue_type(n->a);
        int old_val = emit_load(addr, t);
        int delta = 1;
        if (t && cc_type_is_ptr(t))
            delta = pointee_size(t);
        int dc;
        int add_op;
        int stl_op;
        if (is_i64_type(t)) {
            dc = lower_const64(n->kind == ND_POST_INC ? delta : -delta);
            add_op = IR_ADD64;
            stl_op = IR_STL64;
        } else {
            dc = lower_const(n->kind == ND_POST_INC ? delta : -delta);
            add_op = IR_ADD;
            stl_op = IR_STL;
        }
        ins = emit(add_op);
        ins->dst = new_temp();
        ins->a = old_val;
        ins->b = dc;
        int new_val = ins->dst;
        if (n->a->kind == ND_VAR) {
            struct local *lc = find_local(n->a->name);
            if (lc && !lc->static_name && lc->type && lc->type->kind != TY_ARRAY) {
                ins = emit(stl_op);
                ins->a = new_val;
                ins->slot = lc->slot;
                return old_val;
            }
        }
        emit_store(addr, new_val, t);
        return old_val;
    }

    case ND_TERNARY: {
        int ltrue = new_label();
        int lfalse = new_label();
        int lend = new_label();
        int result = new_temp();

        lower_cond(n->a, ltrue, lfalse);

        emit_label(ltrue);
        int tv = lower_expr(n->b);
        ins = emit(IR_MOV);
        ins->dst = result;
        ins->a = tv;
        emit_jmp(lend);

        emit_label(lfalse);
        int fv = lower_expr(n->c);
        ins = emit(IR_MOV);
        ins->dst = result;
        ins->a = fv;

        emit_label(lend);
        return result;
    }

    case ND_COMMA:
        lower_expr(n->a);
        return lower_expr(n->b);

    case ND_CAST: {
        int val = lower_expr(n->a);
        struct cc_type *from = lvalue_type(n->a);
        struct cc_type *to = n->decl_type;
        if (to && is_float_type(to) && !is_float_type(from)) {
            if (is_i64_type(from)) {
                ins = emit(IR_TRUNC64);
                ins->dst = new_temp();
                ins->a = val;
                val = ins->dst;
            }
            ins = emit(IR_ITOF);
            ins->dst = new_temp();
            ins->a = val;
            return ins->dst;
        }
        if (to && !is_float_type(to) && is_float_type(from)) {
            ins = emit(IR_FTOI);
            ins->dst = new_temp();
            ins->a = val;
            val = ins->dst;
            if (is_i64_type(to)) {
                ins = emit(IR_SEXT64);
                ins->dst = new_temp();
                ins->a = val;
                return ins->dst;
            }
            return val;
        }
        if (to && is_i64_type(to) && !is_i64_type(from)) {
            ins = emit(from && from->is_unsigned ? IR_ZEXT64 : IR_SEXT64);
            ins->dst = new_temp();
            ins->a = val;
            return ins->dst;
        }
        if (to && !is_i64_type(to) && is_i64_type(from)) {
            ins = emit(IR_TRUNC64);
            ins->dst = new_temp();
            ins->a = val;
            return ins->dst;
        }
        return val;
    }

    case ND_SIZEOF: {
        int sz;
        if (n->decl_type)
            sz = cc_type_size(n->decl_type);
        else if (n->a)
            sz = cc_type_size(lvalue_type(n->a));
        else
            sz = 4;
        return lower_const(sz);
    }

    case ND_CALL: {
        int is_indirect = 0;
        int fptr = -1;
        struct cc_type *callee_type = NULL;
        if (n->a->kind == ND_VAR) {
            struct local *lc = find_local(n->a->name);
            if (lc && lc->type && (lc->type->kind == TY_PTR ||
                                    lc->type->kind == TY_FUNC)) {
                is_indirect = 1;
                fptr = lower_expr(n->a);
                callee_type = lc->type;
                if (callee_type->kind == TY_PTR)
                    callee_type = callee_type->base;
            } else {
                struct global *gl = find_global(n->a->name);
                if (gl)
                    callee_type = gl->type;
            }
        } else {
            is_indirect = 1;
            fptr = lower_expr(n->a);
        }
        int args[32];
        int arg_kind[32];
        int nargs = 0;
        struct cc_param *pp = (callee_type && callee_type->kind == TY_FUNC)
                              ? callee_type->params : NULL;
        for (struct cc_node *a = n->b; a; a = a->next) {
            if (nargs >= 32)
                die("lower:%d: too many arguments", n->line);
            args[nargs] = lower_expr(a);
            struct cc_type *at = pp ? pp->type : lvalue_type(a);
            if (is_float_type(at))
                arg_kind[nargs] = 1;
            else if (is_i64_type(at)) {
                arg_kind[nargs] = 2;
                args[nargs] = widen_to_i64(args[nargs], lvalue_type(a));
            } else
                arg_kind[nargs] = 0;
            if (pp)
                pp = pp->next;
            nargs++;
        }
        for (int i = 0; i < nargs; i++) {
            int op = arg_kind[i] == 1 ? IR_FARG
                   : arg_kind[i] == 2 ? IR_ARG64
                   : IR_ARG;
            ins = emit(op);
            ins->a = args[i];
            ins->imm = i;
        }
        int fret = callee_type && callee_type->kind == TY_FUNC &&
                   is_float_type(callee_type->base);
        int i64ret = callee_type && callee_type->kind == TY_FUNC &&
                     is_i64_type(callee_type->base);
        if (is_indirect) {
            int op = fret ? IR_FCALLI : i64ret ? IR_CALLI64 : IR_CALLI;
            ins = emit(op);
            ins->dst = new_temp();
            ins->a = fptr;
            ins->nargs = nargs;
        } else {
            int op = fret ? IR_FCALL : i64ret ? IR_CALL64 : IR_CALL;
            ins = emit(op);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena, n->a->name);
            ins->nargs = nargs;
        }
        return ins->dst;
    }

    case ND_INIT_LIST:
        die("lower:%d: init list in expression context", n->line);
        return -1;

    default:
        die("lower:%d: unhandled expression kind %d", n->line, n->kind);
        return -1;
    }
}

/****************************************************************
 * Conditional lowering (short-circuit)
 ****************************************************************/

static void
lower_cond(struct cc_node *n, int ltrue, int lfalse)
{
    if (n->kind == ND_BINOP && n->op == TOK_ANDAND) {
        int lmid = new_label();
        lower_cond(n->a, lmid, lfalse);
        emit_label(lmid);
        lower_cond(n->b, ltrue, lfalse);
        return;
    }
    if (n->kind == ND_BINOP && n->op == TOK_OROR) {
        int lmid = new_label();
        lower_cond(n->a, ltrue, lmid);
        emit_label(lmid);
        lower_cond(n->b, ltrue, lfalse);
        return;
    }
    if (n->kind == ND_UNOP && n->op == TOK_BANG) {
        lower_cond(n->a, lfalse, ltrue);
        return;
    }
    int v = lower_expr(n);
    emit_bnz(v, ltrue);
    emit_jmp(lfalse);
}

/****************************************************************
 * Statement lowering
 ****************************************************************/

static int static_counter;

static void
lower_local_decl(struct cc_node *n)
{
    struct cc_type *t = n->decl_type;

    if (n->is_extern || t->kind == TY_FUNC) {
        add_global(n->name, t, t->kind == TY_FUNC);
        return;
    }

    if (n->is_static) {
        char mangled[128];
        snprintf(mangled, sizeof mangled, "%s.%s.%d",
                 cur_fn_name, n->name, static_counter++);
        struct ir_global *g = arena_zalloc(lower_arena, sizeof *g);
        g->name = arena_strdup(lower_arena, mangled);
        g->is_local = 1;
        g->base_type = type_to_ir(t);
        if (t->kind == TY_ARRAY) {
            g->base_type = type_to_ir(t->base);
            if (t->array_len < 0 && n->a &&
                n->a->kind == ND_INIT_LIST) {
                int flat = count_init_flat(n->a);
                int elem_words = cc_type_size(t->base) / 4;
                if (elem_words < 1) elem_words = 1;
                t->array_len = flat / elem_words;
            }
            g->arr_size = t->array_len > 0 ? t->array_len : 0;
        }
        if (n->a) {
            if (n->a->kind == ND_INIT_LIST) {
                int cnt = count_init_flat(n->a);
                g->init_ivals = arena_alloc(lower_arena, cnt * sizeof(int64_t));
                g->init_syms = arena_zalloc(lower_arena, cnt * sizeof(char *));
                int pos = 0;
                flatten_init(n->a, g->init_ivals, g->init_syms, &pos);
                g->init_count = pos;
                if (g->arr_size == 0)
                    g->arr_size = pos;
            } else if (n->a->kind == ND_STRLIT) {
                g->init_string = arena_alloc(lower_arena, n->a->slen + 1);
                memcpy(g->init_string, n->a->sval, n->a->slen);
                g->init_string[n->a->slen] = '\0';
                g->init_strlen = n->a->slen + 1;
                if (g->arr_size == 0)
                    g->arr_size = n->a->slen + 1;
            } else if (n->a->kind == ND_INTLIT) {
                g->init_ivals = arena_alloc(lower_arena, sizeof(int64_t));
                g->init_ivals[0] = n->a->ival;
                g->init_count = 1;
            } else if (n->a->kind == ND_UNOP &&
                       n->a->op == TOK_MINUS &&
                       n->a->a &&
                       n->a->a->kind == ND_INTLIT) {
                g->init_ivals = arena_alloc(lower_arena, sizeof(int64_t));
                g->init_ivals[0] = -n->a->a->ival;
                g->init_count = 1;
            }
        }
        g->next = cur_prog->globals;
        cur_prog->globals = g;
        add_global(mangled, t, 0);
        if (n->name) {
            if (nlocals == local_cap) {
                local_cap = local_cap ? local_cap * 2 : 32;
                locals = realloc(locals, local_cap * sizeof *locals);
            }
            locals[nlocals].name = arena_strdup(lower_arena,n->name);
            locals[nlocals].slot = -1;
            locals[nlocals].type = t;
            locals[nlocals].static_name = arena_strdup(lower_arena, mangled);
            nlocals++;
        }
        return;
    }

    int sz = cc_type_size(t);
    if (is_float_type(t) && sz < 8)
        sz = 8;
    if (is_i64_type(t) && sz < 8)
        sz = 8;
    int slot = alloc_slot(sz);
    if (n->name)
        add_local(n->name, slot, t);

    if (!n->a)
        return;

    if (n->a->kind == ND_INIT_LIST && t &&
        (t->kind == TY_ARRAY || t->kind == TY_STRUCT)) {
        /* array/struct initializer list */
        struct ir_insn *ins = emit(IR_ADL);
        ins->dst = new_temp();
        ins->slot = slot;
        int base_addr = ins->dst;

        if (t->kind == TY_ARRAY) {
            int elem_sz = cc_type_size(t->base);
            int offset = 0;
            for (struct cc_node *e = n->a->body; e; e = e->next) {
                int val = lower_expr(e);
                if (offset == 0) {
                    emit_store(base_addr, val, t->base);
                } else {
                    int off = lower_const(offset);
                    ins = emit(IR_ADD);
                    ins->dst = new_temp();
                    ins->a = base_addr;
                    ins->b = off;
                    emit_store(ins->dst, val, t->base);
                }
                offset += elem_sz;
            }
        } else {
            struct cc_field *f = t->fields;
            for (struct cc_node *e = n->a->body; e && f; e = e->next, f = f->next) {
                int val = lower_expr(e);
                if (f->offset == 0) {
                    emit_store(base_addr, val, f->type);
                } else {
                    int off = lower_const(f->offset);
                    ins = emit(IR_ADD);
                    ins->dst = new_temp();
                    ins->a = base_addr;
                    ins->b = off;
                    emit_store(ins->dst, val, f->type);
                }
            }
        }
    } else {
        int val = lower_expr(n->a);
        int op;
        if (is_float_type(t))
            op = IR_FSTL;
        else if (is_i64_type(t)) {
            op = IR_STL64;
            val = widen_to_i64(val, lvalue_type(n->a));
        } else
            op = IR_STL;
        struct ir_insn *ins = emit(op);
        ins->a = val;
        ins->slot = slot;
    }
}

static void
collect_cases(struct cc_node *n, struct switch_ctx *sw)
{
    if (!n)
        return;
    if (n->kind == ND_CASE) {
        int lab = new_label();
        sw->case_vals[sw->ncases] = n->a->ival;
        sw->case_labels[sw->ncases] = lab;
        sw->ncases++;
        collect_cases(n->b, sw);
        return;
    }
    if (n->kind == ND_DEFAULT) {
        sw->default_label = new_label();
        collect_cases(n->a, sw);
        return;
    }
    if (n->kind == ND_BLOCK) {
        for (struct cc_node *s = n->body; s; s = s->next)
            collect_cases(s, sw);
    }
}

static void
lower_stmt(struct cc_node *n)
{
    struct ir_insn *ins;

    if (!n)
        return;

    switch (n->kind) {
    case ND_BLOCK: {
        int saved_nlocals = nlocals;
        for (struct cc_node *s = n->body; s; s = s->next)
            lower_stmt(s);
        nlocals = saved_nlocals;
        return;
    }

    case ND_EXPR_STMT:
        if (n->a)
            lower_expr(n->a);
        return;

    case ND_LOCAL_DECL:
        lower_local_decl(n);
        /* handle chained declarations (int a, b, c;) */
        for (struct cc_node *d = n->next; d && d->kind == ND_LOCAL_DECL; d = d->next)
            lower_local_decl(d);
        return;

    case ND_IF: {
        int ltrue = new_label();
        int lfalse = new_label();
        int lend = new_label();
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
    }

    case ND_WHILE: {
        int ltop = new_label();
        int lbody = new_label();
        int lbrk = new_label();
        emit_label(ltop);
        lower_cond(n->a, lbody, lbrk);
        emit_label(lbody);
        loop_stack[nloops].brk = lbrk;
        loop_stack[nloops].cont = ltop;
        nloops++;
        lower_stmt(n->b);
        nloops--;
        emit_jmp(ltop);
        emit_label(lbrk);
        return;
    }

    case ND_DO_WHILE: {
        int ltop = new_label();
        int lcont = new_label();
        int lbrk = new_label();
        emit_label(ltop);
        loop_stack[nloops].brk = lbrk;
        loop_stack[nloops].cont = lcont;
        nloops++;
        lower_stmt(n->a);
        nloops--;
        emit_label(lcont);
        lower_cond(n->b, ltop, lbrk);
        emit_label(lbrk);
        return;
    }

    case ND_FOR: {
        int ltop = new_label();
        int lbody = new_label();
        int lcont = new_label();
        int lbrk = new_label();
        /* init */
        if (n->a)
            lower_stmt(n->a);
        emit_label(ltop);
        /* condition */
        if (n->b)
            lower_cond(n->b, lbody, lbrk);
        else
            emit_jmp(lbody);
        emit_label(lbody);
        loop_stack[nloops].brk = lbrk;
        loop_stack[nloops].cont = lcont;
        nloops++;
        lower_stmt(n->d);
        nloops--;
        emit_label(lcont);
        /* increment */
        if (n->c)
            lower_expr(n->c);
        emit_jmp(ltop);
        emit_label(lbrk);
        return;
    }

    case ND_SWITCH: {
        struct switch_ctx sw = {0};
        sw.end_label = new_label();
        sw.default_label = -1;
        collect_cases(n->b, &sw);

        int val = lower_expr(n->a);

        for (int i = 0; i < sw.ncases; i++) {
            int cv = lower_const(sw.case_vals[i]);
            ins = emit(IR_CMPEQ);
            ins->dst = new_temp();
            ins->a = val;
            ins->b = cv;
            emit_bnz(ins->dst, sw.case_labels[i]);
        }
        if (sw.default_label >= 0)
            emit_jmp(sw.default_label);
        else
            emit_jmp(sw.end_label);

        struct switch_ctx *prev = cur_switch;
        cur_switch = &sw;
        loop_stack[nloops].brk = sw.end_label;
        loop_stack[nloops].cont = -1;
        nloops++;
        lower_stmt(n->b);
        nloops--;
        cur_switch = prev;
        emit_label(sw.end_label);
        return;
    }

    case ND_CASE: {
        if (!cur_switch)
            die("lower:%d: case outside switch", n->line);
        for (int i = 0; i < cur_switch->ncases; i++) {
            if (cur_switch->case_vals[i] == n->a->ival) {
                emit_label(cur_switch->case_labels[i]);
                break;
            }
        }
        lower_stmt(n->b);
        return;
    }

    case ND_DEFAULT:
        if (!cur_switch)
            die("lower:%d: default outside switch", n->line);
        emit_label(cur_switch->default_label);
        lower_stmt(n->a);
        return;

    case ND_BREAK:
        if (nloops == 0)
            die("lower:%d: break outside loop/switch", n->line);
        emit_jmp(loop_stack[nloops - 1].brk);
        return;

    case ND_CONTINUE:
        if (nloops == 0)
            die("lower:%d: continue outside loop", n->line);
        emit_jmp(loop_stack[nloops - 1].cont);
        return;

    case ND_RETURN:
        if (n->a) {
            int val = lower_expr(n->a);
            if (cur_fn_returns_i64)
                val = widen_to_i64(val, lvalue_type(n->a));
            int op = cur_fn_returns_float ? IR_FRETV
                   : cur_fn_returns_i64   ? IR_RETV64
                   : IR_RETV;
            ins = emit(op);
            ins->a = val;
        } else {
            emit(IR_RET);
        }
        return;

    case ND_GOTO:
        emit_jmp(get_named_label(n->name));
        return;

    case ND_LABEL:
        emit_label(get_named_label(n->name));
        lower_stmt(n->a);
        return;

    default:
        die("lower:%d: unhandled statement kind %d", n->line, n->kind);
    }
}

/****************************************************************
 * Function lowering
 ****************************************************************/

static struct ir_func *
lower_function(struct cc_node *fndef)
{
    struct ir_func *fn = ir_new_func(lower_arena, fndef->name);
    fn->is_local = fndef->is_static;
    cur_fn = fn;
    cur_fn_name = fndef->name;
    nloops = 0;
    nlocals = 0;
    nslots = 0;
    nnamed_labels = 0;
    cur_switch = NULL;

    struct cc_type *ftype = fndef->decl_type;
    cur_fn_returns_float = is_float_type(ftype->base);
    cur_fn_returns_i64 = is_i64_type(ftype->base);
    int nparams = 0;

    /* allocate slots for params */
    for (struct cc_param *p = ftype->params; p; p = p->next) {
        int sz = cc_type_size(p->type);
        if (is_float_type(p->type) && sz < 8)
            sz = 8;
        else if (is_i64_type(p->type) && sz < 8)
            sz = 8;
        else if (sz < 4)
            sz = 4;
        int slot = alloc_slot(sz);
        if (p->name)
            add_local(p->name, slot, p->type);
        nparams++;
    }
    fn->nparams = nparams;

    struct ir_insn *ins = emit(IR_FUNC);
    ins->sym = arena_strdup(lower_arena, fndef->name);
    ins->nargs = nparams;

    lower_stmt(fndef->body);

    /* ensure function ends with a return */
    if (!fn->tail || (fn->tail->op != IR_RET && fn->tail->op != IR_RETV &&
                      fn->tail->op != IR_RETV64 && fn->tail->op != IR_FRETV)) {
        if (cur_fn_returns_i64) {
            int z = lower_const64(0);
            ins = emit(IR_RETV64);
            ins->a = z;
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

static void
lower_global_decl(struct cc_node *gn)
{
    struct cc_type *t = gn->decl_type;
    if (gn->is_extern) {
        add_global(gn->name, t, t->kind == TY_FUNC);
        return;
    }
    if (t->kind == TY_FUNC) {
        add_global(gn->name, t, 1);
        return;
    }

    struct ir_global *g = arena_zalloc(lower_arena, sizeof *g);
    g->name = arena_strdup(lower_arena, gn->name);

    if (t->kind == TY_ARRAY) {
        g->base_type = type_to_ir(t->base);
        if (t->array_len < 0 && gn->a &&
            gn->a->kind == ND_INIT_LIST) {
            int flat = count_init_flat(gn->a);
            int elem_words = cc_type_size(t->base) / 4;
            if (elem_words < 1) elem_words = 1;
            t->array_len = flat / elem_words;
        }
        g->arr_size = t->array_len > 0 ? t->array_len : 0;
    } else if (t->kind == TY_PTR) {
        g->base_type = IR_I32;
        g->is_ptr = 1;
    } else {
        g->base_type = type_to_ir(t);
    }

    if (gn->a) {
        if (gn->a->kind == ND_STRLIT) {
            g->init_string = arena_alloc(lower_arena, gn->a->slen + 1);
            memcpy(g->init_string, gn->a->sval, gn->a->slen);
            g->init_string[gn->a->slen] = '\0';
            g->init_strlen = gn->a->slen + 1;
            if (g->arr_size == 0)
                g->arr_size = gn->a->slen + 1;
        } else if (gn->a->kind == ND_INIT_LIST) {
            int cnt = count_init_flat(gn->a);
            g->init_ivals = arena_alloc(lower_arena, cnt * sizeof(int64_t));
            g->init_syms = arena_zalloc(lower_arena, cnt * sizeof(char *));
            int pos = 0;
            flatten_init(gn->a, g->init_ivals, g->init_syms, &pos);
            g->init_count = pos;
            if (g->arr_size == 0)
                g->arr_size = pos;
        } else if (gn->a->kind == ND_INTLIT) {
            g->init_ivals = arena_alloc(lower_arena, sizeof(int64_t));
            g->init_ivals[0] = gn->a->ival;
            g->init_count = 1;
        } else if (gn->a->kind == ND_UNOP && gn->a->op == TOK_MINUS &&
                   gn->a->a && gn->a->a->kind == ND_INTLIT) {
            g->init_ivals = arena_alloc(lower_arena, sizeof(int64_t));
            g->init_ivals[0] = -gn->a->a->ival;
            g->init_count = 1;
        }
    }

    g->next = cur_prog->globals;
    cur_prog->globals = g;
    add_global(gn->name, t, 0);
}

/****************************************************************
 * Entry point
 ****************************************************************/

struct ir_program *
cc_lower_program(struct arena *a, struct cc_node *ast)
{
    lower_arena = a;
    struct ir_program *prog = arena_zalloc(lower_arena, sizeof *prog);
    cur_prog = prog;
    nglobals = 0;
    str_counter = 0;

    /* first pass: register globals and functions */
    for (struct cc_node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_GLOBAL_DECL)
            lower_global_decl(d);
        else if (d->kind == ND_FUNC_DEF)
            add_global(d->name, d->decl_type, 1);
    }

    /* second pass: lower function bodies */
    struct ir_func **ftail = &prog->funcs;
    for (struct cc_node *d = ast->body; d; d = d->next) {
        if (d->kind != ND_FUNC_DEF)
            continue;
        struct ir_func *fn = lower_function(d);
        *ftail = fn;
        ftail = &fn->next;
    }

    free(slot_sizes);
    slot_sizes = NULL;
    nslots = slot_cap = 0;

    free(locals);
    locals = NULL;
    nlocals = local_cap = 0;

    free(globals);
    globals = NULL;
    nglobals = global_cap = 0;

    return prog;
}
