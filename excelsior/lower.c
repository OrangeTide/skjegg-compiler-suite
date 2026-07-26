/* lower.c : Excelsior typed AST to IR lowering.
 *
 * Runs after name resolution and type checking, over the annotated AST
 * (node->sym, node->type). It walks each class verb/func into an ir_func
 * and hands the ir_program to the shared regalloc + backend.
 *
 * Because Excelsior is statically typed there is no runtime tagging of
 * atomic values: an int or bool is a bare 32-bit machine word, so the
 * lowering is a direct 3AC walk with none of the box/unbox traffic a
 * dynamic language needs.
 *
 * Scope of this first increment: the integer / bool core.
 *   - literals, locals, params, and integer consts
 *   - arithmetic (+ - * / %), comparisons, and the word boolean
 *     operators (short-circuit `and` / `or`, plus `xor` / `not`)
 *   - var / assign (+= / -=), if / elseif / else, while, for over an
 *     integer range, return, break, continue, and expression statements
 *   - calls to a sibling verb / func in the same class
 *
 * The integer/bool core plus the object model: scalar int/bool/fixed/str
 * fields as static-segment globals (a str field is a pointer word whose
 * literal default is a relocation to an interned descriptor), single-
 * inheritance field layout, and verb sends
 * lowered to the host dispatch primitive __exc_send (see host-abi.md), so
 * self, cross-object, inherited, and overridden dispatch all go through
 * one path. Each class emits a descriptor { parent, nverbs, (selector,
 * code)* } the host walks to resolve a send; `self` lowers to the
 * __exc_self handle; spawn(ClassName) lowers to __exc_spawn(&Class__desc).
 * Float (IEEE 754 double) is lowered: locals, literals, arithmetic,
 * comparisons, int<->float casts, and float func args/returns, with ITOF/
 * FTOI inserted where int and float meet. Decimal (base-10 fixed point,
 * value x 10^-4 in an int32; numbers.md) shares int storage, so add/sub/
 * compare/negate/modulo are the int ops on the scaled value, literals fold
 * to scaled constants, multiply and divide call the host helpers
 * __exc_decmul/__exc_decdiv, and int<->decimal conversions rescale by
 * 10^4 (widening range-checked). Strings lower for literals, concat (+),
 * equality (==, !=), lexicographic ordering (< <= > >=), one-char index
 * (s[i]), and slice (s[lo..hi], 1-based inclusive): a string value is a
 * pointer to a { len, data } descriptor (so it reuses int storage), a
 * literal emits the bytes and descriptor as globals, and the operations
 * call host helpers. String interpolation `"...${e}..."` lowers to a concat
 * chain of the literal pieces and each hole stringified (N_TOSTR ->
 * __exc_str_from_int/float/fixed, str is identity). The if-expression
 * `cond then A else B` (N_THENELSE) branches on the bool condition to
 * one of two branch values through a result slot; it is a general
 * expression, any type. Output is a send (output.md): `player.tell(msg)`
 * reaches the host console through __exc_send, and `trace x` / `///` are
 * the -t-gated author channel routed to the type-specific
 * __exc_trace_str/int/float/fixed/bool stderr helpers.
 * A homogeneous data literal ([1 2 3] or ["a" "b"])
 * lowers to a constant list global { count, e0, e1, ... }; list indexing
 * xs[i] loads *(list + i*4) (1-based), and `otherwise` bounds-checks it. The list
 * builtins len(x) (word 0, a str length or list count), append(l, v), and
 * set(l, i, v) are bare-name builtins; append/set are immutable and return a
 * fresh list from the arena. Not lowered yet:
 * prop, is-test, symbolic data literals, enum values, float<->fixed casts,
 * float/fixed/str through sends (argv marshaling is an open ABI question),
 * and the remaining powers.
 *
 * Entry: the compiler emits __exc_entry_class and __exc_entry_selector for
 * the class holding a verb whose name folds to `main`; the test host
 * (runtime/exc_host.c) bootstraps an actor of that class and sends it the
 * entry selector. Members are emitted as `Class__member`.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "excelsior.h"
#include "ir.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static struct arena *la;
static const char *efile;
static struct ir_func *cur_fn;
static struct ir_program *cur_prog;
static const char *cur_class;
static int cur_ret_float;               /* current func/verb returns float */
static struct ex_type *cur_ret_type;    /* current func/verb return type */

static NORETURN void
lerr(struct node *n, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    die("%s:%d: %s", efile, n ? n->line : 0, buf);
}

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

/* the emitted symbol for a class member: `Class__member` */
static char *
member_symbol(const char *cls, const char *member)
{
    char buf[256];

    snprintf(buf, sizeof buf, "%s__%s", cls, member);
    return arena_strdup(la, buf);
}

/* the class's descriptor symbol (see host-abi.md) */
static char *
desc_symbol(const char *cls)
{
    char buf[256];

    snprintf(buf, sizeof buf, "%s__desc", cls);
    return arena_strdup(la, buf);
}

/* Selectors: each distinct verb name (case-folded) gets a global id that
 * the class descriptor and the send site agree on. Interned per program. */
static char **sel_names;
static int nsels, sel_cap;

static int
sel_id(const char *name)
{
    for (int i = 0; i < nsels; i++)
        if (ex_ci_eq(sel_names[i], name))
            return i;
    if (nsels == sel_cap) {
        sel_cap = sel_cap ? sel_cap * 2 : 32;
        sel_names = realloc(sel_names, sel_cap * sizeof *sel_names);
        if (!sel_names)
            die("lower: out of memory");
    }
    sel_names[nsels] = arena_strdup(la, name);
    return nsels++;
}

static int
new_label(void)
{
    return ir_new_label(cur_fn);
}

/****************************************************************
 * Per-function slots and locals
 *
 * Locals and params are keyed by their resolved symbol pointer, which
 * the resolver made unique per declaration. That sidesteps name folding
 * and shadowing entirely: a reference already points at its declaration.
 ****************************************************************/

static int *slot_sizes;
static int nslots, slot_cap;

struct lslot {
    struct sym *sym;
    int slot;
};
static struct lslot *lslots;
static int nlslots, lslot_cap;

/* loop break / continue targets */
struct loopf { int brk, cont; };
static struct loopf loops[64];
static int nloops;

static int
alloc_slot(int bytes)
{
    if (nslots == slot_cap) {
        slot_cap = slot_cap ? slot_cap * 2 : 32;
        slot_sizes = realloc(slot_sizes, slot_cap * sizeof *slot_sizes);
        if (!slot_sizes)
            die("lower: out of memory");
    }
    slot_sizes[nslots] = bytes;
    return nslots++;
}

static int
bind_slot(struct sym *s, int bytes)
{
    int slot = alloc_slot(bytes);

    if (nlslots == lslot_cap) {
        lslot_cap = lslot_cap ? lslot_cap * 2 : 32;
        lslots = realloc(lslots, lslot_cap * sizeof *lslots);
        if (!lslots)
            die("lower: out of memory");
    }
    lslots[nlslots].sym = s;
    lslots[nlslots].slot = slot;
    nlslots++;
    return slot;
}

static int
find_slot(struct sym *s)
{
    for (int i = nlslots - 1; i >= 0; i--)
        if (lslots[i].sym == s)
            return lslots[i].slot;
    return -1;
}

/****************************************************************
 * Small emit helpers
 ****************************************************************/

static int
lower_const(long v)
{
    struct ir_insn *ins = emit(IR_LIC);
    ins->dst = new_temp();
    ins->imm = v;
    return ins->dst;
}

static void
emit_label(int lab)
{
    emit(IR_LABEL)->label = lab;
}

static void
emit_jmp(int lab)
{
    emit(IR_JMP)->label = lab;
}

static void
load_slot(int dst, int slot)
{
    struct ir_insn *ins = emit(IR_LDL);
    ins->dst = dst;
    ins->slot = slot;
}

static void
store_slot(int src, int slot)
{
    struct ir_insn *ins = emit(IR_STL);
    ins->a = src;
    ins->slot = slot;
}

/****************************************************************
 * Float (IEEE 754 double). A float value lives in a float temp defined by
 * a float op; storage (local slot, field global) is 8 bytes. The type
 * checker annotates each expression, so lowering emits int or float ops
 * from node->type and inserts ITOF/FTOI where an int flows into a float
 * context or a cast crosses the two.
 ****************************************************************/

static int fltctr;

/* look through a maybe wrapper: lowering works on the payload type
 * (the failure lives in control flow, fallible.md) */
static struct ex_type *
lstrip(struct ex_type *t)
{
    if (t && t->kind == ET_MAYBE && t->inner)
        return t->inner;
    return t;
}

static int
is_ftype(struct ex_type *t)
{
    t = lstrip(t);
    return t && t->kind == T_TFLOAT;
}

static int
node_is_float(struct node *n)
{
    return is_ftype(n->type);
}

static int
type_size(struct ex_type *t)
{
    return is_ftype(t) ? 8 : 4;
}

/* a float literal: an 8-byte constant in .data, loaded into a float temp */
static int
lower_flt(double v)
{
    union { double d; int64_t l; } u;
    struct ir_global *g;
    struct ir_insn *ins;
    char buf[32];
    int addr;

    u.d = v;
    snprintf(buf, sizeof buf, "__flt_%d", fltctr++);
    g = arena_zalloc(la, sizeof *g);
    g->name = arena_strdup(la, buf);
    g->base_type = IR_F64;
    g->arr_size = 1;
    g->is_local = 1;
    g->init_count = 1;
    g->init_ivals = arena_alloc(la, sizeof *g->init_ivals);
    g->init_ivals[0] = u.l;
    g->next = cur_prog->globals;
    cur_prog->globals = g;

    ins = emit(IR_LEA);
    ins->dst = addr = new_temp();
    ins->sym = arena_strdup(la, buf);
    ins = emit(IR_FLD);
    ins->dst = new_temp();
    ins->a = addr;
    return ins->dst;
}

/****************************************************************
 * Strings. A string value is a pointer to a two-word descriptor
 * { i32 len, ptr data }, so it reuses int storage everywhere (slots,
 * params, fields, returns) the way a fixed reuses int. A literal emits the
 * bytes and the descriptor as two globals; operations call host helpers.
 * This mirrors MooScript's string lowering (moo/lower.c) and runtime
 * (runtime/str.c), from which the descriptor and helper set are ported.
 ****************************************************************/

static int strctr;

/* intern a string literal as two globals (a byte blob plus a { len, &blob }
 * descriptor whose data slot is a relocation the linker fills in) and return
 * the descriptor global's name. A string value is that descriptor's address.
 * Used both by expression lowering (via LEA) and by a string field's default
 * initialiser (as a relocation). Mirrors moo/lower.c. */
static char *
intern_strlit(const char *s, int len)
{
    struct ir_global *g;
    char sbuf[32], hbuf[32];
    char *hname;
    int id = strctr++;

    snprintf(sbuf, sizeof sbuf, "__exc_str_%d", id);
    g = arena_zalloc(la, sizeof *g);
    g->name = arena_strdup(la, sbuf);
    g->base_type = IR_I8;
    g->arr_size = len + 1;
    g->is_local = 1;
    g->init_string = arena_alloc(la, len + 1);
    memcpy(g->init_string, s, len);
    g->init_string[len] = '\0';
    g->init_strlen = len + 1;
    g->next = cur_prog->globals;
    cur_prog->globals = g;

    snprintf(hbuf, sizeof hbuf, "__exc_shdr_%d", id);
    hname = arena_strdup(la, hbuf);
    g = arena_zalloc(la, sizeof *g);
    g->name = hname;
    g->base_type = IR_I32;
    g->arr_size = 2;
    g->is_local = 1;
    g->init_count = 2;
    g->init_ivals = arena_alloc(la, 2 * sizeof *g->init_ivals);
    g->init_ivals[0] = len;             /* slot 0: length */
    g->init_ivals[1] = 0;
    g->init_syms = arena_zalloc(la, 2 * sizeof(char *));
    g->init_syms[1] = arena_strdup(la, sbuf);   /* slot 1: &bytes (reloc) */
    g->next = cur_prog->globals;
    cur_prog->globals = g;
    return hname;
}

/* a string literal as an expression: the descriptor's address in a temp */
static int
lower_strlit(const char *s, int len)
{
    struct ir_insn *ins;
    char *hname = intern_strlit(s, len);

    ins = emit(IR_LEA);
    ins->dst = new_temp();
    ins->sym = hname;
    return ins->dst;
}

/****************************************************************
 * Trap descriptors (runtime-errors.md). A trap site names its
 * failure with a { kind, line, &file, &name } constant handed to
 * __exc_trap(desc, a, b) along with up to two runtime words.
 ****************************************************************/

/* kinds, kept in sync with runtime/exc_host.c */
enum {
    EXC_TRAP_NO_BRANCH   = 1,
    EXC_TRAP_INDEX_RANGE = 2,
    EXC_TRAP_UNCONSUMED  = 3,
    EXC_TRAP_DIV_ZERO    = 4,
    EXC_TRAP_OVERFLOW    = 5,
};

/* a NUL-terminated byte blob global; the host reads it as a C string */
static char *
intern_cstr(const char *s)
{
    struct ir_global *g;
    char buf[32];
    int len = strlen(s);

    snprintf(buf, sizeof buf, "__exc_cstr_%d", strctr++);
    g = arena_zalloc(la, sizeof *g);
    g->name = arena_strdup(la, buf);
    g->base_type = IR_I8;
    g->arr_size = len + 1;
    g->is_local = 1;
    g->init_string = arena_strdup(la, s);
    g->init_strlen = len + 1;
    g->next = cur_prog->globals;
    cur_prog->globals = g;
    return g->name;
}

static char *cstr_file;             /* the module filename, interned once */

/* one { kind, line, &file, &name } constant per trap site; name (the
 * failing callee etc.) may be NULL */
static char *
emit_trapdesc(int kind, int line, const char *name)
{
    struct ir_global *g;
    char buf[32];

    if (!cstr_file)
        cstr_file = intern_cstr(lex_filename());
    snprintf(buf, sizeof buf, "__exc_trapdesc_%d", strctr++);
    g = arena_zalloc(la, sizeof *g);
    g->name = arena_strdup(la, buf);
    g->base_type = IR_I32;
    g->arr_size = 4;
    g->is_local = 1;
    g->init_count = 4;
    g->init_ivals = arena_zalloc(la, 4 * sizeof *g->init_ivals);
    g->init_ivals[0] = kind;
    g->init_ivals[1] = line;
    g->init_syms = arena_zalloc(la, 4 * sizeof(char *));
    g->init_syms[2] = cstr_file;
    if (name)
        g->init_syms[3] = intern_cstr(name);
    g->next = cur_prog->globals;
    cur_prog->globals = g;
    return g->name;
}

/* an inline fault: __exc_trap(desc, a, b) never returns (exit 70) */
static void
emit_trap_call(int kind, int line, const char *name, int a, int b)
{
    struct ir_insn *ins;
    int d;

    ins = emit(IR_LEA);
    ins->dst = new_temp();
    ins->sym = emit_trapdesc(kind, line, name);
    d = ins->dst;
    ins = emit(IR_ARG); ins->a = d; ins->imm = 0;
    ins = emit(IR_ARG); ins->a = a; ins->imm = 1;
    ins = emit(IR_ARG); ins->a = b; ins->imm = 2;
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(la, "__exc_trap");
    ins->nargs = 3;
}

/* A homogeneous data literal `[1 2 3]`, `[0f1.5 0f2.5]`, or `["a" "b"]`
 * lowers to a constant list: a global { count, e0, e1, ... } whose first
 * word is the count and whose elements are int/bool words, scaled fixed
 * words, or interned string-descriptor pointers. The value is the
 * global's address (a list is a pointer, like a str). A `${expr}` hole
 * whose expression folds to a constant joins the skeleton; each
 * runtime-valued hole patches its slot with one `__exc_list_set` call
 * (copy-on-write, so the constant skeleton itself is never written).
 * Symbolic data literals (names, nesting) are not lowered yet. */
static int lower_expr(struct node *n);
static int node_is_dec(struct node *n);
static int copy_if_record(struct node *vn, int v);
static int listctr;

/* fold a compile-time constant expression to its word value: literals,
 * int consts, unary minus, and int + - * / % arithmetic (a fixed value
 * folds to its scaled word; fixed arithmetic does not fold) */
