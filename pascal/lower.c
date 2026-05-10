/* lower.c : Pascal AST to IR lowering */

#include "pascal.h"
#include "arena.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Convenience wrappers
 ****************************************************************/

static struct arena *lower_arena;
static struct ir_func *cur_fn;
static struct ir_program *cur_prog;
static int is_string_type(struct node *n);
static int is_set_type(struct node *n);
static int lower_set_addr(struct node *n);

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
 * Symbol table
 ****************************************************************/

struct sym {
    char *name;
    int is_local;
    int slot;
    int is_func;
    int is_const;
    long const_val;
    char *const_str;
    int const_slen;
    int is_var_param;
    char *type_name;
    struct node *decl;
    char *ir_name;
    int ncaptures;
    char **captures;
};

static struct sym *syms;
static int nsyms;
static int sym_cap;
static int sym_base;

static int *slot_sizes;
static int nslots;
static int slot_cap;

static int
alloc_slot(int bytes)
{
    if (nslots == slot_cap) {
        slot_cap = slot_cap ? slot_cap * 2 : 16;
        slot_sizes = realloc(slot_sizes,
                             slot_cap * sizeof(*slot_sizes));
        if (!slot_sizes)
            die("oom");
    }
    slot_sizes[nslots] = bytes;
    return nslots++;
}

static void
push_sym(struct sym s)
{
    if (nsyms == sym_cap) {
        sym_cap = sym_cap ? sym_cap * 2 : 32;
        syms = realloc(syms, sym_cap * sizeof(*syms));
        if (!syms)
            die("oom");
    }
    syms[nsyms++] = s;
}

static struct sym *
find_sym(const char *name)
{
    for (int i = nsyms - 1; i >= 0; i--)
        if (strcmp(syms[i].name, name) == 0)
            return &syms[i];
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
 * With-statement context stack
 ****************************************************************/

struct with_ctx {
    int base_slot;
    struct type_def *td;
};

static struct with_ctx with_stack[16];
static int nwith;

/****************************************************************
 * Nested function support
 ****************************************************************/

static struct ir_func *nested_funcs;
static struct ir_func **nested_ftail;

static void
collect_names(struct node *n, char **names, int *nn, int cap)
{
    if (!n)
        return;
    if (n->kind == N_NAME && n->name) {
        int dup = 0;
        for (int i = 0; i < *nn; i++)
            if (strcmp(names[i], n->name) == 0) {
                dup = 1;
                break;
            }
        if (!dup && *nn < cap)
            names[(*nn)++] = n->name;
    }
    collect_names(n->a, names, nn, cap);
    collect_names(n->b, names, nn, cap);
    collect_names(n->c, names, nn, cap);
    collect_names(n->d, names, nn, cap);
    collect_names(n->next, names, nn, cap);
}

static int
find_captures(struct node *decl, int outer_start,
              char **caps, int max_caps)
{
    char *names[256];
    int nn = 0, nc = 0;
    struct node *block = decl->b;

    if (block && block->kind == N_BLOCK) {
        collect_names(block->d, names, &nn, 256);
        for (struct node *v = block->b; v; v = v->next)
            collect_names(v->a, names, &nn, 256);
    }

    for (int i = 0; i < nn; i++) {
        int skip = 0;

        for (struct node *p = decl->a; p; p = p->next)
            if (strcmp(p->name, names[i]) == 0) {
                skip = 1;
                break;
            }
        if (skip)
            continue;

        if (block && block->kind == N_BLOCK) {
            for (struct node *v = block->b; v; v = v->next)
                if (strcmp(v->name, names[i]) == 0) {
                    skip = 1;
                    break;
                }
        }
        if (skip)
            continue;

        if (block && block->kind == N_BLOCK) {
            for (struct node *c = block->a; c; c = c->next)
                if (c->kind == N_CONSTDECL &&
                    strcmp(c->name, names[i]) == 0) {
                    skip = 1;
                    break;
                }
        }
        if (skip)
            continue;

        if (strcmp(names[i], decl->name) == 0)
            continue;

        for (int j = nsyms - 1; j >= outer_start; j--) {
            if (strcmp(syms[j].name, names[i]) == 0) {
                if (syms[j].is_local && !syms[j].is_func &&
                    !syms[j].is_const) {
                    if (nc < max_caps)
                        caps[nc++] = names[i];
                }
                break;
            }
        }
    }
    return nc;
}

/****************************************************************
 * Function return tracking
 ****************************************************************/

static char *cur_func_name;
static int ret_slot;
static int exit_label;

/****************************************************************
 * Type table
 ****************************************************************/

struct type_field {
    char *name;
    char *type_name;
    int offset;
    int size;
};

struct type_def {
    char *name;
    int total_size;
    int nfields;
    struct type_field *fields;
    int is_array;
    int lo_bound;
    int hi_bound;
    int elem_size;
    char *elem_type_name;
    int is_string;
    int maxlen;
    int is_set;
};

static struct type_def *types;
static int ntypes;
static int type_cap;

static struct type_def *find_type(const char *name);
static long const_eval(struct node *n);

static void
register_type(struct node *td)
{
    struct type_def *t;
    struct node *f;
    int nf, off;

    if (ntypes == type_cap) {
        type_cap = type_cap ? type_cap * 2 : 8;
        types = realloc(types, type_cap * sizeof(*types));
        if (!types)
            die("oom");
    }
    t = &types[ntypes++];
    memset(t, 0, sizeof(*t));
    t->name = td->name;

    if (td->op == 2) {
        int ml = (int)td->ival;
        t->is_string = 1;
        t->maxlen = ml;
        t->total_size = ml + 1;
        return;
    }

    if (td->op == 3) {
        t->is_set = 1;
        t->total_size = 32;
        return;
    }

    if (td->op == 1) {
        long lo = const_eval(td->b);
        long hi = const_eval(td->c);
        int esz = 4;
        if (td->sval) {
            struct type_def *et = find_type(td->sval);
            if (et)
                esz = et->total_size;
        }
        t->is_array = 1;
        t->lo_bound = (int)lo;
        t->hi_bound = (int)hi;
        t->elem_size = esz;
        t->elem_type_name = td->sval;
        t->total_size = (int)(hi - lo + 1) * esz;
        return;
    }

    nf = 0;
    for (f = td->a; f; f = f->next) {
        if (f->kind == N_VARIANTPART) {
            if (f->name)
                nf++;
            for (struct node *arm = f->a; arm;
                arm = arm->next)
                for (struct node *vf = arm->b; vf;
                    vf = vf->next)
                    nf++;
        } else {
            nf++;
        }
    }
    t->nfields = nf;
    t->fields = arena_zalloc(lower_arena, nf * sizeof(*t->fields));
    off = 0;
    nf = 0;
    {
        int align = td->ival > 0 ? (int)td->ival : 4;
        for (f = td->a; f; f = f->next) {
            if (f->kind == N_VARIANTPART) {
                int vbase, max_vsize;
                if (f->name) {
                    int tsz = 4;
                    if (align > 1 && (off % align) != 0)
                        off += align - (off % align);
                    t->fields[nf].name = f->name;
                    t->fields[nf].type_name = f->sval;
                    t->fields[nf].offset = off;
                    t->fields[nf].size = tsz;
                    off += tsz;
                    nf++;
                }
                if (align > 1 && (off % align) != 0)
                    off += align - (off % align);
                vbase = off;
                max_vsize = 0;
                for (struct node *arm = f->a; arm;
                    arm = arm->next) {
                    int arm_off = vbase;
                    for (struct node *vf = arm->b;
                        vf; vf = vf->next) {
                        int fsz = 4;
                        if (vf->sval) {
                            struct type_def *ft =
                                find_type(
                                vf->sval);
                            if (ft)
                                fsz =
                                    ft->total_size;
                        }
                        if (align > 1 &&
                            (arm_off % align) != 0)
                            arm_off += align -
                                (arm_off % align);
                        t->fields[nf].name =
                            vf->name;
                        t->fields[nf].type_name =
                            vf->sval;
                        t->fields[nf].offset =
                            arm_off;
                        t->fields[nf].size = fsz;
                        arm_off += fsz;
                        nf++;
                    }
                    if (arm_off - vbase > max_vsize)
                        max_vsize = arm_off - vbase;
                }
                off = vbase + max_vsize;
                continue;
            }
            {
                int fsz = 4;
                t->fields[nf].name = f->name;
                t->fields[nf].type_name = f->sval;
                if (f->sval) {
                    struct type_def *ft =
                        find_type(f->sval);
                    if (ft)
                        fsz = ft->total_size;
                }
                if (align > 1 && (off % align) != 0)
                    off += align - (off % align);
                t->fields[nf].offset = off;
                t->fields[nf].size = fsz;
                off += fsz;
                nf++;
            }
        }
        if (align > 1 && (off % align) != 0)
            off += align - (off % align);
    }
    t->total_size = off;
}

static struct type_def *
find_type(const char *name)
{
    for (int i = 0; i < ntypes; i++)
        if (strcmp(types[i].name, name) == 0)
            return &types[i];
    return NULL;
}

static int
type_field_offset(struct type_def *t, const char *field)
{
    for (int i = 0; i < t->nfields; i++)
        if (strcmp(t->fields[i].name, field) == 0)
            return t->fields[i].offset;
    return -1;
}

/****************************************************************
 * String literal pool
 ****************************************************************/

static int strctr;

static void
add_string_global(const char *label, const char *data, int len)
{
    struct ir_global *g;

    g = arena_zalloc(lower_arena, sizeof(*g));
    g->name = arena_strdup(lower_arena, label);
    g->base_type = IR_I8;
    g->arr_size = len;
    g->is_local = 1;
    g->init_string = arena_alloc(lower_arena, len);
    memcpy(g->init_string, data, len);
    g->init_strlen = len;
    g->next = cur_prog->globals;
    cur_prog->globals = g;
}

/****************************************************************
 * IR emission helpers
 ****************************************************************/

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
emit_block_copy(int dst_addr, int src_addr, int nbytes)
{
    struct ir_insn *ins;
    int nwords = nbytes / 4;

    for (int i = 0; i < nwords; i++) {
        int sa = src_addr, da = dst_addr, tmp;
        if (i > 0) {
            int off = lower_const(i * 4);
            ins = emit(IR_ADD);
            ins->dst = new_temp();
            ins->a = src_addr;
            ins->b = off;
            sa = ins->dst;
            ins = emit(IR_ADD);
            ins->dst = new_temp();
            ins->a = dst_addr;
            ins->b = off;
            da = ins->dst;
        }
        tmp = new_temp();
        ins = emit(IR_LW);
        ins->dst = tmp;
        ins->a = sa;
        ins = emit(IR_SW);
        ins->a = da;
        ins->b = tmp;
    }
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
emit_bz(int t, int lab)
{
    struct ir_insn *ins;

    ins = emit(IR_BZ);
    ins->a = t;
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

static void
emit_call(const char *name, int *args, int nargs)
{
    struct ir_insn *ins;

    for (int i = 0; i < nargs; i++) {
        ins = emit(IR_ARG);
        ins->a = args[i];
        ins->imm = i;
    }
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena,name);
    ins->nargs = nargs;
}

static int
emit_call_ret(const char *name, int *args, int nargs)
{
    struct ir_insn *ins;

    for (int i = 0; i < nargs; i++) {
        ins = emit(IR_ARG);
        ins->a = args[i];
        ins->imm = i;
    }
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena,name);
    ins->nargs = nargs;
    return ins->dst;
}

/****************************************************************
 * Expression lowering
 ****************************************************************/

static int lower_expr(struct node *n);
static void lower_stmt(struct node *n);
static int lower_call(struct node *n);

static int
load_sym(struct sym *s)
{
    struct ir_insn *ins;

    if (s->is_const) {
        if (s->const_str) {
            char namebuf[32];
            snprintf(namebuf, sizeof(namebuf),
                     "__str_%d", strctr++);
            add_string_global(namebuf, s->const_str,
                              s->const_slen + 1);
            ins = emit(IR_LEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena,namebuf);
            return ins->dst;
        }
        return lower_const(s->const_val);
    }
    if (s->is_local) {
        if (s->is_var_param) {
            int addr = new_temp();
            ins = emit(IR_LDL);
            ins->dst = addr;
            ins->slot = s->slot;
            ins = emit(IR_LW);
            ins->dst = new_temp();
            ins->a = addr;
            return ins->dst;
        }
        ins = emit(IR_LDL);
        ins->dst = new_temp();
        ins->slot = s->slot;
        return ins->dst;
    }
    {
        int addr;
        ins = emit(IR_LEA);
        ins->dst = new_temp();
        ins->sym = arena_strdup(lower_arena,s->name);
        addr = ins->dst;
        ins = emit(IR_LW);
        ins->dst = new_temp();
        ins->a = addr;
        return ins->dst;
    }
}

static int
addr_sym(struct sym *s)
{
    struct ir_insn *ins;

    if (s->is_local) {
        if (s->is_var_param) {
            ins = emit(IR_LDL);
            ins->dst = new_temp();
            ins->slot = s->slot;
            return ins->dst;
        }
        ins = emit(IR_ADL);
        ins->dst = new_temp();
        ins->slot = s->slot;
        return ins->dst;
    }
    ins = emit(IR_LEA);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena,s->name);
    return ins->dst;
}

static void
store_sym(struct sym *s, int val)
{
    struct ir_insn *ins;

    if (s->is_local) {
        if (s->is_var_param) {
            int addr = new_temp();
            ins = emit(IR_LDL);
            ins->dst = addr;
            ins->slot = s->slot;
            ins = emit(IR_SW);
            ins->a = addr;
            ins->b = val;
        } else {
            ins = emit(IR_STL);
            ins->a = val;
            ins->slot = s->slot;
        }
    } else {
        int addr;
        ins = emit(IR_LEA);
        ins->dst = new_temp();
        ins->sym = arena_strdup(lower_arena,s->name);
        addr = ins->dst;
        ins = emit(IR_SW);
        ins->a = addr;
        ins->b = val;
    }
}

static const char *
with_field_type_name(const char *name)
{
    for (int i = nwith - 1; i >= 0; i--) {
        struct type_def *td = with_stack[i].td;
        for (int j = 0; j < td->nfields; j++)
            if (strcmp(td->fields[j].name, name) == 0)
                return td->fields[j].type_name;
    }
    return NULL;
}

static int
with_field_addr(const char *name)
{
    struct ir_insn *ins;

    for (int i = nwith - 1; i >= 0; i--) {
        struct type_def *td = with_stack[i].td;
        int off = type_field_offset(td, name);
        if (off >= 0) {
            int base;
            ins = emit(IR_LDL);
            ins->dst = new_temp();
            ins->slot = with_stack[i].base_slot;
            base = ins->dst;
            if (off > 0) {
                int c = lower_const(off);
                ins = emit(IR_ADD);
                ins->dst = new_temp();
                ins->a = base;
                ins->b = c;
                return ins->dst;
            }
            return base;
        }
    }
    return -1;
}

static const char *
lvalue_type_name(struct node *n)
{
    if (n->kind == N_NAME) {
        struct sym *s = find_sym(n->name);
        if (s)
            return s->type_name;
        if (nwith > 0)
            return with_field_type_name(n->name);
        return NULL;
    }
    if (n->kind == N_DOT) {
        const char *parent_type = lvalue_type_name(n->a);
        struct type_def *td;
        if (!parent_type)
            return NULL;
        td = find_type(parent_type);
        if (!td)
            return NULL;
        for (int i = 0; i < td->nfields; i++)
            if (strcmp(td->fields[i].name, n->name) == 0)
                return td->fields[i].type_name;
        return NULL;
    }
    if (n->kind == N_INDEX) {
        const char *parent_type = lvalue_type_name(n->a);
        struct type_def *td;
        if (!parent_type)
            return NULL;
        td = find_type(parent_type);
        if (td && td->is_string)
            return "char";
        if (td && td->is_array)
            return td->elem_type_name;
        return NULL;
    }
    return NULL;
}

static int
lower_lvalue_addr(struct node *n)
{
    struct ir_insn *ins;

    if (n->kind == N_NAME) {
        struct sym *s = find_sym(n->name);
        if (!s) {
            if (nwith > 0) {
                int addr = with_field_addr(n->name);
                if (addr >= 0)
                    return addr;
            }
            die("lower:%d: undefined '%s'", n->line, n->name);
        }
        return addr_sym(s);
    }
    if (n->kind == N_DOT) {
        int base = lower_lvalue_addr(n->a);
        const char *parent_tname = lvalue_type_name(n->a);
        struct type_def *td;
        int off;

        if (!parent_tname)
            die("lower:%d: not a record type", n->line);
        td = find_type(parent_tname);
        if (!td)
            die("lower:%d: not a record type", n->line);
        off = type_field_offset(td, n->name);
        if (off < 0)
            die("lower:%d: unknown field '%s'",
                n->line, n->name);
        if (off > 0) {
            int c = lower_const(off);
            ins = emit(IR_ADD);
            ins->dst = new_temp();
            ins->a = base;
            ins->b = c;
            return ins->dst;
        }
        return base;
    }
    if (n->kind == N_INDEX) {
        const char *parent_tname = lvalue_type_name(n->a);
        struct type_def *td;
        int base, idx;

        base = lower_lvalue_addr(n->a);
        if (!parent_tname)
            die("lower:%d: not an array type", n->line);
        td = find_type(parent_tname);
        if (!td)
            die("lower:%d: not an array type", n->line);
        if (td->is_string) {
            idx = lower_expr(n->b);
            ins = emit(IR_ADD);
            ins->dst = new_temp();
            ins->a = base;
            ins->b = idx;
            return ins->dst;
        }
        if (!td->is_array)
            die("lower:%d: not an array type", n->line);
        idx = lower_expr(n->b);
        if (n->op) {
            int lo = lower_const(td->lo_bound);
            int hi = lower_const(td->hi_bound);
            int cmp, labort, lok, abort_args[1];
            labort = new_label();
            lok = new_label();
            cmp = new_temp();
            ins = emit(IR_CMPLTS);
            ins->dst = cmp;
            ins->a = idx;
            ins->b = lo;
            emit_bnz(cmp, labort);
            cmp = new_temp();
            ins = emit(IR_CMPGTS);
            ins->dst = cmp;
            ins->a = idx;
            ins->b = hi;
            emit_bnz(cmp, labort);
            emit_jmp(lok);
            emit_label(labort);
            abort_args[0] = lower_const(134);
            emit_call("exit", abort_args, 1);
            emit_label(lok);
        }
        if (td->lo_bound != 0) {
            int lo = lower_const(td->lo_bound);
            ins = emit(IR_SUB);
            ins->dst = new_temp();
            ins->a = idx;
            ins->b = lo;
            idx = ins->dst;
        }
        if (td->elem_size != 1) {
            int esz = lower_const(td->elem_size);
            ins = emit(IR_MUL);
            ins->dst = new_temp();
            ins->a = idx;
            ins->b = esz;
            idx = ins->dst;
        }
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = base;
        ins->b = idx;
        return ins->dst;
    }
    die("lower:%d: invalid l-value", n->line);
    return 0;
}

/****************************************************************
 * Compile-time constant evaluation
 ****************************************************************/

static long
const_eval(struct node *n)
{
    long l, r;

    if (!n)
        die("lower: null in const expression");

    switch (n->kind) {
    case N_NUM:
    case N_BOOL:
        return n->ival;
    case N_STR:
        if (n->slen == 1)
            return (unsigned char)n->sval[0];
        die("lower:%d: string in const expression", n->line);
        return 0;
    case N_NAME: {
        struct sym *s = find_sym(n->name);
        if (!s || !s->is_const)
            die("lower:%d: '%s' is not a constant",
                n->line, n->name);
        return s->const_val;
    }
    case N_UNOP:
        l = const_eval(n->a);
        if (n->op == T_MINUS)
            return -l;
        if (n->op == T_NOT) {
            if (n->a->kind == N_BOOL)
                return l ? 0 : 1;
            return ~l;
        }
        die("lower:%d: bad unary op in const expr", n->line);
        return 0;
    case N_BINOP:
        l = const_eval(n->a);
        r = const_eval(n->b);
        switch (n->op) {
        case T_PLUS:  return l + r;
        case T_MINUS: return l - r;
        case T_STAR:  return l * r;
        case T_DIV:   return r ? l / r : 0;
        case T_MOD:   return r ? l % r : 0;
        case T_AND:   return l & r;
        case T_OR:    return l | r;
        case T_SHL:   return l << r;
        case T_SHR:   return (unsigned long)l >> r;
        case T_EQ:    return l == r ? 1 : 0;
        case T_NE:    return l != r ? 1 : 0;
        case T_LT:    return l < r ? 1 : 0;
        case T_LE:    return l <= r ? 1 : 0;
        case T_GT:    return l > r ? 1 : 0;
        case T_GE:    return l >= r ? 1 : 0;
        default:
            die("lower:%d: bad binary op in const expr", n->line);
        }
        return 0;
    case N_CALL: {
        const char *fn;
        long a;
        if (n->a->kind != N_NAME)
            die("lower:%d: bad call in const expr", n->line);
        fn = n->a->name;
        if (!n->b)
            die("lower:%d: %s requires argument", n->line, fn);
        a = const_eval(n->b);
        if (strcmp(fn, "ord") == 0)
            return a;
        if (strcmp(fn, "abs") == 0)
            return a < 0 ? -a : a;
        if (strcmp(fn, "sqr") == 0)
            return a * a;
        if (strcmp(fn, "odd") == 0)
            return a & 1;
        if (strcmp(fn, "succ") == 0)
            return a + 1;
        if (strcmp(fn, "pred") == 0)
            return a - 1;
        if (strcmp(fn, "lo") == 0)
            return a & 0xFF;
        if (strcmp(fn, "hi") == 0)
            return (a >> 8) & 0xFF;
        die("lower:%d: unknown function '%s' in const expr",
            n->line, fn);
        return 0;
    }
    case N_SIZEOF: {
        struct type_def *td = find_type(n->name);
        if (!td)
            die("lower:%d: unknown type '%s' in sizeof",
                n->line, n->name);
        return td->total_size;
    }
    default:
        die("lower:%d: not a constant expression", n->line);
    }
    return 0;
}