static int
fold_const_expr(struct node *e, long *out)
{
    long a, b;

    if (!e)
        return 0;
    switch (e->kind) {
    case N_NUM:
        *out = e->ival;
        return 1;
    case N_BOOL:
        *out = e->ival ? 1 : 0;
        return 1;
    case N_FLOAT:
        if (!node_is_dec(e))
            return 0;
        *out = e->ival;                 /* checker folded the scale */
        return 1;
    case N_NAME:
        if (e->sym && e->sym->kind == SYM_CONST && e->sym->decl &&
            e->sym->decl->a &&
            (e->sym->decl->a->kind == N_NUM ||
             (e->sym->decl->a->kind == N_FLOAT &&
              node_is_dec(e->sym->decl->a)))) {
            *out = e->sym->decl->a->ival;
            return 1;
        }
        return 0;
    case N_UNOP:
        if (e->op == T_MINUS && fold_const_expr(e->a, &a)) {
            *out = -a;
            return 1;
        }
        return 0;
    case N_BINOP:
        if (!e->type || e->type->kind != T_TINT)
            return 0;
        if (!fold_const_expr(e->a, &a) || !fold_const_expr(e->b, &b))
            return 0;
        switch (e->op) {
        case T_PLUS:    *out = a + b; return 1;
        case T_MINUS:   *out = a - b; return 1;
        case T_STAR:    *out = a * b; return 1;
        case T_SLASH:   if (!b) return 0; *out = a / b; return 1;
        case T_PERCENT: if (!b) return 0; *out = a % b; return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static int
lower_listlit(struct node *n)
{
    struct ir_global *g;
    struct ir_insn *ins;
    struct node *it;
    char buf[32];
    long v;
    int count = 0, i, all_str = -1, nruntime = 0;

    /* the type checker enforces homogeneity; these are backstops */
    for (it = n->a; it; it = it->next) {
        int isstr;

        if (it->kind == N_HOLE) {       /* class vouched by the checker */
            if (!fold_const_expr(it->a, &v))
                nruntime++;
            count++;
            continue;
        }
        isstr = it->kind == N_STR;
        if (it->kind != N_NUM && it->kind != N_BOOL &&
            it->kind != N_FLOAT && it->kind != N_STR)
            lerr(n, "only number/bool/decimal/string data literals lower "
                    "to a list");
        if (all_str < 0)
            all_str = isstr;
        else if (all_str != isstr)
            lerr(n, "a list literal must be all numbers or all strings");
        count++;
    }

    snprintf(buf, sizeof buf, "__exc_list_%d", listctr++);
    g = arena_zalloc(la, sizeof *g);
    g->name = arena_strdup(la, buf);
    g->base_type = IR_I32;
    g->arr_size = count + 1;
    g->is_local = 1;
    g->init_count = count + 1;
    g->init_ivals = arena_zalloc(la, (count + 1) * sizeof *g->init_ivals);
    g->init_syms = arena_zalloc(la, (count + 1) * sizeof *g->init_syms);
    g->init_ivals[0] = count;           /* elem[i] at word i+1 (1-based) */
    i = 1;
    for (it = n->a; it; it = it->next, i++) {
        if (it->kind == N_HOLE) {
            if (fold_const_expr(it->a, &v))
                g->init_ivals[i] = v;
            /* else: 0 placeholder, patched below */
        } else if (it->kind == N_STR) {
            g->init_syms[i] = intern_strlit(it->sval, it->slen);
        } else if (it->kind == N_FLOAT) {
            g->init_ivals[i] = it->ival;    /* scaled decimal */
        } else {
            g->init_ivals[i] = it->kind == N_BOOL ? (it->ival ? 1 : 0)
                                                  : it->ival;
        }
    }
    g->next = cur_prog->globals;
    cur_prog->globals = g;

    ins = emit(IR_LEA);
    ins->dst = new_temp();
    ins->sym = arena_strdup(la, buf);

    if (nruntime) {
        /* patch each runtime hole left to right; set is copy-on-write,
         * so the result is a fresh arena list per patch and the
         * skeleton stays constant */
        int base = ins->dst;

        i = 1;
        for (it = n->a; it; it = it->next, i++) {
            int val, idx;

            if (it->kind != N_HOLE || fold_const_expr(it->a, &v))
                continue;
            val = copy_if_record(it->a, lower_expr(it->a));
            idx = lower_const(i);
            ins = emit(IR_ARG); ins->a = base; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = idx;  ins->imm = 1;
            ins = emit(IR_ARG); ins->a = val;  ins->imm = 2;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_list_set");
            ins->nargs = 3;
            base = ins->dst;
        }
        return base;
    }
    return ins->dst;
}

static int
emit_conv(int t, int op)                /* IR_ITOF or IR_FTOI */
{
    struct ir_insn *ins = emit(op);
    ins->dst = new_temp();
    ins->a = t;
    return ins->dst;
}

/****************************************************************
 * Decimal (base-10 fixed point, numbers.md). A decimal value is a plain
 * int32 holding `real x 10^4`, so it shares int storage, add/sub,
 * compare, negate, and modulo with int. Only multiply and divide need
 * the rescale: they go through host helpers with a 64-bit intermediate
 * (__exc_decmul / __exc_decdiv, round half away from zero). An int
 * operand in a decimal context is widened by `x 10^4` (range-checked);
 * `as int` narrows by a truncating divide.
 ****************************************************************/

static int
is_dtype(struct ex_type *t)
{
    t = lstrip(t);
    return t && t->kind == T_TDEC;
}

static int
node_is_dec(struct node *n)
{
    return is_dtype(n->type);
}

static int
node_is_str(struct node *n)
{
    return n->type && lstrip(n->type)->kind == T_TSTR;
}

static int
node_is_list(struct node *n)
{
    return n->type && lstrip(n->type)->kind == T_TLIST;
}

/* the element enum of a `set of E` type / node, or NULL (set-of.md) */
static struct sym *
type_set_enum(struct ex_type *t)
{
    struct ex_type *in;
    t = t ? lstrip(t) : NULL;
    in = (t && t->kind == T_TSET && t->inner) ? lstrip(t->inner) : NULL;
    return (in && in->kind == T_IDENT && in->sym &&
            in->sym->kind == SYM_ENUM) ? in->sym : NULL;
}

static struct sym *
node_set_enum(struct node *n)
{
    return n ? type_set_enum(n->type) : NULL;
}

/* lower a set-construction expression to its bitmask word (set-of.md): a
 * member is a singleton bit `1 << ordinal`, `empty`/`full` the identities,
 * and `+`/`-`/`^` are word union/difference/toggle. A set-valued sub-
 * expression (a var, field, or call) is already the mask, loaded directly. */
static int
lower_set(struct node *n, struct sym *en)
{
    struct ir_insn *ins;
    int r;

    if (n->kind == N_EMPTY)
        return lower_const(0);
    if (n->kind == N_FULL) {
        int nm = 0;
        for (struct node *m = en->decl ? en->decl->a : NULL; m; m = m->next)
            nm++;
        return lower_const((1L << nm) - 1);
    }
    if (n->kind == N_BINOP &&
        (n->op == T_PLUS || n->op == T_MINUS || n->op == T_CARET)) {
        int a = lower_set(n->a, en);
        int b = lower_set(n->b, en);
        r = new_temp();
        if (n->op == T_PLUS) {
            ins = emit(IR_OR);  ins->dst = r; ins->a = a; ins->b = b;
        } else if (n->op == T_CARET) {
            ins = emit(IR_XOR); ins->dst = r; ins->a = a; ins->b = b;
        } else {                        /* difference: a AND NOT b */
            int nb = new_temp();
            ins = emit(IR_NOT); ins->dst = nb; ins->a = b;
            ins = emit(IR_AND); ins->dst = r; ins->a = a; ins->b = nb;
        }
        return r;
    }
    if ((n->kind == N_NAME || n->kind == N_FIELD_ACC) && n->sym &&
        n->sym->kind == SYM_ENUM_MEMBER)
        return lower_const(1L << (n->sym->decl ? n->sym->decl->ival : 0));
    if (node_set_enum(n) == NULL && n->type &&      /* a runtime enum value: */
        lstrip(n->type)->kind == T_IDENT &&         /* the singleton 1 << e */
        lstrip(n->type)->sym == en) {
        int e = lower_expr(n);
        int one = lower_const(1);
        r = new_temp();
        ins = emit(IR_SHL); ins->dst = r; ins->a = one; ins->b = e;
        return r;
    }
    return lower_expr(n);               /* a set-valued var / field / call */
}

static int
node_is_bool(struct node *n)
{
    return n->type && lstrip(n->type)->kind == T_TBOOL;
}

/* a record value (records.md): a named type whose sym is a SYM_RECORD */
static struct sym *
node_record_sym(struct node *n)
{
    struct ex_type *t = n->type ? lstrip(n->type) : NULL;
    if (t && t->kind == T_IDENT && t->sym && t->sym->kind == SYM_RECORD)
        return t->sym;
    return NULL;
}

/* is the named field of a record a str? (a str field compares by content) */
static int
rec_field_is_str(struct sym *rec, const char *name)
{
    struct node *f;
    if (rec && rec->decl)
        for (f = rec->decl->a; f; f = f->next)
            if (ex_ci_eq(f->name, name))
                return f->type && lstrip(f->type)->kind == T_TSTR;
    return 0;
}

/* the SYM_RECORD of a record type, or NULL */
static struct sym *
type_record_sym(struct ex_type *t)
{
    t = t ? lstrip(t) : NULL;
    if (t && t->kind == T_IDENT && t->sym && t->sym->kind == SYM_RECORD)
        return t->sym;
    return NULL;
}

/* Records nest flat (records.md): a record field is laid out inline inside
 * its parent's block, no pointer indirection. So a record occupies the sum
 * of its fields' flattened sizes, a nested record field's rvalue is the
 * interior pointer base+offset, and a whole-record copy blits that many
 * words. These helpers compute the flat layout. */

/* the field's own record type, or NULL if it is a scalar/str field */
static struct sym *
rec_field_rec(struct sym *rec, const char *name)
{
    struct node *f;
    if (rec && rec->decl)
        for (f = rec->decl->a; f; f = f->next)
            if (ex_ci_eq(f->name, name))
                return f->type ? type_record_sym(f->type) : NULL;
    return NULL;
}

/* One layout engine for both aggregates. A record's block and an object's
 * field segment are laid out by the same rule (records.md's flat nesting,
 * host-abi.md's per-instance segment): a nested record is inline, every other
 * field is one word, and a class's parent block sits first, so an inherited
 * field keeps its offset in a subclass and a subclass block starts with a
 * valid parent block. A record has no parent, so the recursion stops there.
 *
 * `agg` is a SYM_RECORD or a SYM_CLASS. Offsets are relative to the block; an
 * object adds its header (see field_addr). */
static int
agg_words(struct sym *agg)
{
    int n;
    struct node *m;

    if (!agg)
        return 0;
    n = agg_words(agg->base);           /* the parent prefix, classes only */
    for (m = agg->decl ? agg->decl->a : NULL; m; m = m->next) {
        struct sym *sub;
        if (m->kind != N_FIELD || !m->sym)
            continue;                   /* a class body also holds verbs */
        sub = m->sym->type ? type_record_sym(m->sym->type) : NULL;
        n += sub ? agg_words(sub) : 1;
    }
    return n;
}

/* the word offset of a named field within `agg`'s block, or -1 if absent.
 * An inherited field answers from the parent prefix, whose offsets are
 * already relative to the same block base. */
static int
agg_woffset(struct sym *agg, const char *name)
{
    int off;
    struct node *m;

    if (!agg)
        return -1;
    if (agg->base) {
        int p = agg_woffset(agg->base, name);
        if (p >= 0)
            return p;
    }
    off = agg_words(agg->base);
    for (m = agg->decl ? agg->decl->a : NULL; m; m = m->next) {
        struct sym *sub;
        if (m->kind != N_FIELD || !m->sym)
            continue;
        if (ex_ci_eq(m->name, name))
            return off;
        sub = m->sym->type ? type_record_sym(m->sym->type) : NULL;
        off += sub ? agg_words(sub) : 1;
    }
    return -1;
}

/* the record spellings, kept as the names the record paths already use */
#define rec_flat_words(r)          agg_words(r)
#define rec_field_woffset(r, name) agg_woffset((r), (name))

/* a fresh record construction Point(...) needs no value-copy */
static int
is_record_ctor(struct node *rhs)
{
    return rhs && rhs->kind == N_CALL && rhs->a && rhs->a->kind == N_NAME &&
           rhs->a->sym && rhs->a->sym->kind == SYM_RECORD;
}

/* value semantics for a record value entering a list (or any by-value store):
 * copy it unless it is a fresh construction. A non-record value passes through
 * unchanged, so this is a no-op for list<int> / list<str>. */
static int
copy_if_record(struct node *vn, int v)
{
    struct sym *rec = node_record_sym(vn);
    struct ir_insn *ins;
    int nfc, r;

    if (!rec || is_record_ctor(vn))
        return v;
    nfc = lower_const(rec_flat_words(rec));     /* const before the args */
    ins = emit(IR_ARG); ins->a = v;   ins->imm = 0;
    ins = emit(IR_ARG); ins->a = nfc; ins->imm = 1;
    ins = emit(IR_CALL);
    ins->dst = r = new_temp();
    ins->sym = arena_strdup(la, "__exc_rec_copy");
    ins->nargs = 2;
    return r;
}

/* decimal is base-10 fixed-point: value x 10^-4 in an int32 (numbers.md) */
#define DEC_SCALE   10000
#define DEC_MAX_INT 214748          /* largest int that widens into decimal */

static int trap_site(int kind, struct node *n, const char *name,
                     int aslot, int bslot, int bderef);

/* Widen an int value into decimal (x * 10^4). An int beyond +/- 214748
 * does not fit: a constant is a compile error, a runtime value faults
 * (OVERFLOW, runtime-errors.md). */
static int
dec_widen(int val, struct node *src)
{
    struct ir_insn *ins;
    int sc;

    if (src && src->kind == N_NUM) {    /* constant: fold, checked */
        if (src->ival > DEC_MAX_INT || src->ival < -DEC_MAX_INT)
            lerr(src, "int constant %ld does not fit decimal "
                      "(about +/- 214748)", src->ival);
        return lower_const(src->ival * DEC_SCALE);
    }
    {
        int lf = trap_site(EXC_TRAP_OVERFLOW, src, NULL, -1, -1, 0);
        int lim, c;

        lim = lower_const(DEC_MAX_INT);
        c = new_temp();
        ins = emit(IR_CMPGTS);
        ins->dst = c;
        ins->a = val;
        ins->b = lim;
        ins = emit(IR_BNZ);
        ins->a = c;
        ins->label = lf;
        lim = lower_const(-DEC_MAX_INT);
        c = new_temp();
        ins = emit(IR_CMPLTS);
        ins->dst = c;
        ins->a = val;
        ins->b = lim;
        ins = emit(IR_BNZ);
        ins->a = c;
        ins->label = lf;
    }
    sc = lower_const(DEC_SCALE);
    ins = emit(IR_MUL);
    ins->dst = new_temp();
    ins->a = val;
    ins->b = sc;
    return ins->dst;
}

/* a value from `src` as decimal: identity for decimal, widen for int */
static int
to_dec(int val, struct node *src)
{
    if (node_is_dec(src))
        return val;
    return dec_widen(val, src);
}

/* narrow a decimal to int, truncating toward zero (x / 10^4) */
static int
dec_narrow(int val)
{
    struct ir_insn *ins;
    int sc = lower_const(DEC_SCALE);

    ins = emit(IR_DIVS);
    ins->dst = new_temp();
    ins->a = val;
    ins->b = sc;
    return ins->dst;
}

/* coerce a value computed from `src` to the storage of the destination type:
 * int widens into float (ITOF) or decimal (x 10^4); a float or decimal
 * narrows to int (FTOI / truncating divide) where it reaches an int
 * destination */
static int
coerce(int t, struct node *src, struct ex_type *dst)
{
    if (is_ftype(dst))
        return node_is_float(src) ? t : emit_conv(t, IR_ITOF);
    if (is_dtype(dst))
        return to_dec(t, src);
    if (node_is_float(src))
        return emit_conv(t, IR_FTOI);
    if (node_is_dec(src))
        return dec_narrow(t);
    return t;
}

/****************************************************************
 * Expression lowering
 ****************************************************************/

static int lower_expr(struct node *n);
static void lower_stmt(struct node *n);
static void lower_cond(struct node *n, int ltrue, int lfalse);

/****************************************************************
 * Fallibility (fallible.md): every fallible producer branches to the
 * dynamically scoped fail label; consumers install their own label
 * around their operand. The statement default is the function's shared
 * label: a trap in plain funcs and verbs, the fail epilogue (return
 * the null word) in `returns maybe` / `can fail` funcs.
 ****************************************************************/

static int cur_fail;              /* the active fail label */
static int fn_fail_label;         /* the function's default, lazy */
static int fn_fallible;           /* current member may fail itself */

static int
default_fail(void)
{
    if (fn_fail_label < 0)          /* label ids start at 0: -1 is unset */
        fn_fail_label = new_label();
    return fn_fail_label;
}

/* Per-site cold trampolines (runtime-errors.md). A fallible producer
 * whose failure would trap gets its own label; the trampolines are
 * emitted after the function body, each loading its runtime words and
 * calling __exc_trap with this site's descriptor. The happy path keeps
 * exactly one compare and one branch. */
struct trap_site {
    int label;
    int kind;
    int line;
    const char *name;             /* the failing callee etc., or NULL */
    int aslot;                    /* slot to pass as arg a, or -1 */
    int bslot;                    /* slot to pass as arg b, or -1 */
    int bderef;                   /* b is word 0 at that pointer (a length) */
};
#define MAX_TRAP_SITES 128
static struct trap_site sites[MAX_TRAP_SITES];
static int nsites;

static int
trap_site(int kind, struct node *n, const char *name,
          int aslot, int bslot, int bderef)
{
    struct trap_site *s;

    if (nsites >= MAX_TRAP_SITES)
        lerr(n, "too many trap sites in one member");
    s = &sites[nsites++];
    s->label = new_label();
    s->kind = kind;
    s->line = n ? n->line : 0;
    s->name = name;
    s->aslot = aslot;
    s->bslot = bslot;
    s->bderef = bderef;
    return s->label;
}

/* Failure target for a fallible producer: a consumer's label when one
 * is active, the shared propagate label in a fallible func, or a fresh
 * trap site that reports this exact producer. */
static int
fail_target(int kind, struct node *n, const char *name,
            int aslot, int bslot, int bderef)
{
    if (cur_fail != fn_fail_label || fn_fallible)
        return cur_fail;
    return trap_site(kind, n, name, aslot, bslot, bderef);
}

/* the cold section: one trampoline per registered site */
static void
emit_trap_sites(void)
{
    struct ir_insn *ins;
    int i, a, b;

    for (i = 0; i < nsites; i++) {
        struct trap_site *s = &sites[i];

        emit_label(s->label);
        if (s->aslot >= 0) {
            a = new_temp();
            load_slot(a, s->aslot);
        } else {
            a = lower_const(0);
        }
        if (s->bslot >= 0) {
            b = new_temp();
            load_slot(b, s->bslot);
            if (s->bderef) {
                ins = emit(IR_LW);
                ins->dst = new_temp();
                ins->a = b;
                b = ins->dst;
            }
        } else {
            b = lower_const(0);
        }
        emit_trap_call(s->kind, s->line, s->name, a, b);
        emit(IR_RET);
    }
    nsites = 0;
}

/* branch to the failure target when the null word w is nothing */
static void
fail_if_zero(int w, struct node *n, const char *name)
{
    struct ir_insn *ins = emit(IR_BZ);
    ins->a = w;
    ins->label = fail_target(EXC_TRAP_UNCONSUMED, n, name, -1, -1, 0);
}

static int
maybe_is_word(struct ex_type *mt)   /* payload needs a box in storage */
{
    struct ex_type *in = mt ? mt->inner : NULL;
    if (!in)
        return 1;
    return in->kind != T_TSTR && in->kind != T_TLIST;
}

/* null-word rep of a maybe -> the plain payload (or branch to fail);
 * n/name identify the producer for the trap report */
static int
unwrap_maybe(int w, struct ex_type *mt, struct node *n, const char *name)
{
    struct ir_insn *ins;

    fail_if_zero(w, n, name);
    if (!maybe_is_word(mt))
        return w;                   /* the pointer is the value */
    ins = emit(IR_LW);
    ins->dst = new_temp();
    ins->a = w;
    return ins->dst;
}

/* plain payload -> null-word rep (boxes word-sized payloads) */
static int
box_maybe(int v, struct ex_type *mt)
{
    struct ir_insn *ins;

    if (!maybe_is_word(mt))
        return v;
    ins = emit(IR_ARG);
    ins->a = v;
    ins->imm = 0;
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(la, "__exc_box");
    ins->nargs = 1;
    return ins->dst;
}

static int coerce(int t, struct node *src, struct ex_type *dst);

/* the capture consumer: evaluate a fallible expression into the
 * null-word rep of mt (its failure becomes a stored nothing) */
static int
capture_maybe(struct node *a, struct ex_type *mt)
{
    int slot = alloc_slot(4);
    int lend = new_label(), lf = new_label();
    int save = cur_fail, v, r;

    cur_fail = lf;
    v = coerce(lower_expr(a), a, mt->inner);
    cur_fail = save;
    store_slot(box_maybe(v, mt), slot);
    emit_jmp(lend);
    emit_label(lf);
    store_slot(lower_const(0), slot);
    emit_label(lend);
    r = new_temp();
    load_slot(r, slot);
    return r;
}

/* bounds-check a spilled base/index pair: out of range fails; a trap
 * report carries the index and the length (loaded from the base) */
static void
index_check(struct node *n, int bslot, int islot, int is_str)
{
    struct ir_insn *ins;
    int lf = fail_target(EXC_TRAP_INDEX_RANGE, n, NULL, islot, bslot, 1);

    {                               /* i < 1: fail */
        int i = new_temp(), one, c;
        load_slot(i, islot);
        one = lower_const(1);
        c = new_temp();
        ins = emit(IR_CMPLTS);
        ins->dst = c;
        ins->a = i;
        ins->b = one;
        ins = emit(IR_BNZ);
        ins->a = c;
        ins->label = lf;
    }
    {                               /* i > length: fail. A list's length is
                                     * word 0; a str's is its code-point count
                                     * (R7), so byte-length word 0 is wrong. */
        int b = new_temp(), len = new_temp(), i = new_temp(), c;
        load_slot(b, bslot);
        if (is_str) {
            ins = emit(IR_ARG); ins->a = b; ins->imm = 0;
            ins = emit(IR_CALL);
            ins->dst = len;
            ins->sym = arena_strdup(la, "__exc_str_len");
            ins->nargs = 1;
        } else {
            ins = emit(IR_LW);
            ins->dst = len;
            ins->a = b;
        }
        load_slot(i, islot);
        c = new_temp();
        ins = emit(IR_CMPGTS);
        ins->dst = c;
        ins->a = i;
        ins->b = len;
        ins = emit(IR_BNZ);
        ins->a = c;
        ins->label = lf;
    }
}

/* Checks before an integer divide or modulo (runtime-errors.md). A
 * zero divisor is a fallible failure (a domain condition, consumable
 * with `otherwise`); INT_MIN / -1 has no representable result and is always
 * an OVERFLOW fault, never consumable, so it bypasses fail_target. */
static void
div_checks(struct node *n, int l, int r)
{
    struct ir_insn *ins;
    int lok = new_label();
    int c1, c2, m1, mn;

    ins = emit(IR_BZ);              /* divisor zero: fail */
    ins->a = r;
    ins->label = fail_target(EXC_TRAP_DIV_ZERO, n, NULL, -1, -1, 0);

    m1 = lower_const(-1);           /* r == -1 and l == INT_MIN: fault */
    c1 = new_temp();
    ins = emit(IR_CMPEQ);
    ins->dst = c1;
    ins->a = r;
    ins->b = m1;
    ins = emit(IR_BZ);
    ins->a = c1;
    ins->label = lok;
    mn = lower_const(-2147483647 - 1);
    c2 = new_temp();
    ins = emit(IR_CMPEQ);
    ins->dst = c2;
    ins->a = l;
    ins->b = mn;
    ins = emit(IR_BNZ);
    ins->a = c2;
    ins->label = trap_site(EXC_TRAP_OVERFLOW, n, NULL, -1, -1, 0);
    emit_label(lok);
}

/* an int-valued 0/1 from a boolean expression via the condition path */
static int
lower_bool(struct node *n)
{
    int ltrue = new_label(), lfalse = new_label(), lend = new_label();
    int t = new_temp();
    struct ir_insn *ins;

    lower_cond(n, ltrue, lfalse);
    emit_label(ltrue);
    ins = emit(IR_LIC); ins->dst = t; ins->imm = 1;
    emit_jmp(lend);
    emit_label(lfalse);
    ins = emit(IR_LIC); ins->dst = t; ins->imm = 0;
    emit_label(lend);
    return t;
}

/****************************************************************
 * Fields (the VM static segment)
 *
 * A class field compiles to a module global at a fixed address; `self.x`
 * (or a bare `x` that resolved to a field) is a load/store of that global.
 * This models one actor context: the running VM is the actor, and its
 * fields are the static segment. Only int/bool scalar fields lower yet.
 ****************************************************************/

static int
field_is_scalar(struct sym *f)
{
    /* "scalar" here means lowered as a single word. A maybe field is one
     * null-word (fallible.md); a str or record field is a pointer word (the
     * record's arena block, records.md), so both lower like a scalar. */
    struct ex_type *t = lstrip(f->type);
    if (t && (t->kind == T_TINT || t->kind == T_TBOOL ||
              t->kind == T_TDEC || t->kind == T_TSTR))
        return 1;
    if (t && t->kind == T_IDENT && t->sym && t->sym->kind == SYM_ENUM)
        return 1;                       /* an enum field is a small int (D1) */
    if (type_set_enum(t))
        return 1;                       /* a set field is a bitmask word */
    return type_record_sym(t) != NULL;
}

/* The field segment: every object carries its own, laid out parent fields
 * first so an inherited field keeps its offset in a subclass, and the header
 * (the class pointer) sits in front of it. A field is one word
 * (field_is_scalar), so a class's size is a count. */
#define OBJ_HDR_WORDS 1

static int lower_self(void);

/* a class's field segment, in words: the shared layout engine (agg_words),
 * so an object and a record answer the same way about the same fields */
static int
class_field_words(struct sym *cls)
{
    return agg_words(cls);
}

/* a field's word offset from the object's base, past the header */
static int
field_woffset(struct sym *f)
{
    return OBJ_HDR_WORDS + agg_woffset(f->owner, f->name);
}

/* the address of `self`'s copy of a field: the running actor handle plus the
 * field's constant offset. Every instance has its own, so two actors of one
 * class no longer share storage. */
static int
field_addr(struct sym *f)
{
    struct ir_insn *ins;
    int base = lower_self();                    /* emits; keep it first */
    int off = lower_const((long)field_woffset(f) * 4);
    int r;

    ins = emit(IR_ADD);
    ins->dst = r = new_temp();
    ins->a = base;
    ins->b = off;
    return r;
}

/* a compile-time int/bool constant, for a field's copy-on-write default */
static long
const_eval(struct node *e)
{
    if (!e)
        return 0;
    switch (e->kind) {
    case N_NUM:
        return e->ival;
    case N_BOOL:
        return e->ival ? 1 : 0;
    case N_FLOAT:                       /* a decimal default: scaled */
        return e->ival;
    case N_NOTHING:
        return 0;                   /* a maybe field defaults empty */
    case N_NAME:
        if (ex_ci_eq(e->name, "empty"))     /* a set field defaults empty (0) */
            return 0;
        if (e->sym && e->sym->kind == SYM_CONST && e->sym->decl &&
            e->sym->decl->a &&
            (e->sym->decl->a->kind == N_NUM ||
             (e->sym->decl->a->kind == N_FLOAT &&
              node_is_dec(e->sym->decl->a))))
            return e->sym->decl->a->ival;
        break;
    case N_FIELD_ACC:                   /* an enum-member default: its ordinal */
        if (e->sym && e->sym->kind == SYM_ENUM_MEMBER)
            return e->sym->decl ? e->sym->decl->ival : 0;
        break;
    case N_EMPTY:                       /* a set field defaults empty (0) */
        return 0;
    default:
        break;
    }
    lerr(e, "field default must be a constant int, bool, ratio, or enum member");
}

/* Fill one class's fields into the initial image, at their segment offsets.
 * Walks the parent first, so an inherited field lands where the subclass
 * addresses it and carries the parent's default. */
static void
image_fill(struct sym *cls, struct ir_global *g)
{
    int i;

    if (!cls)
        return;
    image_fill(cls->base, g);
    for (struct node *m = cls->decl ? cls->decl->a : NULL; m; m = m->next) {
        if (m->kind != N_FIELD || !m->sym || !field_is_scalar(m->sym))
            continue;
        i = field_woffset(m->sym) - OBJ_HDR_WORDS;
        if (!m->a)
            continue;                   /* no default: zero, already */
        if (m->sym->type->kind == T_TSTR) {
            /* a string field is a pointer word; its literal default is a
             * relocation to an interned descriptor */
            if (m->a->kind != N_STR)
                lerr(m->a, "a string field default must be a "
                           "string literal");
            g->init_syms[i] = intern_strlit(m->a->sval, m->a->slen);
        } else if (type_record_sym(lstrip(m->sym->type))) {
            /* a record field is a pointer word to its arena block; it
             * defaults to null, so it must be assigned before use (a
             * compile-time record default is not lowered yet) */
            lerr(m->a, "a record field default is not lowered yet; "
                       "assign the record before use");
        } else {                        /* int / bool / decimal / enum / set */
            g->init_ivals[i] = const_eval(m->a);
        }
    }
}

/* `Class__image`: the initial field segment an object of this class starts
 * from, inherited fields included, which __exc_spawn copies into each new
 * instance. Emitted only for a class that has fields. */
static char *
emit_field_image(struct node *cls)
{
    struct ir_global *g;
    int n = class_field_words(cls->sym);
    char buf[256];

    if (n == 0)
        return NULL;
    snprintf(buf, sizeof buf, "%s__image", cls->name);
    g = arena_zalloc(la, sizeof *g);
    g->name = arena_strdup(la, buf);
    g->base_type = IR_I32;
    g->arr_size = n;
    g->is_local = 1;
    g->init_count = n;
    g->init_ivals = arena_zalloc(la, n * sizeof *g->init_ivals);
    g->init_syms = arena_zalloc(la, n * sizeof *g->init_syms);
    image_fill(cls->sym, g);
    g->next = cur_prog->globals;
    cur_prog->globals = g;
    return g->name;
}

/* a per-enum name table `Enum__names`: the member name descriptors in ordinal
 * order, so `${enumval}` prints the word not the ordinal (enums.md D6). */
static void
emit_enum_names(struct node *en)
{
    struct ir_global *g;
    struct node *m;
    int n = 0, i = 0;
    char buf[256];

    for (m = en->a; m; m = m->next)
        n++;
    if (!n)
        return;
    g = arena_zalloc(la, sizeof *g);
    snprintf(buf, sizeof buf, "%s__names", en->name);
    g->name = arena_strdup(la, buf);
    g->base_type = IR_I32;
    g->arr_size = n;
    g->is_local = 1;
    g->init_count = n;
    g->init_syms = arena_zalloc(la, n * sizeof *g->init_syms);
    g->init_ivals = arena_zalloc(la, n * sizeof *g->init_ivals);
    for (m = en->a; m; m = m->next, i++)
        g->init_syms[i] = intern_strlit(m->name, (int)strlen(m->name));
    g->next = cur_prog->globals;
    cur_prog->globals = g;
}

/* the class descriptor (host-abi.md): { parent, nverbs, (selector, code)* }.
 * The host resolves a send by walking this and its parent chain, so
 * inherited, overridden, and cross-object dispatch all go through it. */
static void
emit_class_desc(struct node *cls)
{
    struct ir_global *g;
    int nverbs = 0, n, i;

    for (struct node *m = cls->a; m; m = m->next)
        if (m->kind == N_VERB)
            nverbs++;

    /* { parent, nverbs, nwords, image, (selector, code)* } : nwords sizes the
     * per-instance field segment and image seeds it (host-abi.md) */
    n = 4 + 2 * nverbs;
    g = arena_zalloc(la, sizeof *g);
    g->name = desc_symbol(cls->name);
    g->base_type = IR_I32;
    g->arr_size = n;
    g->is_local = 1;
    g->init_count = n;
    g->init_syms = arena_zalloc(la, n * sizeof *g->init_syms);
    g->init_ivals = arena_zalloc(la, n * sizeof *g->init_ivals);

    if (cls->sym->base)             /* slot 0: parent descriptor, or 0 */
        g->init_syms[0] = desc_symbol(cls->sym->base->name);
    g->init_ivals[1] = nverbs;      /* slot 1: verb count */
    g->init_ivals[2] = class_field_words(cls->sym);      /* slot 2: nwords */
    g->init_syms[3] = emit_field_image(cls);             /* slot 3: image */
    i = 0;
    for (struct node *m = cls->a; m; m = m->next) {
        if (m->kind != N_VERB)
            continue;
        g->init_ivals[4 + 2 * i] = sel_id(m->name);            /* selector */
        g->init_syms[5 + 2 * i] = member_symbol(cls->name, m->name); /* code */
        i++;
    }
    g->next = cur_prog->globals;
    cur_prog->globals = g;
}

/* Globals the test host reads to bootstrap: a pointer to the entry class's
 * descriptor and the entry verb's selector. Emitted once, for the first
 * class that has a verb whose name folds to `main`. */
static void
emit_entry(struct node *file)
{
    struct ir_global *gc, *gs;
    struct node *cls = NULL, *verb = NULL;

    for (struct node *it = file->a; it && !verb; it = it->next) {
        if (it->kind != N_CLASS)
            continue;
        for (struct node *m = it->a; m; m = m->next)
            if (m->kind == N_VERB && ex_ci_eq(m->name, "main")) {
                cls = it;
                verb = m;
                break;
            }
    }
    if (!verb)
        return;                     /* library unit: no entry */

    gc = arena_zalloc(la, sizeof *gc);
    gc->name = "__exc_entry_class";
    gc->base_type = IR_I32;
    gc->init_count = 1;
    gc->init_syms = arena_zalloc(la, sizeof *gc->init_syms);
    gc->init_ivals = arena_zalloc(la, sizeof *gc->init_ivals);
    gc->init_syms[0] = desc_symbol(cls->name);
    gc->next = cur_prog->globals;
    cur_prog->globals = gc;

    gs = arena_zalloc(la, sizeof *gs);
    gs->name = "__exc_entry_selector";
    gs->base_type = IR_I32;
    gs->init_count = 1;
    gs->init_ivals = arena_zalloc(la, sizeof *gs->init_ivals);
    gs->init_ivals[0] = sel_id(verb->name);
    gs->next = cur_prog->globals;
    cur_prog->globals = gs;

    /* the entry verb's arity: 0 for a nullary main, 1 when it takes the
     * player handle (`main(player is obj)`, output.md); the host passes
     * the console object accordingly */
    {
        struct ir_global *ga = arena_zalloc(la, sizeof *ga);
        int nparams = 0;
        for (struct node *p = verb->a; p; p = p->next)
            nparams++;
        ga->name = "__exc_entry_argc";
        ga->base_type = IR_I32;
        ga->init_count = 1;
        ga->init_ivals = arena_zalloc(la, sizeof *ga->init_ivals);
        ga->init_ivals[0] = nparams;
        ga->next = cur_prog->globals;
        cur_prog->globals = ga;
    }
}

/* The selector-name table: { count, &name0, &name1, ... } in selector-id
 * order, so the host can resolve a verb name to this module's selector id
 * (the console object needs `tell`). This is the seed of host-abi.md's
 * selector intern table. Emitted after all lowering, when the id space is
 * complete. */
static void
emit_selnames(void)
{
    struct ir_global *g = arena_zalloc(la, sizeof *g);

    g->name = "__exc_selnames";
    g->base_type = IR_I32;
    g->arr_size = nsels + 1;
    g->init_count = nsels + 1;
    g->init_ivals = arena_zalloc(la, (nsels + 1) * sizeof *g->init_ivals);
    g->init_syms = arena_zalloc(la, (nsels + 1) * sizeof(char *));
    g->init_ivals[0] = nsels;
    for (int i = 0; i < nsels; i++)
        g->init_syms[1 + i] = intern_cstr(sel_names[i]);
    g->next = cur_prog->globals;
    cur_prog->globals = g;
}

/* an assignable location: a local slot, a field global, or a by-reference
 * `shared` parameter whose slot holds the caller's address (shared-params.md) */
enum { LV_SLOT, LV_FIELD, LV_SHARED };

/* a by-reference `shared` parameter: its slot holds the caller's address */
static int
is_shared_param(struct sym *s)
{
    return s && s->kind == SYM_PARAM && s->decl &&
           (s->decl->flags & NF_SHARED);
}

struct lval {
    int kind;
    int slot;
    int is_float;
    struct ex_type *type;               /* declared type of the location */
    struct sym *field;
};

static struct lval
resolve_lval(struct node *n)
{
    struct lval lv;
    int is_self_field =
        n->kind == N_FIELD_ACC && n->a->kind == N_SELF &&
        n->sym && n->sym->kind == SYM_FIELD;
    int is_bare_field =
        n->kind == N_NAME && n->sym && n->sym->kind == SYM_FIELD;

    lv.is_float = 0;
    lv.type = NULL;
    if (n->kind == N_NAME && n->sym &&
        (n->sym->kind == SYM_LOCAL || n->sym->kind == SYM_PARAM)) {
        lv.kind = is_shared_param(n->sym) ? LV_SHARED : LV_SLOT;
        lv.slot = find_slot(n->sym);
        lv.field = NULL;
        lv.type = n->sym->type;
        lv.is_float = is_ftype(n->sym->type);
        if (lv.slot < 0)
            lerr(n, "`%s` used before it is in scope", n->name);
        return lv;
    }
    if (is_self_field || is_bare_field) {
        if (!field_is_scalar(n->sym))
            lerr(n, "only int, bool, decimal, str, and record fields are "
                    "lowered yet");
        lv.kind = LV_FIELD;
        lv.field = n->sym;
        lv.slot = -1;
        lv.type = n->sym->type;
        return lv;
    }
    if (n->kind == N_FIELD_ACC && n->sym && n->sym->kind == SYM_FIELD)
        lerr(n, "cross-object field access is not lowered yet; send a message");
    lerr(n, "this is not an assignable location");
}

static int
lval_load(struct lval *lv)
{
    struct ir_insn *ins;
    int addr, dst;

    if (lv->kind == LV_SLOT) {
        ins = emit(lv->is_float ? IR_FLDL : IR_LDL);
        ins->dst = dst = new_temp();
        ins->slot = lv->slot;
        return dst;
    }
    if (lv->kind == LV_SHARED) {        /* deref: the slot holds the address */
        ins = emit(IR_LDL);
        ins->dst = addr = new_temp();
        ins->slot = lv->slot;
        ins = emit(IR_LW);
        ins->dst = dst = new_temp();
        ins->a = addr;
        return dst;
    }
    addr = field_addr(lv->field);
    /* a record field is laid out inline in the segment, exactly as a record
     * nests in a record, so its value is the interior pointer: no load */
    if (type_record_sym(lstrip(lv->field->type)))
        return addr;
    ins = emit(IR_LW);
    ins->dst = dst = new_temp();
    ins->a = addr;
    return dst;
}

static void
lval_store(struct lval *lv, int val)
{
    struct ir_insn *ins;
    int addr;

    if (lv->kind == LV_SLOT) {
        ins = emit(lv->is_float ? IR_FSTL : IR_STL);
        ins->a = val;
        ins->slot = lv->slot;
        return;
    }
    if (lv->kind == LV_SHARED) {        /* store through the caller's address */
        ins = emit(IR_LDL);
        ins->dst = addr = new_temp();
        ins->slot = lv->slot;
        ins = emit(IR_SW);
        ins->a = addr;
        ins->b = val;
        return;
    }
    {
        struct sym *sub = type_record_sym(lstrip(lv->field->type));

        addr = field_addr(lv->field);
        if (sub) {
            /* an inline record field: blit the value's flat words into the
             * slot, the same store a nested record field takes in a record */
            int fwc = lower_const(agg_words(sub));   /* const before the args */
            ins = emit(IR_ARG); ins->a = addr; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = val;  ins->imm = 1;
            ins = emit(IR_ARG); ins->a = fwc;  ins->imm = 2;
            ins = emit(IR_CALL);
            ins->sym = arena_strdup(la, "__exc_rec_blit");
            ins->nargs = 3;
            return;
        }
        ins = emit(IR_SW);
        ins->a = addr;
        ins->b = val;
    }
}

static int
lower_name(struct node *n)
{
    struct sym *s = n->sym;

    if (!s)
        lerr(n, "`%s` is external; host-prelude values are not lowered yet",
             n->name);
    switch (s->kind) {
    case SYM_PARAM:
    case SYM_LOCAL:
    case SYM_FIELD: {
        struct lval lv = resolve_lval(n);
        int v = lval_load(&lv);
        if (s->type && s->type->kind == ET_MAYBE)
            v = unwrap_maybe(v, s->type, n, s->name);   /* fallible producer */
        return v;
    }
    case SYM_CONST:
        if (s->decl && s->decl->a &&
            (s->decl->a->kind == N_NUM ||
             (s->decl->a->kind == N_FLOAT && node_is_dec(s->decl->a))))
            return lower_const(s->decl->a->ival);
        lerr(n, "only integer and decimal consts are lowered yet");
    default:
        lerr(n, "`%s` is not a value that lowers yet", n->name);
    }
}

/* arithmetic or comparison with a float operand. Any int operand is
 * widened with ITOF; comparisons yield an int (bool) temp. */
static int
lower_fbinop(struct node *n)
{
    struct ir_insn *ins;
    int l = lower_expr(n->a);
    int r = lower_expr(n->b);
    int irop;

    if (!node_is_float(n->a))
        l = emit_conv(l, IR_ITOF);
    if (!node_is_float(n->b))
        r = emit_conv(r, IR_ITOF);

    switch (n->op) {
    case T_PLUS:  irop = IR_FADD; break;
    case T_MINUS: irop = IR_FSUB; break;
    case T_STAR:  irop = IR_FMUL; break;
    case T_SLASH: irop = IR_FDIV; break;
    case T_EQ:    irop = IR_FCMPEQ; break;
    case T_LT:    irop = IR_FCMPLT; break;
    case T_LE:    irop = IR_FCMPLE; break;
    case T_NE: {                    /* not (a == b) */
        int eq, one;
        ins = emit(IR_FCMPEQ);
        ins->dst = eq = new_temp();
        ins->a = l;
        ins->b = r;
        one = lower_const(1);
        ins = emit(IR_XOR);
        ins->dst = new_temp();
        ins->a = eq;
        ins->b = one;
        return ins->dst;
    }
    case T_GT:                      /* a > b  ==  b < a */
        ins = emit(IR_FCMPLT);
        ins->dst = new_temp();
        ins->a = r;
        ins->b = l;
        return ins->dst;
    case T_GE:                      /* a >= b == b <= a */
        ins = emit(IR_FCMPLE);
        ins->dst = new_temp();
        ins->a = r;
        ins->b = l;
        return ins->dst;
    default:
        lerr(n, "float operator %s is not lowered yet", tok_str(n->op));
    }
    ins = emit(irop);
    ins->dst = new_temp();
    ins->a = l;
    ins->b = r;
    return ins->dst;
}

/* a fixed multiply or divide: a host helper does the 64-bit intermediate and
 * the rescale by the shared frac. Args are (a, b, frac), result in d0. */
static int
dec_helper(const char *name, int l, int r)
{
    struct ir_insn *ins;

    ins = emit(IR_ARG);
    ins->a = l;
    ins->imm = 0;
    ins = emit(IR_ARG);
    ins->a = r;
    ins->imm = 1;
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(la, name);
    ins->nargs = 2;
    return ins->dst;
}

/* arithmetic or comparison with a decimal operand. Both operands are
 * brought to the scaled representation; add/sub/compare are the plain int
 * ops on the scaled values, and multiply/divide defer to the host helpers
 * (64-bit intermediate, half-away rounding, range check). */
static int
lower_decbinop(struct node *n)
{
    struct ir_insn *ins;
    int l = to_dec(lower_expr(n->a), n->a);
    int r = to_dec(lower_expr(n->b), n->b);
    int irop;

    switch (n->op) {
    case T_STAR:  return dec_helper("__exc_decmul", l, r);
    case T_SLASH:
        /* a zero divisor fails here (fallible); the helper faults on a
         * result outside the decimal range (runtime-errors.md) */
        ins = emit(IR_BZ);
        ins->a = r;
        ins->label = fail_target(EXC_TRAP_DIV_ZERO, n, NULL, -1, -1, 0);
        return dec_helper("__exc_decdiv", l, r);
    case T_PERCENT:
        /* the remainder of the scaled words IS the scaled remainder:
         * (a x 10^4) mod (b x 10^4) = (a mod b) x 10^4. Same checks as
         * the int path (zero divisor fallible, the raw INT_MIN/-1
         * pattern an OVERFLOW fault). */
        div_checks(n, l, r);
        irop = IR_MODS;
        break;
    case T_PLUS:    irop = IR_ADD;   break;
    case T_MINUS:   irop = IR_SUB;   break;
    case T_EQ:      irop = IR_CMPEQ; break;
    case T_NE:      irop = IR_CMPNE; break;
    case T_LT:      irop = IR_CMPLTS; break;
    case T_LE:      irop = IR_CMPLES; break;
    case T_GT:      irop = IR_CMPGTS; break;
    case T_GE:      irop = IR_CMPGES; break;
    default:
        lerr(n, "decimal operator %s is not lowered yet", tok_str(n->op));
    }
    ins = emit(irop);
    ins->dst = new_temp();
    ins->a = l;
    ins->b = r;
    return ins->dst;
}

/* call a two-argument string helper, result in d0 */
static int
str_call2(const char *name, int l, int r)
{
    struct ir_insn *ins;

    ins = emit(IR_ARG);
    ins->a = l;
    ins->imm = 0;
    ins = emit(IR_ARG);
    ins->a = r;
    ins->imm = 1;
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(la, name);
    ins->nargs = 2;
    return ins->dst;
}

/* load a list element (1-based): *(list + i*4). The count is word 0, so
 * element i sits at byte offset i*4. All within one basic block. */
static int
lower_list_at(int list, int i)
{
    struct ir_insn *ins;
    int four = lower_const(4);
    int off, addr;

    ins = emit(IR_MUL);
    ins->dst = off = new_temp();
    ins->a = i;
    ins->b = four;
    ins = emit(IR_ADD);
    ins->dst = addr = new_temp();
    ins->a = list;
    ins->b = off;
    ins = emit(IR_LW);
    ins->dst = new_temp();
    ins->a = addr;
    return ins->dst;
}

/* string concat, equality, and lexicographic ordering via host helpers.
 * `!=` is `== xor 1`; ordering is `__exc_str_cmp(a, b) <rel> 0`. */
static int
lower_strbinop(struct node *n)
{
    struct ir_insn *ins;
    int l = lower_expr(n->a);
    int r = lower_expr(n->b);
    int irop;

    switch (n->op) {
    case T_PLUS:
        return str_call2("__exc_str_concat", l, r);
    case T_EQ:
        return str_call2("__exc_str_eq", l, r);
    case T_NE: {
        int eq = str_call2("__exc_str_eq", l, r);
        int one = lower_const(1);
        ins = emit(IR_XOR);
        ins->dst = new_temp();
        ins->a = eq;
        ins->b = one;
        return ins->dst;
    }
    case T_LT: irop = IR_CMPLTS; break;
    case T_LE: irop = IR_CMPLES; break;
    case T_GT: irop = IR_CMPGTS; break;
    case T_GE: irop = IR_CMPGES; break;
    default:
        lerr(n, "string operator %s is not lowered yet", tok_str(n->op));
    }
    {                                   /* ordering: cmp against 0 */
        int cmp = str_call2("__exc_str_cmp", l, r);
        int zero = lower_const(0);
        ins = emit(irop);
        ins->dst = new_temp();
        ins->a = cmp;
        ins->b = zero;
        return ins->dst;
    }
}

/* a record field slice `p with (x, y)` (record-slicing.md): the field
 * restriction is a compile-time access rule with no runtime form, so the
 * whole construct is unrolled per named field. Only str and scalar fields
 * are handled (a list or nested-record field is rejected, like `=`). */
static void
with_field_guard(struct node *n, struct sym *rec, const char *fname)
{
    struct node *f;
    if (rec && rec->decl)
        for (f = rec->decl->a; f; f = f->next)
            if (ex_ci_eq(f->name, fname)) {
                struct ex_type *ft = f->type ? lstrip(f->type) : NULL;
                if (ft && (ft->kind == T_TLIST ||
                    (ft->kind == T_IDENT && ft->sym &&
                     ft->sym->kind == SYM_RECORD)))
                    lerr(n, "`with` on a list or nested-record field "
                         "is not lowered yet");
                return;
            }
}

/* load record.field, where the record pointer is held in `slot` */
static int
with_load_field(int slot, struct sym *rec, const char *fname)
{
    struct ir_insn *ins;
    int base = new_temp(), off, addr = new_temp(), v = new_temp();
    load_slot(base, slot);
    off = lower_const(rec_field_woffset(rec, fname) * 4);
    ins = emit(IR_ADD); ins->dst = addr; ins->a = base; ins->b = off;
    ins = emit(IR_LW); ins->dst = v; ins->a = addr;
    return v;
}

/* store `val` into record.field, where the record pointer is held in `slot` */
static void
with_store_field(int slot, struct sym *rec, const char *fname, int val)
{
    struct ir_insn *ins;
    int base = new_temp(), off, addr = new_temp();
    load_slot(base, slot);
    off = lower_const(rec_field_woffset(rec, fname) * 4);
    ins = emit(IR_ADD); ins->dst = addr; ins->a = base; ins->b = off;
    ins = emit(IR_SW); ins->a = addr; ins->b = val;
}

/* `a with (x, y) = b`: compare only the named fields (D3). Either operand
 * may be a bare record or a matching slice; the field set is the slice's. */
static int
lower_with_compare(struct node *n)
{
    struct ir_insn *ins;
    struct node *w = n->a->kind == N_WITH ? n->a : n->b, *f;
    struct sym *rec = node_record_sym(w->a);
    int aslot = alloc_slot(4), bslot = alloc_slot(4), accslot = alloc_slot(4), r;
    if (n->op != T_EQ && n->op != T_NE)
        lerr(n, "records compare only with `=` / `<>`");
    store_slot(lower_expr(n->a), aslot);        /* N_WITH lowers to its base */
    store_slot(lower_expr(n->b), bslot);
    store_slot(lower_const(1), accslot);        /* acc = true */
    for (f = w->b; f; f = f->next) {
        int av, bv, e, acc, nacc;
        with_field_guard(n, rec, f->name);
        av = with_load_field(aslot, rec, f->name);
        bv = with_load_field(bslot, rec, f->name);
        if (rec_field_is_str(rec, f->name))
            e = str_call2("__exc_str_eq", av, bv);      /* a call: reload acc */
        else {
            e = new_temp();
            ins = emit(IR_CMPEQ); ins->dst = e; ins->a = av; ins->b = bv;
        }
        acc = new_temp(); load_slot(acc, accslot);
        nacc = new_temp();
        ins = emit(IR_AND); ins->dst = nacc; ins->a = acc; ins->b = e;
        store_slot(nacc, accslot);
    }
    r = new_temp(); load_slot(r, accslot);
    if (n->op == T_NE) {                         /* <> is not (=) */
        int one = lower_const(1), d = new_temp();
        ins = emit(IR_XOR); ins->dst = d; ins->a = r; ins->b = one;
        return d;
    }
    return r;
}

/* `p with (x, y) = source`: copy only the named fields from source into p
 * (D3, an in-place per-field copy). A shallow word copy per field, matching
 * whole-record assignment. */
static void
lower_with_assign(struct node *tgt, struct node *rhs)
{
    struct sym *rec = node_record_sym(tgt->a);
    int dslot = alloc_slot(4), sslot = alloc_slot(4);
    struct node *f;
    store_slot(lower_expr(tgt->a), dslot);      /* the sliced record p */
    store_slot(lower_expr(rhs), sslot);          /* source (N_WITH -> base) */
    for (f = tgt->b; f; f = f->next) {
        with_field_guard(tgt, rec, f->name);
        with_store_field(dslot, rec, f->name,
                         with_load_field(sslot, rec, f->name));
    }
}

/* Build the str-field bitmask over a record's FLATTENED word positions for
 * `__exc_rec_eq` (records.md value equality). Records nest flat, so a nested
 * record's str leaves land at their own flat positions; a scalar/obj/enum
 * leaf is a plain word compare. A list field is a pointer to elements and
 * cannot be compared this way yet, so it errors. Returns the next flat word
 * index. The mask is one target word, so a str leaf past bit 31 is rejected. */
static int
rec_eq_strmask(struct node *n, struct sym *rec, int w, long *mask)
{
    struct node *f;
    for (f = rec->decl ? rec->decl->a : NULL; f; f = f->next) {
        struct ex_type *ft = f->type ? lstrip(f->type) : NULL;
        struct sym *sub;
        if (ft && ft->kind == T_TSTR) {
            if (w >= 32)
                lerr(n, "record has too many words before a str field to "
                        "compare with `=`");
            *mask |= (1L << w);
            w++;
        } else if (ft && ft->kind == T_TLIST) {
            lerr(n, "record `=` with a list field is not lowered yet");
        } else if ((sub = type_record_sym(ft))) {
            w = rec_eq_strmask(n, sub, w, mask);   /* flat: recurse in place */
        } else {
            w++;                            /* int / bool / decimal / obj / enum */
        }
    }
    return w;
}

static int
lower_binop(struct node *n)
{
    struct ir_insn *ins;
    int l, r, irop;

    switch (n->op) {
    case T_AND:
    case T_OR:
        return lower_bool(n);       /* short-circuit via the cond path */
    case T_XOR:
        l = lower_bool(n->a);
        r = lower_bool(n->b);
        ins = emit(IR_XOR);
        ins->dst = new_temp();
        ins->a = l;
        ins->b = r;
        return ins->dst;
    default:
        break;
    }

    /* set membership (set-of.md D6): `s in t` is subset `(s & t) == s`,
     * `s overlaps t` is non-empty intersection `(s & t) != 0`. Both operands
     * lower in set-mode (a bare member is a singleton bit). */
    if ((n->op == T_IN || n->op == T_OVERLAPS) &&
        node_set_enum(n->b)) {
        struct sym *sen = node_set_enum(n->b);
        int s = lower_set(n->a, sen);
        int t = lower_set(n->b, sen);
        int both = new_temp(), r = new_temp();
        ins = emit(IR_AND); ins->dst = both; ins->a = s; ins->b = t;
        if (n->op == T_OVERLAPS) {
            int zero = lower_const(0);
            ins = emit(IR_CMPNE); ins->dst = r; ins->a = both; ins->b = zero;
        } else {
            ins = emit(IR_CMPEQ); ins->dst = r; ins->a = both; ins->b = s;
        }
        return r;
    }

    /* `in` dispatches on the right operand (the container), before the
     * element-type dispatch below (a str element would misroute otherwise). */
    if (n->op == T_IN) {
        if (node_is_list(n->b)) {
            struct sym *erec = n->b->type ?
                               type_record_sym(n->b->type->inner) : NULL;
            int holds_str = n->b->type->inner &&
                            n->b->type->inner->kind == T_TSTR;
            int list, val;
            if (erec) {                 /* list of records: value membership */
                long strmask = 0;
                int nfc, mc;
                list = lower_expr(n->b);
                val = lower_expr(n->a);
                rec_eq_strmask(n, erec, 0, &strmask);
                nfc = lower_const(rec_flat_words(erec));  /* consts before args */
                mc = lower_const((long)strmask);
                ins = emit(IR_ARG); ins->a = list; ins->imm = 0;
                ins = emit(IR_ARG); ins->a = val;  ins->imm = 1;
                ins = emit(IR_ARG); ins->a = nfc;  ins->imm = 2;
                ins = emit(IR_ARG); ins->a = mc;   ins->imm = 3;
                ins = emit(IR_CALL);
                ins->dst = new_temp();
                ins->sym = arena_strdup(la, "__exc_list_contains_rec");
                ins->nargs = 4;
                return ins->dst;
            }
            list = lower_expr(n->b);
            val = lower_expr(n->a);
            return str_call2(holds_str ? "__exc_list_contains_str"
                                       : "__exc_list_contains", list, val);
        }
        if (node_is_str(n->b)) {        /* substr in str: found position != 0 */
            int hay = lower_expr(n->b);
            int needle = lower_expr(n->a);
            int pos = str_call2("__exc_str_find", hay, needle);
            int zero = lower_const(0);
            ins = emit(IR_CMPNE);
            ins->dst = new_temp();
            ins->a = pos;
            ins->b = zero;
            return ins->dst;
        }
        lerr(n, "`in` needs a list or str on the right");
    }

    if (node_is_float(n->a) || node_is_float(n->b))
        return lower_fbinop(n);
    if (node_is_dec(n->a) || node_is_dec(n->b))
        return lower_decbinop(n);
    if (node_is_str(n->a) || node_is_str(n->b))
        return lower_strbinop(n);
    if (node_is_list(n->a) || node_is_list(n->b)) {     /* xs + ys concat */
        int l = lower_expr(n->a);
        int r = lower_expr(n->b);
        if (n->op == T_PLUS)
            return str_call2("__exc_list_concat", l, r);
        lerr(n, "list operator %s is not lowered yet", tok_str(n->op));
    }
    if (n->a->kind == N_WITH || n->b->kind == N_WITH)   /* record field slice */
        return lower_with_compare(n);
    {                                   /* record `=` / `<>` : value equality */
        struct sym *rec = node_record_sym(n->a);
        if (!rec)
            rec = node_record_sym(n->b);
        if (rec) {
            long strmask = 0;
            int nf, eq, l, r, nfc, mc;
            if (n->op != T_EQ && n->op != T_NE)
                lerr(n, "records compare only with `=` / `<>`");
            /* flat layout: compare the flattened words, with str leaves (at
             * any nesting depth) flagged for content comparison */
            rec_eq_strmask(n, rec, 0, &strmask);
            nf = rec_flat_words(rec);
            l = lower_expr(n->a);       /* compute operands, then constants */
            r = lower_expr(n->b);
            nfc = lower_const(nf);
            mc = lower_const((long)strmask);
            ins = emit(IR_ARG); ins->a = l;   ins->imm = 0;
            ins = emit(IR_ARG); ins->a = r;   ins->imm = 1;
            ins = emit(IR_ARG); ins->a = nfc; ins->imm = 2;
            ins = emit(IR_ARG); ins->a = mc;  ins->imm = 3;
            ins = emit(IR_CALL);
            ins->dst = eq = new_temp();
            ins->sym = arena_strdup(la, "__exc_rec_eq");
            ins->nargs = 4;
            if (n->op == T_NE) {        /* != is not (==) */
                int one = lower_const(1), d = new_temp();
                ins = emit(IR_XOR);
                ins->dst = d;
                ins->a = eq;
                ins->b = one;
                return d;
            }
            return eq;
        }
    }

    l = lower_expr(n->a);
    r = lower_expr(n->b);
    if (n->op == T_SLASH || n->op == T_PERCENT)
        div_checks(n, l, r);
    switch (n->op) {
    case T_PLUS:    irop = IR_ADD;   break;
    case T_MINUS:   irop = IR_SUB;   break;
    case T_STAR:    irop = IR_MUL;   break;
    case T_SLASH:   irop = IR_DIVS;  break;
    case T_PERCENT: irop = IR_MODS;  break;
    case T_EQ:      irop = IR_CMPEQ; break;
    case T_NE:      irop = IR_CMPNE; break;
    case T_LT:      irop = IR_CMPLTS; break;
    case T_LE:      irop = IR_CMPLES; break;
    case T_GT:      irop = IR_CMPGTS; break;
    case T_GE:      irop = IR_CMPGES; break;
    default:
        lerr(n, "operator %s is not lowered yet", tok_str(n->op));
    }
    ins = emit(irop);
    ins->dst = new_temp();
    ins->a = l;
    ins->b = r;
    return ins->dst;
}

/* Call a host helper that consumes the already-lowered value `v` of node
 * `e`, choosing the routine by e's static type: a str passes its pointer, a
 * float goes via IR_FARG, a fixed passes (value, frac), a bool passes the
 * word (rendered true/false), and a plain int passes the word. A NULL name
 * for a kind means "identity" (return v, no call): tostr uses it for str.
 * Returns the call's result temp (or v for the identity case). */
static int
emit_value_call(struct node *e, int v, const char *sfn, const char *bfn,
                const char *ifn, const char *ffn, const char *xfn)
{
    struct ir_insn *ins;
    const char *fn;
    int argc = 1;

    if (node_is_str(e)) {
        if (!sfn)
            return v;
        fn = sfn;
        ins = emit(IR_ARG);
        ins->a = v;
        ins->imm = 0;
    } else if (node_is_float(e)) {
        fn = ffn;
        ins = emit(IR_FARG);
        ins->a = v;
        ins->imm = 0;
    } else if (node_is_dec(e)) {        /* the scaled word */
        fn = xfn;
        ins = emit(IR_ARG);
        ins->a = v;
        ins->imm = 0;
    } else if (node_is_bool(e)) {       /* true / false */
        fn = bfn;
        ins = emit(IR_ARG);
        ins->a = v;
        ins->imm = 0;
    } else {                            /* int */
        fn = ifn;
        ins = emit(IR_ARG);
        ins->a = v;
        ins->imm = 0;
    }
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(la, fn);
    ins->nargs = argc;
    return ins->dst;
}

static int lower_send_to(int recv, const char *name, struct node *argn);
static int lower_self(void);

/* the address of a `shared` argument (shared-params.md): a local or by-value
 * param is its slot address (IR_ADL), a forwarded shared param passes on the
 * address it already holds, a self field is its global's address. */
static int
lower_addr_of(struct node *ve)
{
    struct ir_insn *ins;
    struct sym *s = ve->sym;
    int r;

    if (s && (s->kind == SYM_LOCAL || s->kind == SYM_PARAM)) {
        int slot = find_slot(s);
        ins = emit(is_shared_param(s) ? IR_LDL : IR_ADL);
        ins->dst = r = new_temp();
        ins->slot = slot;
        return r;
    }
    if (s && s->kind == SYM_FIELD)
        return field_addr(s);          /* the caller's own field slot */

    lerr(ve, "this cannot be passed as a `shared` argument");
}

static int
lower_call(struct node *n)
{
    struct sym *callee = n->a->kind == N_NAME ? n->a->sym : NULL;
    struct ir_insn *ins;
    int args[32], nargs = 0;

    /* record construction Point(1, 2) -> a fresh arena block with each field
     * stored positionally, missing trailing fields taking their default
     * (records.md; named args deferred). */
    if (callee && callee->kind == SYM_RECORD) {
        int nf = rec_flat_words(callee), blkslot = alloc_slot(4), blk, woff = 0;
        struct node *f = callee->decl ? callee->decl->a : NULL;
        struct node *ar = n->b;
        int named = ar && ar->alias;

        ins = emit(IR_ARG); ins->a = lower_const(nf); ins->imm = 0;
        ins = emit(IR_CALL);
        ins->dst = blk = new_temp();
        ins->sym = arena_strdup(la, "__exc_rec_new");
        ins->nargs = 1;
        store_slot(blk, blkslot);
        for (; f; f = f->next) {
            struct node *ve;             /* the value for this field */
            struct sym *sub = f->type ? type_record_sym(f->type) : NULL;
            int fw = sub ? rec_flat_words(sub) : 1;  /* the field's word span */
            int val, base, addr, off;
            if (named) {                 /* find the named arg, else default */
                ve = f->a;
                for (struct node *a = ar; a; a = a->next)
                    if (a->alias && ex_ci_eq(a->alias, f->name)) { ve = a; break; }
            } else {
                ve = ar ? ar : f->a;     /* positional: arg, else default */
                if (ar)
                    ar = ar->next;
            }
            if (!ve) {                   /* omitted: the zero-init block stands */
                woff += fw;
                continue;
            }
            val = lower_expr(ve);
            base = new_temp();
            load_slot(base, blkslot);           /* reload after a field call */
            off = lower_const(woff * 4);        /* compute operands first */
            addr = new_temp();
            ins = emit(IR_ADD);
            ins->dst = addr;
            ins->a = base;
            ins->b = off;
            if (sub) {              /* nested record: blit its flat words inline */
                int fwc = lower_const(fw);      /* const before the args */
                ins = emit(IR_ARG); ins->a = addr; ins->imm = 0;
                ins = emit(IR_ARG); ins->a = val;  ins->imm = 1;
                ins = emit(IR_ARG); ins->a = fwc;  ins->imm = 2;
                ins = emit(IR_CALL);
                ins->sym = arena_strdup(la, "__exc_rec_blit");
                ins->nargs = 3;
            } else {
                ins = emit(IR_SW);
                ins->a = addr;
                ins->b = val;
            }
            woff += fw;
        }
        blk = new_temp();
        load_slot(blk, blkslot);
        return blk;
    }

    /* spawn(ClassName) -> __exc_spawn(&Class__desc) : create an actor.
     * Other spawn forms (a template ref) are a host power, not lowered. */
    if (n->a->kind == N_NAME && !callee && ex_ci_eq(n->a->name, "spawn") &&
        n->b && !n->b->next && n->b->kind == N_NAME && n->b->sym &&
        n->b->sym->kind == SYM_CLASS) {
        int desc;
        ins = emit(IR_LEA);
        ins->dst = desc = new_temp();
        ins->sym = desc_symbol(n->b->sym->name);
        ins = emit(IR_ARG);
        ins->a = desc;
        ins->imm = 0;
        ins = emit(IR_CALL);
        ins->dst = new_temp();
        ins->sym = arena_strdup(la, "__exc_spawn");
        ins->nargs = 1;
        return ins->dst;
    }

    /* List builtins (immutable: append/set return a fresh list). len reads
     * word 0, which is the count for a list and the length for a str. */
    if (n->a->kind == N_NAME && !callee) {
        const char *bn = n->a->name;
        struct node *a0 = n->b;

        if (ex_ci_eq(bn, "length") && a0 && !a0->next) {
            int x = lower_expr(a0);
            if (node_is_str(a0)) {      /* a str's length is its code-point
                                         * count (R7), not word 0's byte len */
                ins = emit(IR_ARG); ins->a = x; ins->imm = 0;
                ins = emit(IR_CALL);
                ins->dst = new_temp();
                ins->sym = arena_strdup(la, "__exc_str_len");
                ins->nargs = 1;
                return ins->dst;
            }
            ins = emit(IR_LW);          /* a list's count is word 0 */
            ins->dst = new_temp();
            ins->a = x;
            return ins->dst;
        }
        /* bytes(x): the raw byte size (text-encoding.md R7 D5). A str's byte
         * length is word 0; a list's is its count (word 0) times the 4-byte
         * word-sized element. A low-level accessor beside len for a runtime
         * author sizing a transfer or copy. */
        if (ex_ci_eq(bn, "bytes") && a0 && !a0->next) {
            int x = lower_expr(a0), w = new_temp(), four, r;
            ins = emit(IR_LW);
            ins->dst = w;
            ins->a = x;
            if (node_is_str(a0))
                return w;
            four = lower_const(4);
            r = new_temp();
            ins = emit(IR_MUL);
            ins->dst = r;
            ins->a = w;
            ins->b = four;
            return r;
        }
        /* get(rec, "field"): the compile-time field accessor a field-walking
         * macro emits (record-introspection.md). Reads like `rec.field`. */
        if (ex_ci_eq(bn, "get") && a0 && a0->next && !a0->next->next) {
            struct sym *rec = node_record_sym(a0);
            const char *fname = a0->next->sval;
            int base = lower_expr(a0);
            int off = lower_const(rec_field_woffset(rec, fname) * 4);
            int addr = new_temp(), d;
            ins = emit(IR_ADD); ins->dst = addr; ins->a = base; ins->b = off;
            if (rec_field_rec(rec, fname))
                return addr;            /* nested record: interior pointer */
            ins = emit(IR_LW); ins->dst = d = new_temp(); ins->a = addr;
            return d;
        }
        if (ex_ci_eq(bn, "append") && a0 && a0->next && !a0->next->next) {
            int l = lower_expr(a0);
            int v = copy_if_record(a0->next, lower_expr(a0->next));
            ins = emit(IR_ARG); ins->a = l; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = v; ins->imm = 1;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_list_append");
            ins->nargs = 2;
            return ins->dst;
        }
        if (ex_ci_eq(bn, "set") && a0 && a0->next && a0->next->next &&
            !a0->next->next->next) {
            int l = lower_expr(a0);
            int i = lower_expr(a0->next);
            int v = copy_if_record(a0->next->next, lower_expr(a0->next->next));
            ins = emit(IR_ARG); ins->a = l; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = i; ins->imm = 1;
            ins = emit(IR_ARG); ins->a = v; ins->imm = 2;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_list_set");
            ins->nargs = 3;
            return ins->dst;
        }
        if (ex_ci_eq(bn, "find") && a0 && a0->next && !a0->next->next) {
            /* find(sub, s) returns maybe int: __exc_str_find yields the
             * 1-based position or 0, which IS the null-word protocol,
             * so the failure check is one branch and nothing boxes */
            int sub = lower_expr(a0);
            int hay = lower_expr(a0->next);
            int r;

            ins = emit(IR_ARG); ins->a = hay; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = sub; ins->imm = 1;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_str_find");
            ins->nargs = 2;
            r = ins->dst;
            fail_if_zero(r, n, "find");
            return r;                   /* the plain 1-based position */
        }
        if (ex_ci_eq(bn, "delete") && a0 && a0->next && !a0->next->next) {
            int l = lower_expr(a0);
            int i = lower_expr(a0->next);
            ins = emit(IR_ARG); ins->a = l; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = i; ins->imm = 1;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_list_delete");
            ins->nargs = 2;
            return ins->dst;
        }
        if (ex_ci_eq(bn, "prepend") && a0 && a0->next && !a0->next->next) {
            int l = lower_expr(a0);
            int v = copy_if_record(a0->next, lower_expr(a0->next));
            ins = emit(IR_ARG); ins->a = l; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = v; ins->imm = 1;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_list_prepend");
            ins->nargs = 2;
            return ins->dst;
        }
        if (ex_ci_eq(bn, "insert") && a0 && a0->next && a0->next->next &&
            !a0->next->next->next) {
            int l = lower_expr(a0);
            int i = lower_expr(a0->next);
            int v = copy_if_record(a0->next->next, lower_expr(a0->next->next));
            ins = emit(IR_ARG); ins->a = l; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = i; ins->imm = 1;
            ins = emit(IR_ARG); ins->a = v; ins->imm = 2;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_list_insert");
            ins->nargs = 3;
            return ins->dst;
        }
        if (ex_ci_eq(bn, "reverse") && a0 && !a0->next) {
            int l = lower_expr(a0);
            ins = emit(IR_ARG); ins->a = l; ins->imm = 0;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_list_reverse");
            ins->nargs = 1;
            return ins->dst;
        }
        /* first/last are fallible point access (list-ops.md): first is
         * xs[1], last is xs[count]; the empty list fails through the same
         * index_check as `xs[i]`. rest is the slice xs[2 to count]. */
        if ((ex_ci_eq(bn, "first") || ex_ci_eq(bn, "last")) &&
            a0 && !a0->next) {
            int bslot = alloc_slot(4), islot = alloc_slot(4), b, i;
            store_slot(lower_expr(a0), bslot);
            if (ex_ci_eq(bn, "first")) {
                store_slot(lower_const(1), islot);
            } else {                    /* index = count (word 0) */
                int cnt = new_temp();
                b = new_temp();
                load_slot(b, bslot);
                ins = emit(IR_LW); ins->dst = cnt; ins->a = b;
                store_slot(cnt, islot);
            }
            index_check(n, bslot, islot, 0);    /* empty -> fail */
            b = new_temp(); i = new_temp();
            load_slot(b, bslot); load_slot(i, islot);
            return lower_list_at(b, i);
        }
        if (ex_ci_eq(bn, "rest") && a0 && !a0->next) {
            int b = lower_expr(a0), cnt = new_temp(), lo;
            ins = emit(IR_LW); ins->dst = cnt; ins->a = b;   /* count */
            lo = lower_const(2);
            ins = emit(IR_ARG); ins->a = b;   ins->imm = 0;
            ins = emit(IR_ARG); ins->a = lo;  ins->imm = 1;
            ins = emit(IR_ARG); ins->a = cnt; ins->imm = 2;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_list_slice");
            ins->nargs = 3;
            return ins->dst;
        }
    }

    if (!callee || (callee->kind != SYM_FUNC && callee->kind != SYM_VERB))
        lerr(n, "only calls to a sibling verb or func are lowered yet");

    /* a bare call to a sibling verb dispatches (a self-send), so a
     * subclass override takes effect; funcs stay direct static calls */
    if (callee->kind == SYM_VERB)
        return lower_send_to(lower_self(), callee->name, n->b);

    {
        struct node *param = callee->decl ? callee->decl->a : NULL;
        int named = n->b && n->b->alias;
        struct node *pos = n->b;        /* positional cursor */
        int argf[32];

        /* iterate parameters; take each one's argument (named by alias, or the
         * next positional) or its compile-time default (records.md). */
        for (; param; param = param->next) {
            struct ex_type *pt = param->type;
            struct node *ve = param->a;         /* default */
            if (named) {
                for (struct node *a = n->b; a; a = a->next)
                    if (a->alias && ex_ci_eq(a->alias, param->name))
                        { ve = a; break; }
            } else if (pos) {
                ve = pos;
                pos = pos->next;
            }
            if (!ve)
                lerr(n, "missing argument to `%s`", callee->name);
            if (nargs >= 32)
                lerr(n, "too many arguments");
            {
                struct sym *prec = pt ? type_record_sym(pt) : NULL;
                if (param->flags & NF_SHARED)   /* pass the caller's address */
                    args[nargs] = lower_addr_of(ve);
                else if (prec && param->c)      /* field-restricted view: no copy */
                    args[nargs] = lower_expr(ve);   /* by reference (D4) */
                else if (prec) {                /* plain record param: value copy */
                    int v = lower_expr(ve);
                    if (!is_record_ctor(ve))
                        v = str_call2("__exc_rec_copy", v,
                                      lower_const(rec_flat_words(prec)));
                    args[nargs] = v;
                } else if (pt && pt->kind == ET_MAYBE)
                    args[nargs] = capture_maybe(ve, pt);
                else
                    args[nargs] = coerce(lower_expr(ve), ve, pt);
            }
            argf[nargs] = (param->flags & NF_SHARED) ? 0 : is_ftype(pt);
            nargs++;
        }
        for (int i = 0; i < nargs; i++) {
            ins = emit(argf[i] ? IR_FARG : IR_ARG);
            ins->a = args[i];
            ins->imm = i;
        }
    }
    ins = emit(is_ftype(callee->type) ? IR_FCALL : IR_CALL);
    ins->dst = new_temp();
    ins->sym = member_symbol(cur_class, callee->name);
    ins->nargs = nargs;
    if (callee->type && callee->type->kind == ET_MAYBE)
        /* fallible call */
        return unwrap_maybe(ins->dst, callee->type, n, callee->name);
    return ins->dst;               /* can-fail: the word IS the bool */
}

/* A verb send lowers to the host dispatch primitive:
 *     __exc_send(recv, selector, argc, argv)
 * The host resolves the selector against the receiver's actual class
 * descriptor, so self, cross-object, inherited, and overridden dispatch
 * all go through this one call. Arguments are marshaled into a flat argv
 * buffer on the frame; the receiver of a self-send is the __exc_self
 * handle (lowered by the N_SELF case in lower_expr). */
static int
lower_send_to(int recv, const char *name, struct node *argn)
{
    struct ir_insn *ins;
    int args[32], nargs = 0;
    int argv, callargs[4];

    for (struct node *a = argn; a; a = a->next) {
        if (nargs >= 32)
            lerr(a, "too many arguments");
        args[nargs++] = lower_expr(a);
    }

    if (nargs) {
        int slot = alloc_slot(nargs * 4);
        int base = new_temp();
        ins = emit(IR_ADL);
        ins->dst = base;
        ins->slot = slot;
        for (int i = 0; i < nargs; i++) {
            int addr = base;
            if (i) {
                int off = lower_const((long)i * 4);
                ins = emit(IR_ADD);
                ins->dst = addr = new_temp();
                ins->a = base;
                ins->b = off;
            }
            ins = emit(IR_SW);
            ins->a = addr;
            ins->b = args[i];
        }
        argv = base;
    } else {
        argv = lower_const(0);
    }

    callargs[0] = recv;
    callargs[1] = lower_const(sel_id(name));
    callargs[2] = lower_const(nargs);
    callargs[3] = argv;
    for (int i = 0; i < 4; i++) {
        ins = emit(IR_ARG);
        ins->a = callargs[i];
        ins->imm = i;
    }
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(la, "__exc_send");
    ins->nargs = 4;
    return ins->dst;
}

static int
lower_send(struct node *n)
{
    return lower_send_to(lower_expr(n->a), n->name, n->b);
}

/* self as a value: the running actor handle, __exc_self */
static int
lower_self(void)
{
    struct ir_insn *ins;
    int addr = new_temp();

    ins = emit(IR_LEA);
    ins->dst = addr;
    ins->sym = arena_strdup(la, "__exc_self");
    ins = emit(IR_LW);
    ins->dst = new_temp();
    ins->a = addr;
    return ins->dst;
}

/* the compile-time value of a match label (typecheck guarantees the
 * node kinds; a const name must have an integer initializer) */
static long
match_label_value(struct node *e)
{
    switch (e->kind) {
    case N_NUM:
        return e->ival;
    case N_BOOL:
        return e->ival ? 1 : 0;
    case N_FLOAT:                       /* a decimal label: scaled */
        return e->ival;
    case N_NAME:
        if (e->sym && e->sym->kind == SYM_ENUM_MEMBER)   /* enum label ordinal */
            return e->sym->decl ? e->sym->decl->ival : 0;
        if (e->sym && e->sym->kind == SYM_CONST && e->sym->decl &&
            e->sym->decl->a &&
            (e->sym->decl->a->kind == N_NUM ||
             (e->sym->decl->a->kind == N_FLOAT &&
              node_is_dec(e->sym->decl->a))))
            return e->sym->decl->a->ival;
        lerr(e, "match label `%s` is not a lowered constant yet", e->name);
    default:
        lerr(e, "match label does not lower");
    }
}

/* emit one arm's label tests against the spilled subject: branch to
 * lbody on any match, fall through when none match. Each test reloads
 * the subject into a fresh temp (temps are per-block SSA). */
static void
lower_arm_tests(struct node *arm, int subjslot, int lbody)
{
    struct ir_insn *ins;

    for (struct node *v = arm->a; v; v = v->next) {
        if (v->kind == N_RANGE) {       /* lo <= subj <= hi */
            int lfail = new_label();
            int s = new_temp(), k, c;

            load_slot(s, subjslot);
            k = lower_const(match_label_value(v->a));
            c = new_temp();
            ins = emit(IR_CMPLTS);      /* subj < lo: no match */
            ins->dst = c;
            ins->a = s;
            ins->b = k;
            ins = emit(IR_BNZ);
            ins->a = c;
            ins->label = lfail;
            s = new_temp();
            load_slot(s, subjslot);
            k = lower_const(match_label_value(v->b));
            c = new_temp();
            ins = emit(IR_CMPGTS);      /* subj > hi: no match */
            ins->dst = c;
            ins->a = s;
            ins->b = k;
            ins = emit(IR_BNZ);
            ins->a = c;
            ins->label = lfail;
            emit_jmp(lbody);
            emit_label(lfail);
        } else {
            int s = new_temp(), k, c;

            load_slot(s, subjslot);
            k = lower_const(match_label_value(v));
            c = new_temp();
            ins = emit(IR_CMPEQ);
            ins->dst = c;
            ins->a = s;
            ins->b = k;
            ins = emit(IR_BNZ);
            ins->a = c;
            ins->label = lbody;
        }
    }
}

/* the shared fallback tail for value-producing choosers (select and
 * the match expression): store the `otherwise` value into the result slot,
 * or trap when there is no `otherwise`. The trap is already on the cold
 * no-match path, so it reports inline: n names the chooser, subjslot
 * holds the subject/index that chose nothing. */
static void
lower_else_or_trap(struct node *n, struct node *els, struct ex_type *ty,
                   int slot, int isf, int subjslot)
{
    struct ir_insn *ins;

    if (els) {
        int v = coerce(lower_expr(els), els, ty);
        ins = emit(isf ? IR_FSTL : IR_STL);
        ins->a = v;
        ins->slot = slot;
    } else {
        int subj = new_temp(), z;
        load_slot(subj, subjslot);
        z = lower_const(0);
        emit_trap_call(EXC_TRAP_NO_BRANCH, n->line, NULL, subj, z);
    }
}

static int
lower_expr(struct node *n)
{
    struct ir_insn *ins;
    int t;

    switch (n->kind) {
    case N_NUM:
        return lower_const(n->ival);
    case N_BOOL:
        return lower_const(n->ival ? 1 : 0);
    case N_NAME:
        return lower_name(n);
    case N_BINOP:
        return lower_binop(n);
    case N_WITH:
        /* as an rvalue a slice is just the record; the field restriction
         * matters only where it is assigned or compared (record-slicing.md) */
        return lower_expr(n->a);
    case N_UNOP:
        if (n->op == T_NOT)
            return lower_bool(n);
        t = lower_expr(n->a);
        ins = emit(node_is_float(n) ? IR_FNEG : IR_NEG);   /* unary minus */
        ins->dst = new_temp();
        ins->a = t;
        return ins->dst;
    case N_CALL:
        return lower_call(n);
    case N_FLOAT:
        if (n->type && lstrip(n->type)->kind == ET_DEC)
            lerr(n, "unresolved decimal literal (checker bug)");
        if (node_is_dec(n))
            return lower_const(n->ival);    /* checker folded the scale */
        return lower_flt(n->fval);
    case N_STR:
        return lower_strlit(n->sval, n->slen);
    case N_TOSTR: {                     /* stringify an interpolation hole */
        struct ex_type *ht = n->a->type ? lstrip(n->a->type) : NULL;
        int v = lower_expr(n->a);
        if (ht && ht->kind == T_IDENT && ht->sym &&
            ht->sym->kind == SYM_ENUM) {
            /* an enum prints its member name via the per-enum table (D6) */
            char buf[256];
            int tbl = new_temp();
            snprintf(buf, sizeof buf, "%s__names", ht->sym->name);
            ins = emit(IR_LEA);
            ins->dst = tbl;
            ins->sym = arena_strdup(la, buf);
            ins = emit(IR_ARG); ins->a = v;   ins->imm = 0;
            ins = emit(IR_ARG); ins->a = tbl; ins->imm = 1;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_str_from_enum");
            ins->nargs = 2;
            return ins->dst;
        }
        return emit_value_call(n->a, v, NULL, "__exc_str_from_bool",
                               "__exc_str_from_int", "__exc_str_from_float",
                               "__exc_str_from_dec");
    }
    case N_THENELSE: {                  /* cond then A else B */
        int isf = is_ftype(n->type);
        int ltrue = new_label(), lfalse = new_label(), lend = new_label();
        int slot = alloc_slot(type_size(n->type));
        int r;

        lower_cond(n->a, ltrue, lfalse);
        emit_label(ltrue);
        {
            int v = coerce(lower_expr(n->b), n->b, n->type);
            ins = emit(isf ? IR_FSTL : IR_STL);
            ins->a = v;
            ins->slot = slot;
        }
        emit_jmp(lend);
        emit_label(lfalse);
        {
            int v = coerce(lower_expr(n->c), n->c, n->type);
            ins = emit(isf ? IR_FSTL : IR_STL);
            ins->a = v;
            ins->slot = slot;
        }
        emit_label(lend);
        r = new_temp();
        ins = emit(isf ? IR_FLDL : IR_LDL);
        ins->dst = r;
        ins->slot = slot;
        return r;
    }
    case N_SELECT: {                    /* select idx from ... else ... */
        int isf = is_ftype(n->type);
        int idxslot = alloc_slot(4);    /* idx is read across blocks: spill */
        int slot = alloc_slot(type_size(n->type));
        int lend = new_label();
        int i = 0, r;
        struct node *br;

        store_slot(lower_expr(n->a), idxslot);
        for (br = n->b; br; br = br->next, i++) {
            int lnext = new_label();
            int cmp = new_temp();
            int idx = new_temp();
            int k, v;
            load_slot(idx, idxslot);    /* fresh load per comparison block */
            k = lower_const(i);         /* compute before the compare uses it */
            ins = emit(IR_CMPEQ);       /* idx == i ? */
            ins->dst = cmp;
            ins->a = idx;
            ins->b = k;
            ins = emit(IR_BZ);          /* not this one: try the next */
            ins->a = cmp;
            ins->label = lnext;
            v = coerce(lower_expr(br), br, n->type);
            ins = emit(isf ? IR_FSTL : IR_STL);
            ins->a = v;
            ins->slot = slot;
            emit_jmp(lend);
            emit_label(lnext);
        }
        lower_else_or_trap(n, n->c, n->type, slot, isf, idxslot);
        emit_label(lend);
        r = new_temp();
        ins = emit(isf ? IR_FLDL : IR_LDL);
        ins->dst = r;
        ins->slot = slot;
        return r;
    }
    case N_MATCHEXPR: {                  /* case E of vals -> e ... else -> e */
        int isf = is_ftype(n->type);
        int subjslot = alloc_slot(4);   /* subject is read per test block */
        int slot = alloc_slot(type_size(n->type));
        int lend = new_label();
        int r;

        store_slot(lower_expr(n->a), subjslot);
        for (struct node *arm = n->b; arm; arm = arm->next) {
            int lbody = new_label(), lnext = new_label();

            lower_arm_tests(arm, subjslot, lbody);
            emit_jmp(lnext);
            emit_label(lbody);
            {
                int v = coerce(lower_expr(arm->b), arm->b, n->type);
                ins = emit(isf ? IR_FSTL : IR_STL);
                ins->a = v;
                ins->slot = slot;
            }
            emit_jmp(lend);
            emit_label(lnext);
        }
        lower_else_or_trap(n, n->c, n->type, slot, isf, subjslot);
        emit_label(lend);
        r = new_temp();
        ins = emit(isf ? IR_FLDL : IR_LDL);
        ins->dst = r;
        ins->slot = slot;
        return r;
    }
    case N_OTHERWISE: {                      /* A otherwise B: the general consumer */
        int rslot = alloc_slot(type_size(n->type));
        int isf = is_ftype(n->type);
        int lout = new_label(), lend = new_label();
        int save = cur_fail, v, r;

        cur_fail = lout;                /* any failure in A lands on B */
        v = coerce(lower_expr(n->a), n->a, n->type);
        cur_fail = save;
        ins = emit(isf ? IR_FSTL : IR_STL);
        ins->a = v;
        ins->slot = rslot;
        emit_jmp(lend);
        emit_label(lout);               /* the fallback */
        v = coerce(lower_expr(n->b), n->b, n->type);
        ins = emit(isf ? IR_FSTL : IR_STL);
        ins->a = v;
        ins->slot = rslot;
        emit_label(lend);
        r = new_temp();
        ins = emit(isf ? IR_FLDL : IR_LDL);
        ins->dst = r;
        ins->slot = rslot;
        return r;
    }
    case N_NOTHING:
        /* nothing IS the failure */
        emit_jmp(fail_target(EXC_TRAP_UNCONSUMED, n, "nothing",
                             -1, -1, 0));
        return lower_const(0);      /* unreachable value */
    case N_NIL:
        lerr(n, "nil is not lowered yet");
    case N_FIELD_ACC: {
        struct sym *rec;
        /* a qualified enum member `Color.red` is its 0-based ordinal, a small
         * int (enums.md D1); the ordinal was stamped on the member node at
         * resolve time */
        if (n->sym && n->sym->kind == SYM_ENUM_MEMBER)
            return lower_const(n->sym->decl ? n->sym->decl->ival : 0);
        rec = node_record_sym(n->a);
        if (rec) {                          /* record field: base ptr + offset */
            int base = lower_expr(n->a);
            int off = lower_const(rec_field_woffset(rec, n->name) * 4);
            int addr = new_temp(), d;       /* compute operands first */
            ins = emit(IR_ADD);
            ins->dst = addr;
            ins->a = base;
            ins->b = off;
            /* a nested record field is inline: its value is the interior
             * pointer (addr itself), not a word loaded from it */
            if (rec_field_rec(rec, n->name))
                return addr;
            ins = emit(IR_LW);
            ins->dst = d = new_temp();
            ins->a = addr;
            return d;
        }
        if (n->sym && n->sym->kind == SYM_FIELD) {
            struct lval lv = resolve_lval(n);
            int v = lval_load(&lv);
            if (n->sym->type && n->sym->type->kind == ET_MAYBE)
                v = unwrap_maybe(v, n->sym->type, n, n->sym->name);
            return v;
        }
        lerr(n, "only `self` field reads are lowered yet");
    }
    case N_SELF:
        return lower_self();
    case N_CMPCHAIN: {
        /* `a < b < c`: pairwise signed compares joined like `and`, each
         * operand evaluated once (spilled to a slot between links) and
         * short-circuiting to false on the first failed compare, so
         * later operands are not evaluated (typecheck restricts the
         * operands to int/bool/fixed, which share signed word compares) */
        int rslot = alloc_slot(4);
        int prevslot = alloc_slot(4);
        int lfalse = new_label(), lend = new_label();
        int r;

        store_slot(lower_expr(n->a), prevslot);
        for (struct node *l = n->b; l; l = l->next) {
            int cur = lower_expr(l->a);
            int prev = new_temp(), c = new_temp();
            int irop;

            switch (l->op) {
            case T_LT: irop = IR_CMPLTS; break;
            case T_LE: irop = IR_CMPLES; break;
            case T_GT: irop = IR_CMPGTS; break;
            case T_GE: irop = IR_CMPGES; break;
            default:   irop = IR_CMPEQ;  break;
            }
            load_slot(prev, prevslot);
            ins = emit(irop);
            ins->dst = c;
            ins->a = prev;
            ins->b = cur;
            store_slot(cur, prevslot);
            ins = emit(IR_BZ);
            ins->a = c;
            ins->label = lfalse;
        }
        store_slot(lower_const(1), rslot);
        emit_jmp(lend);
        emit_label(lfalse);
        store_slot(lower_const(0), rslot);
        emit_label(lend);
        r = new_temp();
        load_slot(r, rslot);
        return r;
    }
    case N_SEND:
        return lower_send(n);
    case N_CAST: {
        /* numeric conversions among int / float / decimal; other casts
         * are identity. float <-> decimal is not a single instruction
         * and is not lowered yet. */
        int df = node_is_float(n), dx = node_is_dec(n);
        int sf = node_is_float(n->a), sx = node_is_dec(n->a);
        t = lower_expr(n->a);
        if ((df && sx) || (dx && sf))
            lerr(n, "float<->decimal casts are not lowered yet");
        if (df && !sf)
            return emit_conv(t, IR_ITOF);           /* int -> float */
        if (dx && !sx)
            return to_dec(t, n->a);                 /* int -> decimal */
        if (!df && !dx && sf)
            return emit_conv(t, IR_FTOI);           /* float -> int */
        if (!df && !dx && sx)
            return dec_narrow(t);                   /* decimal -> int */
        return t;
    }
    case N_ISTEST:
        lerr(n, "`is` type-tests are not lowered yet");
    case N_INDEX: {
        /* a fallible producer: out of range branches to the active
         * fail label (a consumer's, or the statement default) */
        int bslot = alloc_slot(4), islot = alloc_slot(4);
        int is_list = node_is_list(n->a);
        int b, i;

        if (!is_list && !node_is_str(n->a))
            lerr(n, "only string and list indexing is lowered yet");
        store_slot(lower_expr(n->a), bslot);
        store_slot(lower_expr(n->b), islot);
        index_check(n, bslot, islot, !is_list && node_is_str(n->a));
        b = new_temp();
        i = new_temp();
        load_slot(b, bslot);
        load_slot(i, islot);
        return is_list ? lower_list_at(b, i)
                       : str_call2("__exc_str_at", b, i);
    }
    case N_SLICE:
        if (node_is_str(n->a)) {        /* s[lo..hi] -> substring */
            int s = lower_expr(n->a);
            int lo = lower_expr(n->b);
            int hi = lower_expr(n->c);
            ins = emit(IR_ARG); ins->a = s;  ins->imm = 0;
            ins = emit(IR_ARG); ins->a = lo; ins->imm = 1;
            ins = emit(IR_ARG); ins->a = hi; ins->imm = 2;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_str_slice");
            ins->nargs = 3;
            return ins->dst;
        }
        if (node_is_list(n->a)) {       /* xs[lo..hi] -> sublist */
            int list = lower_expr(n->a);
            int lo = lower_expr(n->b);
            int hi = lower_expr(n->c);
            ins = emit(IR_ARG); ins->a = list; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = lo;   ins->imm = 1;
            ins = emit(IR_ARG); ins->a = hi;   ins->imm = 2;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_list_slice");
            ins->nargs = 3;
            return ins->dst;
        }
        lerr(n, "only string and list slicing is lowered yet");
    case N_DATALIT:
        return lower_listlit(n);
    case N_QUOTE:
        lerr(n, "data literals are not lowered yet");
    default:
        lerr(n, "expression kind %d is not lowered yet", n->kind);
    }
}

/* branch to ltrue when the condition holds, else fall to lfalse */
static void
lower_cond(struct node *n, int ltrue, int lfalse)
{
    if (n->kind == N_BINOP && n->op == T_AND) {
        int lmid = new_label();
        lower_cond(n->a, lmid, lfalse);
        emit_label(lmid);
        lower_cond(n->b, ltrue, lfalse);
        return;
    }
    if (n->kind == N_BINOP && n->op == T_OR) {
        int lmid = new_label();
        lower_cond(n->a, ltrue, lmid);
        emit_label(lmid);
        lower_cond(n->b, ltrue, lfalse);
        return;
    }
    if (n->kind == N_UNOP && n->op == T_NOT) {
        lower_cond(n->a, lfalse, ltrue);
        return;
    }
    {
        int t = lower_expr(n);          /* compute before emitting the branch */
        struct ir_insn *ins = emit(IR_BNZ);
        ins->a = t;
        ins->label = ltrue;
        emit_jmp(lfalse);
    }
}

/****************************************************************
 * Statement lowering
 ****************************************************************/

static void
lower_stmts(struct node *list)
{
    for (struct node *s = list; s; s = s->next)
        lower_stmt(s);
}

static void
lower_stmt(struct node *n)
{
    struct ir_insn *ins;

    /* an unconsumed failure in this statement goes to the function's
     * shared label: a trap, or the fail epilogue in a fallible func */
    cur_fail = default_fail();

    switch (n->kind) {
    case N_VAR: {
        struct sym *rec = type_record_sym(n->sym->type);
        struct sym *sen = type_set_enum(n->sym->type);
        int is_f = is_ftype(n->sym->type);
        int slot = bind_slot(n->sym, is_f ? 8 : 4);
        if (n->a) {
            int v;
            if (sen) {          /* set of E: build the bitmask (set-of.md) */
                v = lower_set(n->a, sen);
                ins = emit(IR_STL);
                ins->a = v;
                ins->slot = slot;
                break;
            }
            if (rec) {          /* record: copy an alias (value semantics) */
                v = lower_expr(n->a);
                if (!is_record_ctor(n->a))
                    v = str_call2("__exc_rec_copy", v,
                                  lower_const(rec_flat_words(rec)));
            } else if (n->sym->type && n->sym->type->kind == ET_MAYBE)
                v = capture_maybe(n->a, n->sym->type);
            else
                v = coerce(lower_expr(n->a), n->a, n->sym->type);
            ins = emit(is_f ? IR_FSTL : IR_STL);
            ins->a = v;
            ins->slot = slot;
        }
        break;
    }
    case N_ASSIGN: {
        struct node *tgt = n->a;
        struct sym *frec;
        struct lval lv;
        /* record field slice p with (x, y) = source: per-field copy */
        if (tgt->kind == N_WITH) {
            lower_with_assign(tgt, n->b);
            break;
        }
        /* record field write p.x = v: store at base ptr + offset */
        if (tgt->kind == N_FIELD_ACC &&
            (frec = node_record_sym(tgt->a))) {
            struct sym *sub = rec_field_rec(frec, tgt->name);
            int val = coerce(lower_expr(n->b), n->b, tgt->type);
            int base = lower_expr(tgt->a);      /* p: a local load, no call */
            int off = lower_const(rec_field_woffset(frec, tgt->name) * 4);
            int addr = new_temp();
            ins = emit(IR_ADD);
            ins->dst = addr;
            ins->a = base;
            ins->b = off;
            if (sub) {              /* nested record: blit its flat words inline */
                int fwc = lower_const(rec_flat_words(sub));  /* const first */
                ins = emit(IR_ARG); ins->a = addr; ins->imm = 0;
                ins = emit(IR_ARG); ins->a = val;  ins->imm = 1;
                ins = emit(IR_ARG); ins->a = fwc;  ins->imm = 2;
                ins = emit(IR_CALL);
                ins->sym = arena_strdup(la, "__exc_rec_blit");
                ins->nargs = 3;
            } else {
                ins = emit(IR_SW);
                ins->a = addr;
                ins->b = val;
            }
            break;
        }
        lv = resolve_lval(tgt);
        {
            struct sym *sen = type_set_enum(lv.type);
            if (sen) {                  /* set assignment: build the bitmask */
                lval_store(&lv, lower_set(n->b, sen));
                break;
            }
        }
        if ((frec = type_record_sym(lv.type))) {   /* record var: copy alias */
            int v = lower_expr(n->b);
            if (!is_record_ctor(n->b))
                v = str_call2("__exc_rec_copy", v,
                              lower_const(rec_flat_words(frec)));
            lval_store(&lv, v);
        } else if (lv.type && lv.type->kind == ET_MAYBE)
            lval_store(&lv, capture_maybe(n->b, lv.type));
        else
            lval_store(&lv, coerce(lower_expr(n->b), n->b, lv.type));
        break;
    }
    case N_EXPR_STMT: {
        int v = lower_expr(n->a);
        if (n->a->kind == N_CALL && n->a->a->kind == N_NAME &&
            n->a->a->sym && n->a->a->sym->decl &&
            (n->a->a->sym->decl->flags & NF_CANFAIL))
            /* trap or propagate, never ignore */
            fail_if_zero(v, n->a, n->a->a->sym->name);
        break;
    }
    case N_ONFAIL: {
        /* `stmt on fail handler` (fallible-consumers.md): the handler label
         * is the fail target for the left action's fallible producer (the
         * same dynamically scoped mechanism `otherwise`/`if var` use). The left
         * is lowered inline rather than through lower_stmt, since that
         * resets cur_fail per statement; here cur_fail must reach the
         * producer. On success the left falls through and we skip the
         * handler; on failure the producer branches into it. */
        struct node *left = n->a;
        int lhandler = new_label(), lend = new_label();
        int save = cur_fail;
        cur_fail = lhandler;
        if (left->kind == N_ASSIGN) {
            struct lval lv = resolve_lval(left->a);
            if (lv.type && lv.type->kind == ET_MAYBE)
                lval_store(&lv, capture_maybe(left->b, lv.type));
            else
                lval_store(&lv,
                           coerce(lower_expr(left->b), left->b, lv.type));
        } else {                        /* N_EXPR_STMT */
            int v = lower_expr(left->a);
            if (left->a->kind == N_CALL && left->a->a->kind == N_NAME &&
                left->a->a->sym && left->a->a->sym->decl &&
                (left->a->a->sym->decl->flags & NF_CANFAIL))
                fail_if_zero(v, left->a, left->a->a->sym->name);
        }
        cur_fail = save;
        emit_jmp(lend);
        emit_label(lhandler);
        lower_stmt(n->b);
        emit_label(lend);
        break;
    }
    case N_IF: {
        int ltrue = new_label(), lfalse = new_label();
        if (n->name) {              /* if var i = fallible-expr */
            int slot = bind_slot(n->sym, 4);
            int save = cur_fail, v;
            cur_fail = lfalse;
            v = lower_expr(n->a);   /* plain payload on success */
            cur_fail = save;
            ins = emit(IR_STL);
            ins->a = v;
            ins->slot = slot;
            emit_jmp(ltrue);
        } else {
            lower_cond(n->a, ltrue, lfalse);
        }
        emit_label(ltrue);
        lower_stmts(n->b);
        if (n->c) {
            int lend = new_label();
            emit_jmp(lend);
            emit_label(lfalse);
            if (n->c->kind == N_IF)
                lower_stmt(n->c);   /* elseif chain */
            else
                lower_stmts(n->c);
            emit_label(lend);
        } else {
            emit_label(lfalse);
        }
        break;
    }
    case N_WHILE: {
        int ltop = new_label(), lbody = new_label(), lbrk = new_label();
        emit_label(ltop);
        if (n->name) {              /* while var i = fallible-expr */
            int slot = bind_slot(n->sym, 4);
            int save = cur_fail, v;
            cur_fail = lbrk;
            v = lower_expr(n->a);
            cur_fail = save;
            ins = emit(IR_STL);
            ins->a = v;
            ins->slot = slot;
            emit_jmp(lbody);
        } else {
            lower_cond(n->a, lbody, lbrk);
        }
        emit_label(lbody);
        loops[nloops].brk = lbrk;
        loops[nloops].cont = ltop;
        nloops++;
        lower_stmts(n->b);
        nloops--;
        emit_jmp(ltop);
        emit_label(lbrk);
        break;
    }
    case N_FOR: {
        int ltop = new_label(), lcont = new_label(), lbrk = new_label();

        if (n->b) {
            /* integer range: for i in lo .. hi (inclusive) */
            int slot = bind_slot(n->sym, 4);
            int hislot = alloc_slot(4);
            int cur, hi, cond, next, one;

            store_slot(lower_expr(n->a), slot);
            store_slot(lower_expr(n->b), hislot);

            emit_label(ltop);
            cur = new_temp();
            load_slot(cur, slot);
            hi = new_temp();
            load_slot(hi, hislot);
            ins = emit(IR_CMPLES);      /* cur <= hi ? */
            ins->dst = cond = new_temp();
            ins->a = cur;
            ins->b = hi;
            ins = emit(IR_BZ);
            ins->a = cond;
            ins->label = lbrk;

            loops[nloops].brk = lbrk;
            loops[nloops].cont = lcont;
            nloops++;
            lower_stmts(n->c);
            nloops--;

            emit_label(lcont);
            cur = new_temp();
            load_slot(cur, slot);
            one = lower_const(1);
            ins = emit(IR_ADD);
            ins->dst = next = new_temp();
            ins->a = cur;
            ins->b = one;
            store_slot(next, slot);
            emit_jmp(ltop);
            emit_label(lbrk);
        } else {
            /* iterate a list or a str: element i for i in 1..count, where
             * count is word 0 (a list's length / a str descriptor's len) */
            int is_list = node_is_list(n->a);
            int bslot = alloc_slot(4);          /* base pointer */
            int cntslot = alloc_slot(4);        /* count */
            int islot = alloc_slot(4);          /* 1-based index */
            int velslot = bind_slot(n->sym, type_size(n->sym->type));
            int base, cnt;

            if (!is_list && !node_is_str(n->a))
                lerr(n, "cannot iterate over this value");

            store_slot(lower_expr(n->a), bslot);
            base = new_temp();
            load_slot(base, bslot);
            cnt = new_temp();
            if (is_list) {              /* a list's count is word 0 */
                ins = emit(IR_LW);
                ins->dst = cnt;
                ins->a = base;
            } else {                    /* a str iterates by code point (R7) */
                ins = emit(IR_ARG); ins->a = base; ins->imm = 0;
                ins = emit(IR_CALL);
                ins->dst = cnt;
                ins->sym = arena_strdup(la, "__exc_str_len");
                ins->nargs = 1;
            }
            store_slot(cnt, cntslot);
            store_slot(lower_const(1), islot);

            emit_label(ltop);
            {                               /* i > count -> done */
                int i = new_temp(), c = new_temp(), cmp = new_temp();
                load_slot(i, islot);
                load_slot(c, cntslot);
                ins = emit(IR_CMPGTS);
                ins->dst = cmp;
                ins->a = i;
                ins->b = c;
                ins = emit(IR_BNZ);
                ins->a = cmp;
                ins->label = lbrk;
            }
            {                               /* loop var = base[i] */
                int b = new_temp(), i = new_temp(), elem;
                load_slot(b, bslot);
                load_slot(i, islot);
                elem = is_list ? lower_list_at(b, i)
                               : str_call2("__exc_str_at", b, i);
                store_slot(elem, velslot);
            }

            loops[nloops].brk = lbrk;
            loops[nloops].cont = lcont;
            nloops++;
            lower_stmts(n->c);
            nloops--;

            emit_label(lcont);
            {                               /* i = i + 1 */
                int i = new_temp(), one, next;
                load_slot(i, islot);
                one = lower_const(1);
                ins = emit(IR_ADD);
                ins->dst = next = new_temp();
                ins->a = i;
                ins->b = one;
                store_slot(next, islot);
            }
            emit_jmp(ltop);
            emit_label(lbrk);
        }
        break;
    }
    case N_RETURN:
        if (n->a && cur_ret_type && cur_ret_type->kind == ET_MAYBE) {
            /* success returns the boxed/null-word value; an uncaught
             * inner failure returns nothing (Icon, one level up) */
            int lf = new_label(), save = cur_fail, t, bx, z;
            cur_fail = lf;
            t = coerce(lower_expr(n->a), n->a, cur_ret_type->inner);
            cur_fail = save;
            bx = box_maybe(t, cur_ret_type);    /* compute, then return */
            emit(IR_RETV)->a = bx;
            emit_label(lf);
            z = lower_const(0);
            emit(IR_RETV)->a = z;
        } else if (n->a) {
            /* compute (and coerce) before emitting the return */
            int t = coerce(lower_expr(n->a), n->a, cur_ret_type);
            emit(cur_ret_float ? IR_FRETV : IR_RETV)->a = t;
        } else if (fn_fallible && !cur_ret_type) {
            int one = lower_const(1);           /* can fail: success */
            emit(IR_RETV)->a = one;
        } else {
            emit(IR_RET);
        }
        break;
    case N_FAIL: {
        int z = lower_const(0);                 /* the null word */
        emit(IR_RETV)->a = z;
        break;
    }
    case N_BREAK:
        if (!nloops)
            lerr(n, "break outside a loop");
        emit_jmp(loops[nloops - 1].brk);
        break;
    case N_CONTINUE:
        if (!nloops)
            lerr(n, "continue outside a loop");
        emit_jmp(loops[nloops - 1].cont);
        break;
    case N_MATCH: {
        int subjslot = alloc_slot(4);
        int lend = new_label();

        store_slot(lower_expr(n->a), subjslot);
        for (struct node *arm = n->b; arm; arm = arm->next) {
            int lbody = new_label(), lnext = new_label();

            lower_arm_tests(arm, subjslot, lbody);
            emit_jmp(lnext);
            emit_label(lbody);
            lower_stmts(arm->b);
            emit_jmp(lend);
            emit_label(lnext);
        }
        if (n->c) {
            lower_stmts(n->c);
        } else if (lex_trace_on()) {
            /* no match, no else: a no-op in release, but under -t the
             * silently-skipped match reports itself (runtime-errors.md,
             * the R2 trace event) */
            int subj = new_temp(), d;
            load_slot(subj, subjslot);
            ins = emit(IR_LEA);
            ins->dst = new_temp();
            ins->sym = emit_trapdesc(EXC_TRAP_NO_BRANCH, n->line, NULL);
            d = ins->dst;
            ins = emit(IR_ARG); ins->a = d; ins->imm = 0;
            ins = emit(IR_ARG); ins->a = subj; ins->imm = 1;
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(la, "__exc_trace_nomatch");
            ins->nargs = 2;
        }
        emit_label(lend);
        break;
    }
    case N_TRACE:
        /* the author channel (output.md): compiled only under -t, like
         * /// and the R2 no-match event; routed by static type to the
         * stderr trace helpers (the machinery log() built) */
        if (lex_trace_on()) {
            int v = lower_expr(n->a);
            emit_value_call(n->a, v, "__exc_trace_str", "__exc_trace_bool",
                            "__exc_trace_int", "__exc_trace_float",
                            "__exc_trace_dec");
        }
        break;
    case N_TRACE_CMT: {
        /* /// text: the lexer emits this token only under -t; it prints
         * its text on the author channel like `trace` does */
        struct ir_insn *c;
        int v = lower_strlit(n->sval, n->slen);
        c = emit(IR_ARG);
        c->a = v;
        c->imm = 0;
        c = emit(IR_CALL);
        c->dst = new_temp();
        c->sym = arena_strdup(la, "__exc_trace_str");
        c->nargs = 1;
        break;
    }
    default:
        lerr(n, "statement kind %d is not lowered yet", n->kind);
    }
}

/****************************************************************
 * Function and program lowering
 ****************************************************************/

static struct ir_func *
lower_member(struct node *cls, struct node *m)
{
    struct ir_func *fn;
    struct ir_insn *ins;
    char *mangled = member_symbol(cls->name, m->name);
    int nparams = 0;

    fn = ir_new_func(la, mangled);
    fn->is_local = !(m->flags & NF_PUBLIC);
    cur_fn = fn;
    nslots = 0;
    nlslots = 0;
    nloops = 0;
    cur_ret_float = is_ftype(m->type);
    cur_ret_type = m->type;
    fn_fail_label = -1;
    nsites = 0;
    fn_fallible = (m->type && m->type->kind == ET_MAYBE) ||
                  (m->flags & NF_CANFAIL);

    for (struct node *p = m->a; p; p = p->next) {
        bind_slot(p->sym, type_size(p->sym->type));
        nparams++;
    }
    fn->nparams = nparams;

    ins = emit(IR_FUNC);
    ins->sym = arena_strdup(la, mangled);
    ins->nargs = nparams;

    lower_stmts(m->b);

    /* fall-through return so every path ends in a terminator */
    if (!fn->tail || (fn->tail->op != IR_RET &&
                      fn->tail->op != IR_RETV && fn->tail->op != IR_FRETV)) {
        if (cur_ret_float) {
            int z = lower_flt(0.0);
            emit(IR_FRETV)->a = z;
        } else if (m->type) {
            /* for `returns maybe T` the fall-through 0 IS nothing */
            int z = lower_const(0);
            emit(IR_RETV)->a = z;
        } else if (m->flags & NF_CANFAIL) {
            int z = lower_const(1);         /* fell off the end: success */
            emit(IR_RETV)->a = z;
        } else {
            emit(IR_RET);
        }
    }
    if (fn_fail_label >= 0 && fn_fallible) {
        /* the shared statement-default fail label survives only in
         * fallible funcs, which propagate (return the null word); in a
         * plain func or verb every default failure branched to its own
         * trap site instead */
        emit_label(fn_fail_label);
        {
            int z = lower_const(0);
            emit(IR_RETV)->a = z;
        }
    }
    emit_trap_sites();              /* the cold section */
    emit(IR_ENDF);

    fn->nslots = nslots;
    fn->slot_size = arena_alloc(la, (nslots ? nslots : 1) * sizeof(int));
    memcpy(fn->slot_size, slot_sizes, nslots * sizeof(int));
    return fn;
}

struct ir_program *
lower_program(struct arena *a, struct node *file)
{
    struct ir_program *p;
    struct ir_func **ftail;

    la = a;
    efile = lex_filename();
    p = arena_zalloc(la, sizeof *p);
    cur_prog = p;
    cstr_file = NULL;               /* re-interned per program */
    ftail = &p->funcs;
    nsels = sel_cap = 0;
    sel_names = NULL;
    fltctr = 0;

    for (struct node *it = file->a; it; it = it->next) {
        if (it->kind == N_ENUM) {
            emit_enum_names(it);
            continue;
        }
        if (it->kind != N_CLASS)
            continue;
        cur_class = it->name;
        emit_class_desc(it);
        for (struct node *m = it->a; m; m = m->next) {
            struct ir_func *fn;
            if (m->kind != N_VERB && m->kind != N_FUNC)
                continue;
            if (!m->b)
                continue;           /* .exi signature, no body */
            fn = lower_member(it, m);
            *ftail = fn;
            ftail = &fn->next;
        }
    }
    emit_entry(file);
    emit_selnames();                /* after emit_entry: id space complete */

    free(slot_sizes);
    slot_sizes = NULL;
    nslots = slot_cap = 0;
    free(lslots);
    lslots = NULL;
    nlslots = lslot_cap = 0;
    free(sel_names);
    sel_names = NULL;
    nsels = sel_cap = 0;

    return p;
}