static int
count_init_list(struct node *n)
{
    int cnt = 0;

    if (n->kind != N_INITLIST)
        return 1;
    for (struct node *e = n->a; e; e = e->next)
        cnt += count_init_list(e);
    return cnt;
}

static void
flatten_init_list(struct node *n, long *iv, int *pos)
{
    if (n->kind != N_INITLIST) {
        iv[(*pos)++] = const_eval(n);
        return;
    }
    for (struct node *e = n->a; e; e = e->next)
        flatten_init_list(e, iv, pos);
}

/****************************************************************
 * IR emission helpers
 ****************************************************************/

static int
bin_op_for_tok(int op)
{
    switch (op) {
    case T_PLUS:  return IR_ADD;
    case T_MINUS: return IR_SUB;
    case T_STAR:  return IR_MUL;
    case T_DIV:   return IR_DIVS;
    case T_MOD:   return IR_MODS;
    case T_AND:   return IR_AND;
    case T_OR:    return IR_OR;
    case T_SHL:   return IR_SHL;
    case T_SHR:   return IR_SHRS;
    case T_EQ:    return IR_CMPEQ;
    case T_NE:    return IR_CMPNE;
    case T_LT:    return IR_CMPLTS;
    case T_LE:    return IR_CMPLES;
    case T_GT:    return IR_CMPGTS;
    case T_GE:    return IR_CMPGES;
    default:      return -1;
    }
}

static int
lower_str_addr(struct node *n)
{
    struct ir_insn *ins;

    if (n->kind == N_NAME) {
        struct sym *s = find_sym(n->name);
        if (!s)
            die("lower:%d: undefined '%s'", n->line, n->name);
        return addr_sym(s);
    }
    if (n->kind == N_STR) {
        char namebuf[32];
        int padded = ((n->slen + 1) + 3) & ~3;
        char *buf = arena_zalloc(lower_arena, padded);
        buf[0] = (char)n->slen;
        memcpy(buf + 1, n->sval, n->slen);
        snprintf(namebuf, sizeof(namebuf), "__str_%d", strctr++);
        add_string_global(namebuf, buf, padded);
        ins = emit(IR_LEA);
        ins->dst = new_temp();
        ins->sym = arena_strdup(lower_arena,namebuf);
        return ins->dst;
    }
    return lower_expr(n);
}

static int
lower_expr(struct node *n)
{
    struct ir_insn *ins;
    int l, r, op;

    if (!n)
        die("lower: null expression");

    switch (n->kind) {
    case N_NUM:
        return lower_const(n->ival);

    case N_BOOL:
        return lower_const(n->ival);

    case N_STR:
        if (n->slen == 1)
            return lower_const((unsigned char)n->sval[0]);
        {
            char namebuf[32];

            snprintf(namebuf, sizeof(namebuf), "__str_%d", strctr++);
            add_string_global(namebuf, n->sval, n->slen + 1);
            ins = emit(IR_LEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena,namebuf);
            return ins->dst;
        }

    case N_NAME: {
        struct sym *s;
        if (strcmp(n->name, "eof") == 0)
            return emit_call_ret("__pascal_eof", NULL, 0);
        s = find_sym(n->name);
        if (!s) {
            if (nwith > 0) {
                int addr = with_field_addr(n->name);
                if (addr >= 0) {
                    ins = emit(IR_LW);
                    ins->dst = new_temp();
                    ins->a = addr;
                    return ins->dst;
                }
            }
            die("lower:%d: undefined '%s'", n->line, n->name);
        }
        if (s->is_func)
            return emit_call_ret(n->name, NULL, 0);
        return load_sym(s);
    }

    case N_DOT:
    case N_INDEX: {
        int addr = lower_lvalue_addr(n);
        const char *ptname = lvalue_type_name(n->a);
        struct type_def *ptd = ptname ? find_type(ptname) : NULL;
        if (ptd && ptd->is_string)
            ins = emit(IR_LB);
        else
            ins = emit(IR_LW);
        ins->dst = new_temp();
        ins->a = addr;
        return ins->dst;
    }

    case N_UNOP:
        if (n->op == T_MINUS) {
            l = lower_expr(n->a);
            ins = emit(IR_NEG);
            ins->dst = new_temp();
            ins->a = l;
            return ins->dst;
        }
        if (n->op == T_NOT) {
            int zero;
            l = lower_expr(n->a);
            zero = lower_const(0);
            ins = emit(IR_CMPEQ);
            ins->dst = new_temp();
            ins->a = l;
            ins->b = zero;
            return ins->dst;
        }
        die("lower:%d: bad unary op", n->line);
        return 0;

    case N_BINOP:
        if (n->op == T_AND_THEN) {
            int result_slot, lfalse, lend, res, c0;
            result_slot = alloc_slot(4);
            l = lower_expr(n->a);
            lfalse = new_label();
            lend = new_label();
            emit_bz(l, lfalse);
            r = lower_expr(n->b);
            ins = emit(IR_STL);
            ins->a = r;
            ins->slot = result_slot;
            emit_jmp(lend);
            emit_label(lfalse);
            c0 = lower_const(0);
            ins = emit(IR_STL);
            ins->a = c0;
            ins->slot = result_slot;
            emit_label(lend);
            res = new_temp();
            ins = emit(IR_LDL);
            ins->dst = res;
            ins->slot = result_slot;
            return res;
        }
        if (n->op == T_OR_ELSE) {
            int result_slot, ltrue, lend, res, c1;
            result_slot = alloc_slot(4);
            l = lower_expr(n->a);
            ltrue = new_label();
            lend = new_label();
            emit_bnz(l, ltrue);
            r = lower_expr(n->b);
            ins = emit(IR_STL);
            ins->a = r;
            ins->slot = result_slot;
            emit_jmp(lend);
            emit_label(ltrue);
            c1 = lower_const(1);
            ins = emit(IR_STL);
            ins->a = c1;
            ins->slot = result_slot;
            emit_label(lend);
            res = new_temp();
            ins = emit(IR_LDL);
            ins->dst = res;
            ins->slot = result_slot;
            return res;
        }
        if (n->op == T_IN) {
            int args[2];
            args[0] = lower_expr(n->a);
            args[1] = lower_set_addr(n->b);
            return emit_call_ret("__pascal_set_in",
                                 args, 2);
        }
        if (n->op == T_PLUS) {
            if (is_set_type(n->a) || is_set_type(n->b)) {
                int slot, args[3], dst;
                slot = alloc_slot(32);
                ins = emit(IR_ADL);
                ins->dst = new_temp();
                ins->slot = slot;
                dst = ins->dst;
                args[0] = dst;
                args[1] = lower_set_addr(n->a);
                args[2] = lower_set_addr(n->b);
                emit_call("__pascal_set_union",
                          args, 3);
                return dst;
            }
        }
        if (n->op == T_STAR) {
            if (is_set_type(n->a) || is_set_type(n->b)) {
                int slot, args[3], dst;
                slot = alloc_slot(32);
                ins = emit(IR_ADL);
                ins->dst = new_temp();
                ins->slot = slot;
                dst = ins->dst;
                args[0] = dst;
                args[1] = lower_set_addr(n->a);
                args[2] = lower_set_addr(n->b);
                emit_call("__pascal_set_intersect",
                          args, 3);
                return dst;
            }
        }
        if (n->op == T_MINUS) {
            if (is_set_type(n->a) || is_set_type(n->b)) {
                int slot, args[3], dst;
                slot = alloc_slot(32);
                ins = emit(IR_ADL);
                ins->dst = new_temp();
                ins->slot = slot;
                dst = ins->dst;
                args[0] = dst;
                args[1] = lower_set_addr(n->a);
                args[2] = lower_set_addr(n->b);
                emit_call("__pascal_set_diff",
                          args, 3);
                return dst;
            }
        }
        if (n->op == T_PLUS) {
            int is_str = 0;
            if (is_string_type(n->a) || is_string_type(n->b))
                is_str = 1;
            else if (n->a->kind == N_STR && n->a->slen != 1)
                is_str = 1;
            else if (n->b->kind == N_STR && n->b->slen != 1)
                is_str = 1;
            if (is_str) {
                int slot, args[4], dst;
                slot = alloc_slot(256);
                ins = emit(IR_ADL);
                ins->dst = new_temp();
                ins->slot = slot;
                dst = ins->dst;
                args[0] = dst;
                args[1] = lower_str_addr(n->a);
                args[2] = lower_str_addr(n->b);
                args[3] = lower_const(255);
                emit_call("__pascal_str_concat",
                          args, 4);
                return dst;
            }
        }
        if (n->op >= T_EQ && n->op <= T_GE) {
            if (is_set_type(n->a) || is_set_type(n->b)) {
                int args[2];
                const char *fn = NULL;
                args[0] = lower_set_addr(n->a);
                args[1] = lower_set_addr(n->b);
                switch (n->op) {
                case T_EQ:
                    fn = "__pascal_set_eq";
                    break;
                case T_NE:
                    fn = "__pascal_set_ne";
                    break;
                case T_LE:
                    fn = "__pascal_set_subset";
                    break;
                case T_GE:
                    fn = "__pascal_set_subset";
                    /* swap: a >= b means b subset of a */
                    { int tmp = args[0]; args[0] = args[1]; args[1] = tmp; }
                    break;
                default:
                    die("lower:%d: unsupported set comparison", n->line);
                }
                return emit_call_ret(fn, args, 2);
            }
        }
        if (n->op >= T_EQ && n->op <= T_GE) {
            int is_str = 0;
            if (is_string_type(n->a) || is_string_type(n->b))
                is_str = 1;
            else if (n->a->kind == N_STR && n->a->slen != 1)
                is_str = 1;
            else if (n->b->kind == N_STR && n->b->slen != 1)
                is_str = 1;
            if (is_str) {
                int args[2], cmp, zero;
                args[0] = lower_str_addr(n->a);
                args[1] = lower_str_addr(n->b);
                cmp = emit_call_ret("__pascal_str_compare",
                                    args, 2);
                zero = lower_const(0);
                op = bin_op_for_tok(n->op);
                ins = emit(op);
                ins->dst = new_temp();
                ins->a = cmp;
                ins->b = zero;
                return ins->dst;
            }
        }
        l = lower_expr(n->a);
        r = lower_expr(n->b);
        op = bin_op_for_tok(n->op);
        if (op < 0)
            die("lower:%d: bad binary op", n->line);
        ins = emit(op);
        ins->dst = new_temp();
        ins->a = l;
        ins->b = r;
        if (n->ival && (op == IR_ADD || op == IR_SUB ||
                        op == IR_MUL)) {
            int result, t1, t2, t3, zero, cmp;
            int labort, lok, abort_args[1];
            result = ins->dst;
            labort = new_label();
            lok = new_label();
            if (op == IR_ADD) {
                t1 = new_temp();
                ins = emit(IR_XOR);
                ins->dst = t1;
                ins->a = l;
                ins->b = result;
                t2 = new_temp();
                ins = emit(IR_XOR);
                ins->dst = t2;
                ins->a = r;
                ins->b = result;
                t3 = new_temp();
                ins = emit(IR_AND);
                ins->dst = t3;
                ins->a = t1;
                ins->b = t2;
            } else if (op == IR_SUB) {
                t1 = new_temp();
                ins = emit(IR_XOR);
                ins->dst = t1;
                ins->a = l;
                ins->b = r;
                t2 = new_temp();
                ins = emit(IR_XOR);
                ins->dst = t2;
                ins->a = l;
                ins->b = result;
                t3 = new_temp();
                ins = emit(IR_AND);
                ins->dst = t3;
                ins->a = t1;
                ins->b = t2;
            } else {
                /* MUL: overflow if a != 0 && result / a != b */
                int lnomul, div_res;
                lnomul = new_label();
                zero = lower_const(0);
                cmp = new_temp();
                ins = emit(IR_CMPEQ);
                ins->dst = cmp;
                ins->a = l;
                ins->b = zero;
                emit_bnz(cmp, lnomul);
                div_res = new_temp();
                ins = emit(IR_DIVS);
                ins->dst = div_res;
                ins->a = result;
                ins->b = l;
                cmp = new_temp();
                ins = emit(IR_CMPNE);
                ins->dst = cmp;
                ins->a = div_res;
                ins->b = r;
                emit_bnz(cmp, labort);
                emit_label(lnomul);
                emit_jmp(lok);
                emit_label(labort);
                abort_args[0] = lower_const(134);
                emit_call("exit", abort_args, 1);
                emit_label(lok);
                return result;
            }
            zero = lower_const(0);
            cmp = new_temp();
            ins = emit(IR_CMPLTS);
            ins->dst = cmp;
            ins->a = t3;
            ins->b = zero;
            emit_bnz(cmp, labort);
            emit_jmp(lok);
            emit_label(labort);
            abort_args[0] = lower_const(134);
            emit_call("exit", abort_args, 1);
            emit_label(lok);
            return result;
        }
        return ins->dst;

    case N_CALL:
        return lower_call(n);

    case N_SET: {
        int slot, dst;
        struct node *e;

        slot = alloc_slot(32);
        ins = emit(IR_ADL);
        ins->dst = new_temp();
        ins->slot = slot;
        dst = ins->dst;
        {
            int args[1];
            args[0] = dst;
            emit_call("__pascal_set_clear", args, 1);
        }
        for (e = n->a; e; e = e->next) {
            if (e->kind == N_SETRANGE) {
                int args[3];
                args[0] = dst;
                args[1] = lower_expr(e->a);
                args[2] = lower_expr(e->b);
                emit_call("__pascal_set_addrange",
                          args, 3);
            } else {
                int args[2];
                args[0] = dst;
                args[1] = lower_expr(e);
                emit_call("__pascal_set_add", args, 2);
            }
        }
        return dst;
    }

    case N_SIZEOF: {
        struct type_def *td = find_type(n->name);
        if (!td)
            die("lower:%d: unknown type '%s' in sizeof",
                n->line, n->name);
        ins = emit(IR_LIC);
        ins->dst = new_temp();
        ins->imm = td->total_size;
        return ins->dst;
    }

    default:
        die("lower:%d: unhandled expr kind %d", n->line, n->kind);
    }
    return 0;
}

/****************************************************************
 * Write / writeln builtin lowering
 ****************************************************************/

static void
write_str_literal(const char *str, int slen)
{
    char namebuf[32];
    int args[3];
    struct ir_insn *lea;
    int fd, clen;

    snprintf(namebuf, sizeof(namebuf), "__str_%d", strctr++);
    add_string_global(namebuf, str, slen + 1);
    lea = emit(IR_LEA);
    lea->dst = new_temp();
    lea->sym = arena_strdup(lower_arena,namebuf);
    fd = lower_const(1);
    clen = lower_const(slen);
    args[0] = fd;
    args[1] = lea->dst;
    args[2] = clen;
    emit_call("write", args, 3);
}

static int
is_char_call(struct node *n)
{
    if (n->kind != N_CALL || !n->a || n->a->kind != N_NAME)
        return 0;
    return strcmp(n->a->name, "chr") == 0 ||
           strcmp(n->a->name, "upcase") == 0 ||
           strcmp(n->a->name, "lowercase") == 0;
}

static int
is_char_type(struct node *n)
{
    const char *tn = lvalue_type_name(n);
    return tn && strcmp(tn, "char") == 0;
}

static int
is_string_type(struct node *n)
{
    if (n->kind == N_NAME) {
        struct sym *s = find_sym(n->name);
        if (s && s->type_name) {
            struct type_def *td = find_type(s->type_name);
            return td && td->is_string;
        }
    }
    return 0;
}

static int
is_set_type(struct node *n)
{
    if (n->kind == N_NAME) {
        struct sym *s = find_sym(n->name);
        if (s && s->type_name) {
            struct type_def *td = find_type(s->type_name);
            return td && td->is_set;
        }
    }
    if (n->kind == N_SET)
        return 1;
    return 0;
}

static int
lower_set_addr(struct node *n)
{
    if (n->kind == N_NAME) {
        struct sym *s = find_sym(n->name);
        if (!s)
            die("lower:%d: undefined '%s'", n->line, n->name);
        return addr_sym(s);
    }
    return lower_expr(n);
}

static void
lower_write_arg(struct node *arg)
{
    if (arg->kind == N_STR) {
        write_str_literal(arg->sval, arg->slen);
    } else if (arg->kind == N_NAME && is_string_type(arg)) {
        struct sym *s = find_sym(arg->name);
        int args[1];
        args[0] = addr_sym(s);
        emit_call("__pascal_write_str", args, 1);
    } else if (arg->kind == N_DOT || arg->kind == N_INDEX) {
        const char *tn = lvalue_type_name(arg);
        struct type_def *td = tn ? find_type(tn) : NULL;
        if (td && td->is_string) {
            int args[1];
            args[0] = lower_lvalue_addr(arg);
            emit_call("__pascal_write_str", args, 1);
        } else {
            const char *ptname = lvalue_type_name(arg->a);
            struct type_def *ptd =
                ptname ? find_type(ptname) : NULL;
            if (ptd && ptd->is_string) {
                int args[1];
                args[0] = lower_expr(arg);
                emit_call("__pascal_write_char", args, 1);
            } else if (is_char_type(arg)) {
                int args[1];
                args[0] = lower_expr(arg);
                emit_call("__pascal_write_char", args, 1);
            } else {
                int args[1];
                args[0] = lower_expr(arg);
                emit_call("__pascal_write_int", args, 1);
            }
        }
    } else if (arg->kind == N_NAME) {
        struct sym *s = find_sym(arg->name);
        if (s && s->is_const && s->const_str) {
            write_str_literal(s->const_str, s->const_slen);
        } else if (is_char_type(arg)) {
            int args[1];
            args[0] = lower_expr(arg);
            emit_call("__pascal_write_char", args, 1);
        } else {
            int args[1];
            args[0] = lower_expr(arg);
            emit_call("__pascal_write_int", args, 1);
        }
    } else if (is_char_call(arg) || is_char_type(arg)) {
        int args[1];
        args[0] = lower_expr(arg);
        emit_call("__pascal_write_char", args, 1);
    } else {
        int args[1];
        args[0] = lower_expr(arg);
        emit_call("__pascal_write_int", args, 1);
    }
}

static void
lower_write(struct node *n, int newline)
{
    struct node *arg;

    for (arg = n->b; arg; arg = arg->next)
        lower_write_arg(arg);
    if (newline) {
        int args[3];
        char namebuf[32];
        snprintf(namebuf, sizeof(namebuf), "__str_%d", strctr++);
        add_string_global(namebuf, "\n", 2);
        {
            struct ir_insn *lea = emit(IR_LEA);
            lea->dst = new_temp();
            lea->sym = arena_strdup(lower_arena,namebuf);
            args[0] = lower_const(1);
            args[1] = lea->dst;
            args[2] = lower_const(1);
        }
        emit_call("write", args, 3);
    }
}

/****************************************************************
 * Call lowering
 ****************************************************************/

static int
lower_call(struct node *n)
{
    struct ir_insn *ins;
    const char *name;
    struct node *arg;
    int args[16];
    int nargs;

    if (n->a->kind != N_NAME)
        die("lower:%d: indirect call not supported", n->line);
    name = n->a->name;

    /* user-defined procedures/functions take priority over builtins */
    {
        struct sym *check = find_sym(name);
        if (check && check->is_func)
            goto user_call;
    }

    if (strcmp(name, "halt") == 0) {
        if (n->b)
            args[0] = lower_expr(n->b);
        else
            args[0] = lower_const(0);
        emit_call("exit", args, 1);
        return 0;
    }

    if (strcmp(name, "exit") == 0) {
        if (n->b && cur_func_name) {
            int val = lower_expr(n->b);
            ins = emit(IR_STL);
            ins->a = val;
            ins->slot = ret_slot;
        }
        emit_jmp(exit_label);
        return 0;
    }

    if (strcmp(name, "write") == 0) {
        lower_write(n, 0);
        return 0;
    }
    if (strcmp(name, "writeln") == 0) {
        lower_write(n, 1);
        return 0;
    }

    if (strcmp(name, "read") == 0 ||
        strcmp(name, "readln") == 0) {
        struct node *a;
        int is_readln = (name[4] == 'l');
        int read_str = 0;
        for (a = n->b; a; a = a->next) {
            int addr = 0;
            if (a->kind == N_NAME) {
                struct sym *s = find_sym(a->name);
                if (!s)
                    die("lower:%d: undefined '%s'",
                        n->line, a->name);
                addr = addr_sym(s);
            } else if (a->kind == N_DOT ||
                       a->kind == N_INDEX) {
                addr = lower_lvalue_addr(a);
            } else {
                die("lower:%d: read requires a variable",
                    n->line);
            }
            if (is_string_type(a)) {
                struct sym *s = find_sym(a->name);
                struct type_def *td =
                    find_type(s->type_name);
                int rargs[2];
                rargs[0] = addr;
                rargs[1] = lower_const(td->maxlen);
                emit_call("__pascal_read_str", rargs, 2);
                read_str = 1;
            } else {
                const char *rt_fn =
                    is_char_type(a) ?
                    "__pascal_read_char" :
                    "__pascal_read_int";
                int rargs[1];
                rargs[0] = addr;
                emit_call(rt_fn, rargs, 1);
            }
        }
        if (is_readln && !read_str)
            emit_call("__pascal_readln", NULL, 0);
        return 0;
    }

    /* eof — check for end of input */
    if (strcmp(name, "eof") == 0)
        return emit_call_ret("__pascal_eof", NULL, 0);

    /* str(value, s) — integer to string */
    if (strcmp(name, "str") == 0) {
        struct node *val_node, *dst_node;
        struct sym *ds;
        struct type_def *dtd;
        int sargs[3];
        if (!n->b || !n->b->next)
            die("lower:%d: str requires two arguments",
                n->line);
        val_node = n->b;
        dst_node = n->b->next;
        if (dst_node->kind != N_NAME)
            die("lower:%d: str destination must be a "
                "variable", n->line);
        ds = find_sym(dst_node->name);
        if (!ds || !ds->type_name)
            die("lower:%d: str destination must be a "
                "string", n->line);
        dtd = find_type(ds->type_name);
        if (!dtd || !dtd->is_string)
            die("lower:%d: str destination must be a "
                "string", n->line);
        sargs[0] = lower_expr(val_node);
        sargs[1] = addr_sym(ds);
        sargs[2] = lower_const(dtd->maxlen);
        emit_call("__pascal_str_from_int", sargs, 3);
        return 0;
    }

    /* ord — identity on integers */
    if (strcmp(name, "ord") == 0) {
        if (!n->b)
            die("lower:%d: ord requires argument", n->line);
        return lower_expr(n->b);
    }

    /* integer(x) — identity cast */
    if (strcmp(name, "integer") == 0) {
        if (!n->b)
            die("lower:%d: integer() requires argument", n->line);
        return lower_expr(n->b);
    }

    /* chr(x), char(x), byte(x) — mask to 8 bits */
    if (strcmp(name, "chr") == 0 ||
        strcmp(name, "char") == 0 || strcmp(name, "byte") == 0) {
        int val, cmask;
        if (!n->b)
            die("lower:%d: %s() requires argument", n->line, name);
        val = lower_expr(n->b);
        cmask = lower_const(0xFF);
        ins = emit(IR_AND);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = cmask;
        return ins->dst;
    }

    /* word(x) — mask to 16 bits */
    if (strcmp(name, "word") == 0) {
        int val, cmask;
        if (!n->b)
            die("lower:%d: word() requires argument", n->line);
        val = lower_expr(n->b);
        cmask = lower_const(0xFFFF);
        ins = emit(IR_AND);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = cmask;
        return ins->dst;
    }

    /* abs(x) — branchless: mask=x>>31, (x^mask)-mask */
    if (strcmp(name, "abs") == 0) {
        int val, c31, mask, xored;
        if (!n->b)
            die("lower:%d: abs requires argument", n->line);
        val = lower_expr(n->b);
        c31 = lower_const(31);
        ins = emit(IR_SHRS);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = c31;
        mask = ins->dst;
        ins = emit(IR_XOR);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = mask;
        xored = ins->dst;
        ins = emit(IR_SUB);
        ins->dst = new_temp();
        ins->a = xored;
        ins->b = mask;
        return ins->dst;
    }

    /* odd(x) — x AND 1 */
    if (strcmp(name, "odd") == 0) {
        int val, c1;
        if (!n->b)
            die("lower:%d: odd requires argument", n->line);
        val = lower_expr(n->b);
        c1 = lower_const(1);
        ins = emit(IR_AND);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = c1;
        return ins->dst;
    }

    /* succ(x) — x + 1 */
    if (strcmp(name, "succ") == 0) {
        int val, c1;
        if (!n->b)
            die("lower:%d: succ requires argument", n->line);
        val = lower_expr(n->b);
        c1 = lower_const(1);
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = c1;
        return ins->dst;
    }

    /* pred(x) — x - 1 */
    if (strcmp(name, "pred") == 0) {
        int val, c1;
        if (!n->b)
            die("lower:%d: pred requires argument", n->line);
        val = lower_expr(n->b);
        c1 = lower_const(1);
        ins = emit(IR_SUB);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = c1;
        return ins->dst;
    }

    /* length(s) — string length */
    if (strcmp(name, "length") == 0) {
        if (!n->b)
            die("lower:%d: length requires argument", n->line);
        if (n->b->kind == N_STR)
            return lower_const(n->b->slen);
        if (n->b->kind == N_NAME && is_string_type(n->b)) {
            struct sym *s = find_sym(n->b->name);
            int addr = addr_sym(s);
            ins = emit(IR_LB);
            ins->dst = new_temp();
            ins->a = addr;
            return ins->dst;
        }
        if (n->b->kind == N_DOT || n->b->kind == N_INDEX) {
            const char *tn = lvalue_type_name(n->b);
            struct type_def *td = tn ? find_type(tn) : NULL;
            if (td && td->is_string) {
                int addr = lower_lvalue_addr(n->b);
                ins = emit(IR_LB);
                ins->dst = new_temp();
                ins->a = addr;
                return ins->dst;
            }
        }
        die("lower:%d: length requires string argument",
            n->line);
    }

    /* copy(s, index, count) — substring */
    if (strcmp(name, "copy") == 0) {
        struct node *a1, *a2, *a3;
        int slot, dst, cargs[5];
        if (!n->b || !n->b->next || !n->b->next->next)
            die("lower:%d: copy requires 3 arguments", n->line);
        a1 = n->b;
        a2 = n->b->next;
        a3 = n->b->next->next;
        slot = alloc_slot(256);
        ins = emit(IR_ADL);
        ins->dst = new_temp();
        ins->slot = slot;
        dst = ins->dst;
        cargs[0] = dst;
        cargs[1] = lower_str_addr(a1);
        cargs[2] = lower_expr(a2);
        cargs[3] = lower_expr(a3);
        cargs[4] = lower_const(255);
        emit_call("__pascal_str_copy", cargs, 5);
        return dst;
    }

    /* pos(substr, str) — find substring position */
    if (strcmp(name, "pos") == 0) {
        int pargs[2];
        if (!n->b || !n->b->next)
            die("lower:%d: pos requires 2 arguments", n->line);
        pargs[0] = lower_str_addr(n->b);
        pargs[1] = lower_str_addr(n->b->next);
        return emit_call_ret("__pascal_str_pos", pargs, 2);
    }

    /* delete(s, index, count) — remove characters in place */
    if (strcmp(name, "delete") == 0) {
        struct node *a1, *a2, *a3;
        int dargs[3];
        if (!n->b || !n->b->next || !n->b->next->next)
            die("lower:%d: delete requires 3 arguments",
                n->line);
        a1 = n->b;
        a2 = n->b->next;
        a3 = n->b->next->next;
        if (a1->kind != N_NAME)
            die("lower:%d: delete requires a variable",
                n->line);
        dargs[0] = lower_str_addr(a1);
        dargs[1] = lower_expr(a2);
        dargs[2] = lower_expr(a3);
        emit_call("__pascal_str_delete", dargs, 3);
        return 0;
    }

    /* insert(source, dest, index) — insert string in place */
    if (strcmp(name, "insert") == 0) {
        struct node *src_node, *dst_node, *idx_node;
        struct sym *ds;
        struct type_def *dtd;
        int iargs[4];
        if (!n->b || !n->b->next || !n->b->next->next)
            die("lower:%d: insert requires 3 arguments",
                n->line);
        src_node = n->b;
        dst_node = n->b->next;
        idx_node = n->b->next->next;
        if (dst_node->kind != N_NAME)
            die("lower:%d: insert dest must be a variable",
                n->line);
        ds = find_sym(dst_node->name);
        if (!ds || !ds->type_name)
            die("lower:%d: insert dest must be a string",
                n->line);
        dtd = find_type(ds->type_name);
        if (!dtd || !dtd->is_string)
            die("lower:%d: insert dest must be a string",
                n->line);
        iargs[0] = lower_str_addr(src_node);
        iargs[1] = addr_sym(ds);
        iargs[2] = lower_expr(idx_node);
        iargs[3] = lower_const(dtd->maxlen);
        emit_call("__pascal_str_insert", iargs, 4);
        return 0;
    }

    /* concat(s1, s2, ...) — concatenate multiple strings */
    if (strcmp(name, "concat") == 0) {
        struct node *a;
        int slot, dst, prev;
        if (!n->b || !n->b->next)
            die("lower:%d: concat requires at least 2 arguments",
                n->line);
        slot = alloc_slot(256);
        ins = emit(IR_ADL);
        ins->dst = new_temp();
        ins->slot = slot;
        dst = ins->dst;
        a = n->b;
        prev = lower_str_addr(a);
        a = a->next;
        while (a) {
            int cargs[4];
            cargs[0] = dst;
            cargs[1] = prev;
            cargs[2] = lower_str_addr(a);
            cargs[3] = lower_const(255);
            emit_call("__pascal_str_concat", cargs, 4);
            prev = dst;
            a = a->next;
        }
        return dst;
    }

    /* sqr(x) — x * x */
    if (strcmp(name, "sqr") == 0) {
        int val;
        if (!n->b)
            die("lower:%d: sqr requires argument", n->line);
        val = lower_expr(n->b);
        ins = emit(IR_MUL);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = val;
        return ins->dst;
    }

    /* lo(x) — x AND $FF */
    if (strcmp(name, "lo") == 0) {
        int val, cff;
        if (!n->b)
            die("lower:%d: lo requires argument", n->line);
        val = lower_expr(n->b);
        cff = lower_const(0xFF);
        ins = emit(IR_AND);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = cff;
        return ins->dst;
    }

    /* hi(x) — (x SHR 8) AND $FF */
    if (strcmp(name, "hi") == 0) {
        int val, c8, shifted, cff;
        if (!n->b)
            die("lower:%d: hi requires argument", n->line);
        val = lower_expr(n->b);
        c8 = lower_const(8);
        ins = emit(IR_SHRS);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = c8;
        shifted = ins->dst;
        cff = lower_const(0xFF);
        ins = emit(IR_AND);
        ins->dst = new_temp();
        ins->a = shifted;
        ins->b = cff;
        return ins->dst;
    }

    /* swap(w) — exchange high and low bytes of a word */
    if (strcmp(name, "swap") == 0) {
        int val, c8, cff, lo, hi, shifted;
        if (!n->b)
            die("lower:%d: swap requires argument", n->line);
        val = lower_expr(n->b);
        c8 = lower_const(8);
        cff = lower_const(0xFF);
        ins = emit(IR_AND);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = cff;
        lo = ins->dst;
        ins = emit(IR_SHL);
        ins->dst = new_temp();
        ins->a = lo;
        ins->b = c8;
        shifted = ins->dst;
        ins = emit(IR_SHRS);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = c8;
        hi = ins->dst;
        ins = emit(IR_AND);
        ins->dst = new_temp();
        ins->a = hi;
        ins->b = cff;
        hi = ins->dst;
        ins = emit(IR_OR);
        ins->dst = new_temp();
        ins->a = shifted;
        ins->b = hi;
        return ins->dst;
    }

    /* upcase(ch) — lowercase to uppercase */
    if (strcmp(name, "upcase") == 0) {
        int val, ca, cz, c32, cmp_lo, cmp_hi, both, upper;
        int lskip, result_slot;
        if (!n->b)
            die("lower:%d: upcase requires argument", n->line);
        val = lower_expr(n->b);
        ca = lower_const('a');
        cz = lower_const('z');
        c32 = lower_const(32);
        ins = emit(IR_CMPLTS);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = ca;
        cmp_lo = ins->dst;
        ins = emit(IR_CMPGTS);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = cz;
        cmp_hi = ins->dst;
        ins = emit(IR_OR);
        ins->dst = new_temp();
        ins->a = cmp_lo;
        ins->b = cmp_hi;
        both = ins->dst;
        result_slot = alloc_slot(4);
        ins = emit(IR_STL);
        ins->a = val;
        ins->slot = result_slot;
        lskip = new_label();
        emit_bnz(both, lskip);
        ins = emit(IR_SUB);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = c32;
        upper = ins->dst;
        ins = emit(IR_STL);
        ins->a = upper;
        ins->slot = result_slot;
        emit_label(lskip);
        ins = emit(IR_LDL);
        ins->dst = new_temp();
        ins->slot = result_slot;
        return ins->dst;
    }

    /* lowercase(ch) — uppercase to lowercase */
    if (strcmp(name, "lowercase") == 0) {
        int val, ca, cz, c32, cmp_lo, cmp_hi, both, low;
        int lskip, result_slot;
        if (!n->b)
            die("lower:%d: lowercase requires argument", n->line);
        val = lower_expr(n->b);
        ca = lower_const('A');
        cz = lower_const('Z');
        c32 = lower_const(32);
        ins = emit(IR_CMPLTS);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = ca;
        cmp_lo = ins->dst;
        ins = emit(IR_CMPGTS);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = cz;
        cmp_hi = ins->dst;
        ins = emit(IR_OR);
        ins->dst = new_temp();
        ins->a = cmp_lo;
        ins->b = cmp_hi;
        both = ins->dst;
        result_slot = alloc_slot(4);
        ins = emit(IR_STL);
        ins->a = val;
        ins->slot = result_slot;
        lskip = new_label();
        emit_bnz(both, lskip);
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = val;
        ins->b = c32;
        low = ins->dst;
        ins = emit(IR_STL);
        ins->a = low;
        ins->slot = result_slot;
        emit_label(lskip);
        ins = emit(IR_LDL);
        ins->dst = new_temp();
        ins->slot = result_slot;
        return ins->dst;
    }

    /* inc(x) / inc(x, n) */
    if (strcmp(name, "inc") == 0) {
        struct sym *s;
        int cur_val, amount;
        if (!n->b || n->b->kind != N_NAME)
            die("lower:%d: inc requires a variable", n->line);
        s = find_sym(n->b->name);
        if (!s)
            die("lower:%d: undefined '%s'", n->line, n->b->name);
        cur_val = load_sym(s);
        amount = n->b->next ? lower_expr(n->b->next) :
                               lower_const(1);
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = cur_val;
        ins->b = amount;
        store_sym(s, ins->dst);
        return 0;
    }

    /* dec(x) / dec(x, n) */
    if (strcmp(name, "dec") == 0) {
        struct sym *s;
        int cur_val, amount;
        if (!n->b || n->b->kind != N_NAME)
            die("lower:%d: dec requires a variable", n->line);
        s = find_sym(n->b->name);
        if (!s)
            die("lower:%d: undefined '%s'", n->line, n->b->name);
        cur_val = load_sym(s);
        amount = n->b->next ? lower_expr(n->b->next) :
                               lower_const(1);
        ins = emit(IR_SUB);
        ins->dst = new_temp();
        ins->a = cur_val;
        ins->b = amount;
        store_sym(s, ins->dst);
        return 0;
    }

    /* val(s: string; var v: integer; var code: integer) */
    if (strcmp(name, "val") == 0) {
        struct node *a1, *a2, *a3;
        struct sym *vs, *cs;
        int saddr, vaddr, caddr, fargs[3];
        if (!n->b || !n->b->next || !n->b->next->next)
            die("lower:%d: val requires 3 arguments",
                n->line);
        a1 = n->b;
        a2 = n->b->next;
        a3 = n->b->next->next;
        saddr = lower_str_addr(a1);
        if (a2->kind != N_NAME)
            die("lower:%d: val result must be a variable",
                n->line);
        vs = find_sym(a2->name);
        if (!vs)
            die("lower:%d: undefined '%s'",
                n->line, a2->name);
        vaddr = addr_sym(vs);
        if (a3->kind != N_NAME)
            die("lower:%d: val code must be a variable",
                n->line);
        cs = find_sym(a3->name);
        if (!cs)
            die("lower:%d: undefined '%s'",
                n->line, a3->name);
        caddr = addr_sym(cs);
        fargs[0] = saddr;
        fargs[1] = vaddr;
        fargs[2] = caddr;
        emit_call("__pascal_val", fargs, 3);
        return 0;
    }

    /* fillchar(var x; count: integer; value: byte) */
    if (strcmp(name, "fillchar") == 0) {
        int addr, count, val, fargs[3];
        struct node *a1, *a2, *a3;
        if (!n->b || !n->b->next || !n->b->next->next)
            die("lower:%d: fillchar requires 3 arguments",
                n->line);
        a1 = n->b;
        a2 = n->b->next;
        a3 = n->b->next->next;
        if (a1->kind == N_NAME) {
            struct sym *s = find_sym(a1->name);
            if (!s)
                die("lower:%d: undefined '%s'",
                    n->line, a1->name);
            addr = addr_sym(s);
        } else if (a1->kind == N_DOT ||
                   a1->kind == N_INDEX) {
            addr = lower_lvalue_addr(a1);
        } else {
            die("lower:%d: fillchar requires a variable",
                n->line);
            addr = 0;
        }
        count = lower_expr(a2);
        val = lower_expr(a3);
        fargs[0] = addr;
        fargs[1] = val;
        fargs[2] = count;
        emit_call("memset", fargs, 3);
        return 0;
    }

    /* move(var source; var dest; count: integer) */
    if (strcmp(name, "move") == 0) {
        int src, dst, count, fargs[3];
        struct node *a1, *a2, *a3;
        if (!n->b || !n->b->next || !n->b->next->next)
            die("lower:%d: move requires 3 arguments",
                n->line);
        a1 = n->b;
        a2 = n->b->next;
        a3 = n->b->next->next;
        if (a1->kind == N_NAME) {
            struct sym *s = find_sym(a1->name);
            if (!s)
                die("lower:%d: undefined '%s'",
                    n->line, a1->name);
            src = addr_sym(s);
        } else if (a1->kind == N_DOT ||
                   a1->kind == N_INDEX) {
            src = lower_lvalue_addr(a1);
        } else {
            die("lower:%d: move requires a variable",
                n->line);
            src = 0;
        }
        if (a2->kind == N_NAME) {
            struct sym *s = find_sym(a2->name);
            if (!s)
                die("lower:%d: undefined '%s'",
                    n->line, a2->name);
            dst = addr_sym(s);
        } else if (a2->kind == N_DOT ||
                   a2->kind == N_INDEX) {
            dst = lower_lvalue_addr(a2);
        } else {
            die("lower:%d: move requires a variable",
                n->line);
            dst = 0;
        }
        count = lower_expr(a3);
        fargs[0] = dst;
        fargs[1] = src;
        fargs[2] = count;
        emit_call("memmove", fargs, 3);
        return 0;
    }

    /* look up callee to check for var/const/compound parameters */
user_call:
    {
        struct sym *callee = find_sym(name);
        struct node *param = callee && callee->decl ?
                             callee->decl->a : NULL;
        const char *call_name = name;

        if (callee && callee->ir_name)
            call_name = callee->ir_name;

        nargs = 0;

        /* hidden capture arguments for nested functions */
        if (callee && callee->ncaptures > 0) {
            for (int i = 0; i < callee->ncaptures; i++) {
                struct sym *vs;
                if (nargs >= 16)
                    die("lower: too many args");
                vs = find_sym(callee->captures[i]);
                if (!vs)
                    die("lower:%d: capture '%s' "
                        "not found",
                        n->line,
                        callee->captures[i]);
                args[nargs++] = addr_sym(vs);
            }
        }

        for (arg = n->b; arg; arg = arg->next) {
            int pass_addr = 0;
            if (nargs >= 16)
                die("lower: too many args");
            if (param && param->op == 1)
                pass_addr = 1;
            else if (param && param->sval &&
                     find_type(param->sval))
                pass_addr = 1;
            if (pass_addr) {
                if (arg->kind == N_NAME) {
                    struct sym *vs;
                    vs = find_sym(arg->name);
                    if (!vs)
                        die("lower:%d: undefined "
                            "variable '%s'",
                            n->line, arg->name);
                    args[nargs++] = addr_sym(vs);
                } else if (arg->kind == N_DOT ||
                           arg->kind == N_INDEX) {
                    args[nargs++] =
                        lower_lvalue_addr(arg);
                } else {
                    die("lower:%d: parameter "
                        "requires a variable",
                        n->line);
                }
            } else {
                args[nargs++] = lower_expr(arg);
            }
            if (param)
                param = param->next;
        }
        return emit_call_ret(call_name, args, nargs);
    }
}

/****************************************************************
 * Statement lowering
 ****************************************************************/

static void
lower_stmts(struct node *n)
{
    for (; n; n = n->next)
        lower_stmt(n);
}

static void
lower_stmt(struct node *n)
{
    struct ir_insn *ins;
    int ltrue, lfalse, lend, ltop, lbrk;

    if (!n)
        return;

    switch (n->kind) {
    case N_COMPOUND:
        lower_stmts(n->a);
        return;

    case N_ASSIGN: {
        int val;
        struct sym *s;
        const char *lhs_name;

        if (n->a->kind == N_DOT || n->a->kind == N_INDEX) {
            const char *tn = lvalue_type_name(n->a);
            struct type_def *td =
                tn ? find_type(tn) : NULL;
            if (td && td->is_string) {
                int dst = lower_lvalue_addr(n->a);
                int src = lower_str_addr(n->b);
                int maxl = lower_const(td->maxlen);
                int sargs[3];
                sargs[0] = dst;
                sargs[1] = src;
                sargs[2] = maxl;
                emit_call("__pascal_str_assign",
                          sargs, 3);
                return;
            }
            {
                const char *ptname =
                    lvalue_type_name(n->a->a);
                struct type_def *ptd =
                    ptname ? find_type(ptname) : NULL;
                int addr = lower_lvalue_addr(n->a);
                val = lower_expr(n->b);
                if (ptd && ptd->is_string)
                    ins = emit(IR_SB);
                else
                    ins = emit(IR_SW);
                ins->a = addr;
                ins->b = val;
                return;
            }
        }
        if (n->a->kind != N_NAME)
            die("lower:%d: assignment to non-name", n->line);
        lhs_name = n->a->name;

        /* function return: assignment to function name */
        if (cur_func_name &&
            strcmp(lhs_name, cur_func_name) == 0 &&
            ret_slot >= 0) {
            val = lower_expr(n->b);
            ins = emit(IR_STL);
            ins->a = val;
            ins->slot = ret_slot;
            return;
        }

        s = find_sym(lhs_name);
        if (!s) {
            if (nwith > 0) {
                int addr = with_field_addr(lhs_name);
                if (addr >= 0) {
                    val = lower_expr(n->b);
                    ins = emit(IR_SW);
                    ins->a = addr;
                    ins->b = val;
                    return;
                }
            }
            die("lower:%d: undefined '%s'",
                n->line, lhs_name);
        }

        if (s->type_name) {
            struct type_def *td = find_type(s->type_name);
            if (td && td->is_string) {
                int dst = addr_sym(s);
                int src = 0, maxl;
                int args[3];
                if (n->b->kind == N_STR) {
                    char namebuf[32];
                    int slen = n->b->slen;
                    int ml = td->maxlen;
                    int copylen = slen < ml ?
                                  slen : ml;
                    char *buf;
                    struct ir_insn *lea;
                    int padded = ((copylen + 1) +
                                  3) & ~3;
                    buf = arena_zalloc(lower_arena, padded);
                    buf[0] = (char)copylen;
                    memcpy(buf + 1, n->b->sval,
                           copylen);
                    snprintf(namebuf, sizeof(namebuf),
                             "__str_%d", strctr++);
                    add_string_global(namebuf, buf,
                                      padded);
                    lea = emit(IR_LEA);
                    lea->dst = new_temp();
                    lea->sym = arena_strdup(lower_arena,namebuf);
                    src = lea->dst;
                } else if (n->b->kind == N_NAME) {
                    struct sym *rs;
                    rs = find_sym(n->b->name);
                    if (!rs)
                        die("lower:%d: "
                            "undefined '%s'",
                            n->line,
                            n->b->name);
                    src = addr_sym(rs);
                } else {
                    src = lower_expr(n->b);
                }
                maxl = lower_const(td->maxlen);
                args[0] = dst;
                args[1] = src;
                args[2] = maxl;
                emit_call("__pascal_str_assign",
                          args, 3);
                return;
            }
            if (td && td->is_set) {
                int dst = addr_sym(s);
                int src = lower_expr(n->b);
                emit_block_copy(dst, src, 32);
                return;
            }
            if (td) {
                int dst = addr_sym(s);
                int src = 0;
                if (n->b->kind == N_NAME) {
                    struct sym *rs;
                    rs = find_sym(n->b->name);
                    if (!rs)
                        die("lower:%d: undefined '%s'",
                            n->line, n->b->name);
                    src = addr_sym(rs);
                } else if (n->b->kind == N_DOT ||
                           n->b->kind == N_INDEX) {
                    src = lower_lvalue_addr(n->b);
                } else {
                    die("lower:%d: cannot copy compound "
                        "type from expression",
                        n->line);
                }
                emit_block_copy(dst, src, td->total_size);
                return;
            }
        }

        val = lower_expr(n->b);
        store_sym(s, val);
        return;
    }

    case N_CALL:
        (void)lower_call(n);
        return;

    case N_IF:
        ltrue = new_label();
        lfalse = new_label();
        lend = new_label();
        emit_bnz(lower_expr(n->a), ltrue);
        emit_jmp(lfalse);
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
        emit_label(ltop);
        emit_bz(lower_expr(n->a), lbrk);
        if (nloops >= 64)
            die("lower: nested loops too deep");
        loops[nloops].brk = lbrk;
        loops[nloops].cont = ltop;
        nloops++;
        lower_stmt(n->b);
        nloops--;
        emit_jmp(ltop);
        emit_label(lbrk);
        return;

    case N_FOR: {
        int ltop_f, lbrk_f, lbody, lcont_f;
        struct sym *s;
        int cmp_tmp, limit_slot;

        s = find_sym(n->name);
        if (!s)
            die("lower:%d: undefined for variable '%s'",
                n->line, n->name);

        /* evaluate limit once and stash in a hidden slot */
        limit_slot = alloc_slot(4);
        {
            int lv = lower_expr(n->b);
            ins = emit(IR_STL);
            ins->a = lv;
            ins->slot = limit_slot;
        }

        /* init: var := start */
        {
            int start = lower_expr(n->a);
            store_sym(s, start);
        }

        ltop_f = new_label();
        lbrk_f = new_label();
        lbody = new_label();
        lcont_f = new_label();
        emit_label(ltop_f);

        /* condition: var <= limit (or >= for downto) */
        {
            int cur_val, limit;
            cur_val = load_sym(s);
            limit = new_temp();
            ins = emit(IR_LDL);
            ins->dst = limit;
            ins->slot = limit_slot;
            if (n->downto) {
                ins = emit(IR_CMPGES);
                ins->dst = new_temp();
                ins->a = cur_val;
                ins->b = limit;
            } else {
                ins = emit(IR_CMPLES);
                ins->dst = new_temp();
                ins->a = cur_val;
                ins->b = limit;
            }
            cmp_tmp = ins->dst;
        }
        emit_bz(cmp_tmp, lbrk_f);
        emit_label(lbody);

        if (nloops >= 64)
            die("lower: nested loops too deep");
        loops[nloops].brk = lbrk_f;
        loops[nloops].cont = lcont_f;
        nloops++;
        lower_stmt(n->c);
        nloops--;

        /* increment/decrement */
        emit_label(lcont_f);
        {
            int cur_val, step, result;
            cur_val = load_sym(s);
            step = lower_const(n->downto ? -1 : 1);
            ins = emit(IR_ADD);
            ins->dst = new_temp();
            ins->a = cur_val;
            ins->b = step;
            result = ins->dst;
            store_sym(s, result);
        }
        emit_jmp(ltop_f);
        emit_label(lbrk_f);
        return;
    }

    case N_REPEAT: {
        int ltop_r, lbrk_r;

        ltop_r = new_label();
        lbrk_r = new_label();
        emit_label(ltop_r);
        if (nloops >= 64)
            die("lower: nested loops too deep");
        loops[nloops].brk = lbrk_r;
        loops[nloops].cont = ltop_r;
        nloops++;
        lower_stmts(n->a);
        nloops--;
        emit_bz(lower_expr(n->b), ltop_r);
        emit_label(lbrk_r);
        return;
    }

    case N_CASE: {
        struct node *arm, *lab;
        int sel, sel_slot, lend;

        sel = lower_expr(n->a);
        sel_slot = alloc_slot(4);
        ins = emit(IR_STL);
        ins->a = sel;
        ins->slot = sel_slot;
        lend = new_label();

        for (arm = n->b; arm; arm = arm->next) {
            int lbody = new_label();
            int lnext = new_label();

            for (lab = arm->a; lab; lab = lab->next) {
                long start = const_eval(lab->b);
                int sv = new_temp();
                ins = emit(IR_LDL);
                ins->dst = sv;
                ins->slot = sel_slot;
                if (lab->a) {
                    long end = const_eval(lab->a);
                    int cstart, cend;
                    int t1, t2, t3;
                    cstart = lower_const(start);
                    ins = emit(IR_CMPGES);
                    ins->dst = new_temp();
                    ins->a = sv;
                    ins->b = cstart;
                    t1 = ins->dst;
                    sv = new_temp();
                    ins = emit(IR_LDL);
                    ins->dst = sv;
                    ins->slot = sel_slot;
                    cend = lower_const(end);
                    ins = emit(IR_CMPLES);
                    ins->dst = new_temp();
                    ins->a = sv;
                    ins->b = cend;
                    t2 = ins->dst;
                    ins = emit(IR_AND);
                    ins->dst = new_temp();
                    ins->a = t1;
                    ins->b = t2;
                    t3 = ins->dst;
                    emit_bnz(t3, lbody);
                } else {
                    int cstart = lower_const(start);
                    ins = emit(IR_CMPEQ);
                    ins->dst = new_temp();
                    ins->a = sv;
                    ins->b = cstart;
                    emit_bnz(ins->dst, lbody);
                }
            }
            emit_jmp(lnext);
            emit_label(lbody);
            lower_stmt(arm->b);
            emit_jmp(lend);
            emit_label(lnext);
        }
        if (n->c)
            lower_stmt(n->c);
        emit_label(lend);
        return;
    }

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

    case N_WITH: {
        struct node *w;
        int saved_nwith = nwith;
        for (w = n->a; w; w = w->next) {
            const char *tname = lvalue_type_name(w);
            struct type_def *td;
            int base_addr, wslot;
            if (!tname)
                die("lower:%d: with requires a record",
                    n->line);
            td = find_type(tname);
            if (!td || td->is_array)
                die("lower:%d: with requires a record",
                    n->line);
            base_addr = lower_lvalue_addr(w);
            wslot = alloc_slot(4);
            ins = emit(IR_STL);
            ins->a = base_addr;
            ins->slot = wslot;
            if (nwith >= 16)
                die("lower:%d: too many nested with",
                    n->line);
            with_stack[nwith].base_slot = wslot;
            with_stack[nwith].td = td;
            nwith++;
        }
        lower_stmt(n->b);
        nwith = saved_nwith;
        return;
    }

    default:
        die("lower:%d: unhandled stmt kind %d", n->line, n->kind);
    }
}

/****************************************************************
 * Block lowering (declarations + compound)
 ****************************************************************/

static void
lower_const_section(struct node *consts)
{
    struct node *c;

    for (c = consts; c; c = c->next) {
        if (c->kind == N_TYPEDEF) {
            register_type(c);
            continue;
        }
        struct sym s = {0};
        s.name = c->name;
        s.is_const = 1;
        if (c->a && c->a->kind == N_STR && c->a->slen >= 1) {
            s.const_str = c->a->sval;
            s.const_slen = c->a->slen;
            if (c->a->slen == 1)
                s.const_val = (unsigned char)c->a->sval[0];
        } else {
            s.const_val = const_eval(c->a);
        }
        push_sym(s);
    }
    /* fixup array element sizes that couldn't resolve at registration */
    for (int i = 0; i < ntypes; i++) {
        struct type_def *t = &types[i];
        if (t->is_array && t->elem_type_name) {
            struct type_def *et = find_type(t->elem_type_name);
            if (et && et->total_size != t->elem_size) {
                t->elem_size = et->total_size;
                t->total_size = (t->hi_bound - t->lo_bound + 1)
                                * t->elem_size;
            }
        }
    }
}

static void
lower_var_section(struct node *vars)
{
    struct node *v;

    for (v = vars; v; v = v->next) {
        struct sym s = {0};
        int sz = 4;
        s.name = v->name;
        s.is_local = 1;
        s.type_name = v->sval;
        if (v->sval) {
            struct type_def *td = find_type(v->sval);
            if (td)
                sz = td->total_size;
        }
        s.slot = alloc_slot(sz);
        push_sym(s);
    }
}

static void
lower_var_inits(struct node *vars)
{
    struct node *v;

    for (v = vars; v; v = v->next) {
        if (!v->a)
            continue;
        struct sym *s = find_sym(v->name);
        if (!s)
            continue;
        if (s->type_name && v->a->kind == N_STR) {
            struct type_def *td = find_type(s->type_name);
            if (td && td->is_string) {
                char namebuf[32];
                int slen = v->a->slen;
                int ml = td->maxlen;
                int copylen = slen < ml ? slen : ml;
                int padded = ((copylen + 1) + 3) & ~3;
                char *buf = arena_zalloc(lower_arena, padded);
                struct ir_insn *lea;
                int args[3];
                buf[0] = (char)copylen;
                memcpy(buf + 1, v->a->sval, copylen);
                snprintf(namebuf, sizeof(namebuf),
                         "__str_%d", strctr++);
                add_string_global(namebuf, buf, padded);
                lea = emit(IR_LEA);
                lea->dst = new_temp();
                lea->sym = arena_strdup(lower_arena, namebuf);
                args[0] = addr_sym(s);
                args[1] = lea->dst;
                args[2] = lower_const(ml);
                emit_call("__pascal_str_assign",
                          args, 3);
                continue;
            }
        }
        {
            int val = lower_expr(v->a);
            store_sym(s, val);
        }
    }
}

/****************************************************************
 * Function/procedure lowering
 ****************************************************************/

static struct ir_func *
lower_function(struct node *decl, int is_func,
               char **caps, int ncaps)
{
    struct ir_func *fn, *saved_fn;
    struct ir_insn *ins;
    struct node *p, *d, *block;
    int nparams, saved_nsyms, saved_nslots;
    char *saved_func_name;
    int saved_ret_slot, saved_exit_label, saved_nloops, saved_nwith;
    const char *ir_name = decl->name;

    {
        struct sym *fs = find_sym(decl->name);
        if (fs && fs->ir_name)
            ir_name = fs->ir_name;
    }

    fn = ir_new_func(lower_arena, ir_name);
    saved_fn = cur_fn;
    cur_fn = fn;
    saved_nloops = nloops;
    nloops = 0;
    saved_nwith = nwith;
    nwith = 0;
    saved_func_name = cur_func_name;
    saved_ret_slot = ret_slot;
    saved_exit_label = exit_label;
    exit_label = new_label();

    saved_nsyms = nsyms;
    saved_nslots = nslots;
    nslots = 0;

    nparams = 0;

    /* hidden capture parameters (passed as pointers) */
    for (int i = 0; i < ncaps; i++) {
        struct sym s = {0};
        s.name = caps[i];
        s.is_local = 1;
        s.is_var_param = 1;
        s.slot = alloc_slot(4);
        push_sym(s);
        nparams++;
    }

    for (p = decl->a; p; p = p->next) {
        struct sym s = {0};
        int is_compound = 0;
        s.name = p->name;
        s.is_local = 1;
        s.type_name = p->sval;
        if (p->sval && find_type(p->sval))
            is_compound = 1;
        if (p->op == 1)
            s.is_var_param = 1;
        else if (p->op == 2 && is_compound)
            s.is_var_param = 1;
        s.slot = alloc_slot(4);
        push_sym(s);
        nparams++;
    }
    fn->nparams = nparams;

    /* function return value slot */
    if (is_func) {
        cur_func_name = decl->name;
        ret_slot = alloc_slot(4);
    } else {
        cur_func_name = NULL;
        ret_slot = -1;
    }

    block = decl->b;
    if (block && block->kind == N_BLOCK) {
        lower_const_section(block->a);
        lower_var_section(block->b);

        /* register nested procedures in symbol table */
        for (d = block->c; d; d = d->next) {
            struct sym s = {0};
            char mangled[256];
            char *ncap[32];
            int nc;

            snprintf(mangled, sizeof(mangled), "%s__%s",
                     ir_name, d->name);
            s.name = d->name;
            s.is_func = 1;
            s.decl = d;
            s.ir_name = arena_strdup(lower_arena, mangled);

            nc = find_captures(d, saved_nsyms, ncap, 32);
            if (nc > 0) {
                s.ncaptures = nc;
                s.captures = arena_alloc(lower_arena, nc * sizeof(char *));
                for (int i = 0; i < nc; i++)
                    s.captures[i] = ncap[i];
            }
            push_sym(s);
        }

        /* lower nested procedures */
        for (d = block->c; d; d = d->next) {
            struct ir_func *nfn;
            struct sym *fs;

            if (d->is_forward)
                continue;
            fs = find_sym(d->name);
            nfn = lower_function(d,
                                 d->kind == N_FUNCDECL,
                                 fs->captures,
                                 fs->ncaptures);
            *nested_ftail = nfn;
            nested_ftail = &nfn->next;
        }
    }

    ins = emit(IR_FUNC);
    ins->sym = arena_strdup(lower_arena,ir_name);
    ins->nargs = nparams;

    /* init return slot to 0 */
    if (is_func) {
        int zero = lower_const(0);
        ins = emit(IR_STL);
        ins->a = zero;
        ins->slot = ret_slot;
    }

    /* compound value params: copy from caller address to local slot */
    for (p = decl->a; p; p = p->next) {
        struct type_def *td;
        if (p->op != 0 || !p->sval)
            continue;
        td = find_type(p->sval);
        if (!td)
            continue;
        {
            struct sym *ps = find_sym(p->name);
            int param_slot = ps->slot;
            int copy_slot = alloc_slot(td->total_size);
            int src_addr, dst_addr;
            ins = emit(IR_LDL);
            ins->dst = new_temp();
            ins->slot = param_slot;
            src_addr = ins->dst;
            ins = emit(IR_ADL);
            ins->dst = new_temp();
            ins->slot = copy_slot;
            dst_addr = ins->dst;
            emit_block_copy(dst_addr, src_addr, td->total_size);
            ps->slot = copy_slot;
            ps->is_var_param = 0;
        }
    }

    if (block && block->b)
        lower_var_inits(block->b);

    if (block && block->d)
        lower_stmt(block->d);

    emit_label(exit_label);

    /* emit return */
    if (is_func) {
        int rv = new_temp();
        ins = emit(IR_LDL);
        ins->dst = rv;
        ins->slot = ret_slot;
        ins = emit(IR_RETV);
        ins->a = rv;
    } else {
        emit(IR_RET);
    }

    emit(IR_ENDF);

    fn->nslots = nslots;
    fn->slot_size = arena_alloc(lower_arena, nslots * sizeof(int));
    memcpy(fn->slot_size, slot_sizes, nslots * sizeof(int));

    nsyms = saved_nsyms;
    nslots = saved_nslots;
    cur_fn = saved_fn;
    cur_func_name = saved_func_name;
    ret_slot = saved_ret_slot;
    exit_label = saved_exit_label;
    nloops = saved_nloops;
    nwith = saved_nwith;

    return fn;
}

/****************************************************************
 * Entry point
 ****************************************************************/

struct ir_program *
lower_program(struct arena *a, struct node *ast)
{
    struct ir_program *prog;
    struct ir_func *fn, **ftail;
    struct node *block, *d;
    struct ir_insn *ins;
    int nparams;

    lower_arena = a;
    prog = arena_zalloc(lower_arena, sizeof(*prog));
    cur_prog = prog;
    nsyms = 0;
    ntypes = 0;
    strctr = 0;

    if (!ast || ast->kind != N_PROGRAM)
        die("lower: expected program node");

    block = ast->a;
    if (!block || block->kind != N_BLOCK)
        die("lower: expected block node");

    /* register global constants */
    lower_const_section(block->a);

    /* register global variables as IR globals */
    for (d = block->b; d; d = d->next) {
        struct ir_global *g;
        struct sym s = {0};
        struct type_def *vtd = NULL;

        g = arena_zalloc(lower_arena, sizeof(*g));
        g->name = arena_strdup(lower_arena, d->name);
        g->base_type = IR_I32;
        if (d->sval) {
            vtd = find_type(d->sval);
            if (vtd)
                g->arr_size = vtd->total_size / 4;
        }
        if (vtd && vtd->is_set) {
            g->base_type = IR_I8;
            g->arr_size = 32;
            if (d->a && d->a->kind == N_SET) {
                unsigned char setbuf[32] = {0};
                for (struct node *e = d->a->a; e;
                     e = e->next) {
                    if (e->kind == N_SETRANGE) {
                        int lo = (int)const_eval(e->a);
                        int hi = (int)const_eval(e->b);
                        for (int k = lo; k <= hi; k++)
                            if (k >= 0 && k <= 255)
                                setbuf[k >> 3] |=
                                    (1 << (k & 7));
                    } else {
                        int v = (int)const_eval(e);
                        if (v >= 0 && v <= 255)
                            setbuf[v >> 3] |=
                                (1 << (v & 7));
                    }
                }
                g->init_ivals = arena_zalloc(lower_arena, 32 *
                    sizeof(*g->init_ivals));
                for (int k = 0; k < 32; k++)
                    g->init_ivals[k] = setbuf[k];
                g->init_count = 32;
            }
            g->next = prog->globals;
            prog->globals = g;
            s.name = d->name;
            s.is_local = 0;
            s.type_name = d->sval;
            push_sym(s);
            continue;
        }
        if (vtd && vtd->is_string) {
            int ml = vtd->maxlen;
            int tsz = vtd->total_size;
            g->base_type = IR_I8;
            g->arr_size = tsz;
            if (d->a && d->a->kind == N_STR) {
                int slen = d->a->slen;
                int copylen = slen < ml ? slen : ml;
                g->init_ivals = arena_zalloc(lower_arena, (copylen + 1) *
                    sizeof(*g->init_ivals));
                g->init_ivals[0] = copylen;
                for (int i = 0; i < copylen; i++)
                    g->init_ivals[i + 1] =
                        (unsigned char)d->a->sval[i];
                g->init_count = copylen + 1;
            }
        } else if (d->a && d->a->kind == N_INITLIST) {
            int cnt = count_init_list(d->a);
            int pos = 0;
            g->init_ivals = arena_zalloc(lower_arena, cnt * sizeof(*g->init_ivals));
            flatten_init_list(d->a, g->init_ivals, &pos);
            g->init_count = cnt;
        } else if (d->a && d->a->kind == N_STR &&
                   d->a->slen > 1) {
            int slen = d->a->slen;
            g->init_ivals = arena_zalloc(lower_arena, slen * sizeof(*g->init_ivals));
            for (int i = 0; i < slen; i++)
                g->init_ivals[i] =
                    (unsigned char)d->a->sval[i];
            g->init_count = slen;
        } else if (d->a) {
            g->init_ivals = arena_zalloc(lower_arena, 1 * sizeof(*g->init_ivals));
            g->init_ivals[0] = const_eval(d->a);
            g->init_count = 1;
        }
        g->next = prog->globals;
        prog->globals = g;

        s.name = d->name;
        s.is_local = 0;
        s.type_name = d->sval;
        push_sym(s);
    }

    /* register all procedures/functions in symbol table */
    for (d = block->c; d; d = d->next) {
        struct sym s = {0};
        s.name = d->name;
        s.is_func = 1;
        s.decl = d;
        push_sym(s);
    }

    ftail = &prog->funcs;
    nested_funcs = NULL;
    nested_ftail = &nested_funcs;

    /* lower each procedure/function */
    for (d = block->c; d; d = d->next) {
        if (d->is_forward)
            continue;
        fn = lower_function(d, d->kind == N_FUNCDECL,
                            NULL, 0);
        *ftail = fn;
        ftail = &fn->next;
    }

    /* chain nested functions */
    *ftail = nested_funcs;
    while (*ftail)
        ftail = &(*ftail)->next;

    /* lower main block as "main" function */
    {
        struct ir_func *main_fn;

        main_fn = ir_new_func(lower_arena, "main");
        cur_fn = main_fn;
        nloops = 0;
        nslots = 0;
        nparams = 0;
        cur_func_name = NULL;
        ret_slot = -1;

        /* main block may have its own locals (though unusual) */
        sym_base = nsyms;

        ins = emit(IR_FUNC);
        ins->sym = arena_strdup(lower_arena,"main");
        ins->nargs = 0;

        if (block->d)
            lower_stmt(block->d);

        /* implicit exit(0) */
        {
            int args[1];
            args[0] = lower_const(0);
            emit_call("exit", args, 1);
        }
        {
            int z = lower_const(0);
            ins = emit(IR_RETV);
            ins->a = z;
        }
        emit(IR_ENDF);

        main_fn->nparams = nparams;
        main_fn->nslots = nslots;
        if (nslots > 0) {
            main_fn->slot_size = arena_alloc(lower_arena, nslots * sizeof(int));
            memcpy(main_fn->slot_size, slot_sizes,
                   nslots * sizeof(int));
        }

        *ftail = main_fn;
    }

    free(syms);
    syms = NULL;
    nsyms = sym_cap = 0;

    free(slot_sizes);
    slot_sizes = NULL;
    nslots = slot_cap = 0;

    free(types);
    types = NULL;
    ntypes = type_cap = 0;

    return prog;
}
