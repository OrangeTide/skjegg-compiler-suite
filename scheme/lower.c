/* lower.c : S-expression to IR lowering for TinScheme
 *
 * Tagged value representation on the 32-bit ColdFire target:
 *   fixnum:  n<<2 | 1      (TAG_FIX)
 *   pointer: raw ptr | 0   (TAG_OBJ, nil = 0)
 *   boolean: #f = 2, #t = 6 (TAG_IMM)
 *
 * Tagged arithmetic:
 *   (+ a b) = a + b - 1
 *   (- a b) = a - b + 1
 *   (* a b) = (a>>2) * (b-1) + 1
 *   (- a)   = 2 - a
 *
 * Boolean from comparison:
 *   IR cmp yields 0/1  ->  (cmp << 2) | 2  ->  2 or 6 = #f or #t
 *
 * R5RS falsiness: only #f (value 2) is falsy.
 */

#include "scheme.h"
#include "target.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Value-word width
 *
 * A TinScheme value is a tagged machine word: 4 bytes under ILP32, 8 under
 * LP64.  Heap objects (pairs, closures) are real machine pointers, so under
 * LP64 every value load/store, every value address, the heap object layout,
 * and the global value cells widen to 64 bits.  The small-integer (fixnum)
 * and boolean tag arithmetic stays 32-bit: the runtime keeps every live
 * address below 4GB, so a value word's upper half is always zero and a 32-bit
 * op (which zero-extends its result into the full register on both x86-64 and
 * arm64) preserves it.  With VWORD 4 the macros below expand to the exact
 * ILP32 opcodes, so that build is byte-identical.
 ****************************************************************/
#if SKJ_LP64
#define VWORD     8
#define IR_VLD    IR_LD64       /* load a value word from [addr] */
#define IR_VST    IR_ST64       /* store a value word to [addr] */
#define IR_VLDL   IR_LDL64      /* load a value word from a local slot */
#define IR_VSTL   IR_STL64      /* store a value word to a local slot */
#define IR_VLEA   IR_LEA64      /* address of a global value */
#define IR_VADD   IR_ADD64      /* add an offset to a pointer value */
#define IR_VBASE  IR_I64        /* global base type for a value word */
#else
#define VWORD     4
#define IR_VLD    IR_LW
#define IR_VST    IR_SW
#define IR_VLDL   IR_LDL
#define IR_VSTL   IR_STL
#define IR_VLEA   IR_LEA
#define IR_VADD   IR_ADD
#define IR_VBASE  IR_I32
#endif

/****************************************************************
 * IR builder shortcuts
 ****************************************************************/

static struct arena *lower_arena;
static struct ir_func *cur_fn;
static struct ir_program *cur_prog;

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

/****************************************************************
 * S-expression helpers
 ****************************************************************/

static int
is_sym(val_t v, const char *name)
{
    struct gc_string *s;

    if (!IS_PTR(v))
        return 0;
    if (PTR(v)->type != OBJ_SYMBOL)
        return 0;
    s = (struct gc_string *)PTR(v);
    return strcmp(s->data, name) == 0;
}

static const char *
sym_name(val_t v)
{
    return ((struct gc_string *)PTR(v))->data;
}

static int
is_pair(val_t v)
{
    return IS_PTR(v) && PTR(v)->type == OBJ_PAIR;
}

static int
is_symbol(val_t v)
{
    return IS_PTR(v) && PTR(v)->type == OBJ_SYMBOL;
}

/****************************************************************
 * Global symbol table
 ****************************************************************/

enum { GSYM_FUNC, GSYM_VAR, GSYM_RUNTIME };

struct gsym {
    char *name;
    int kind;
    int nparams;
    int needs_closure;      /* referenced as a value: emit a static closure */
};

static struct gsym *gsyms;
static int ngsyms;
static int gsym_cap;

static void
add_gsym(const char *name, int kind, int nparams)
{
    if (ngsyms == gsym_cap) {
        gsym_cap = gsym_cap ? gsym_cap * 2 : 16;
        gsyms = realloc(gsyms, gsym_cap * sizeof(*gsyms));
        if (!gsyms)
            die("oom");
    }
    gsyms[ngsyms].name = arena_strdup(lower_arena, name);
    gsyms[ngsyms].kind = kind;
    gsyms[ngsyms].nparams = nparams;
    gsyms[ngsyms].needs_closure = 0;
    ngsyms++;
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
        slot_sizes = realloc(slot_sizes,
            slot_cap * sizeof(*slot_sizes));
        if (!slot_sizes)
            die("oom");
    }
    slot_sizes[nslots] = bytes;
    return nslots++;
}

static void
add_lsym(const char *name)
{
    if (nlsyms == lsym_cap) {
        lsym_cap = lsym_cap ? lsym_cap * 2 : 16;
        lsyms = realloc(lsyms, lsym_cap * sizeof(*lsyms));
        if (!lsyms)
            die("oom");
    }
    lsyms[nlsyms].name = arena_strdup(lower_arena, name);
    lsyms[nlsyms].slot = alloc_slot(VWORD);
    nlsyms++;
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
 * Lambda lifting
 ****************************************************************/

struct lifted {
    char name[64];
    val_t params;
    val_t body;
    char **free_vars;
    int nfree;
    struct lifted *next;
};

static struct lifted *lifted_head;
static struct lifted *lifted_tail;
static int lambda_serial;

/****************************************************************
 * Forward declaration
 ****************************************************************/

static int lower_expr(val_t expr, int tail);
static void finish_slots(struct ir_func *fn);
static int closure_fn(int t_closure);
static int emit_closure(int fn_temp, int *free_vals, int nfree);
#ifdef CC_WASM
static void find_free(val_t expr, val_t params, char **fv, int *nfv, int max_fv);
static struct gc_heap *lower_heap;
#endif

/****************************************************************
 * Boolean conversion: IR cmp result (0/1) -> Scheme boolean
 *
 * #f = 2 = 0<<2|2, #t = 6 = 1<<2|2, so: (cmp << 2) | 2
 ****************************************************************/

static int
bool_from_cmp(int cmp_temp)
{
    struct ir_insn *ins;
    int t_two, t_shifted;

    t_two = lower_const(2);
    ins = emit(IR_SHL);
    ins->dst = new_temp();
    ins->a = cmp_temp;
    ins->b = t_two;
    t_shifted = ins->dst;

    ins = emit(IR_OR);
    ins->dst = new_temp();
    ins->a = t_shifted;
    ins->b = t_two;
    return ins->dst;
}

/****************************************************************
 * Special form lowering
 ****************************************************************/

static int
lower_cmp(val_t args, int ir_cmp_op)
{
    struct ir_insn *ins;
    int ta, tb, tcmp;

    ta = lower_expr(gc_car(args), 0);
    tb = lower_expr(gc_car(gc_cdr(args)), 0);
    ins = emit(ir_cmp_op);
    ins->dst = new_temp();
    ins->a = ta;
    ins->b = tb;
    tcmp = ins->dst;
    return bool_from_cmp(tcmp);
}

/* (if test then [else]) — with shortcut for comparison tests */
static int
lower_if(val_t args, int tail)
{
    struct ir_insn *ins;
    val_t test_form, then_form, else_form;
    int t_result;
    int ltrue, lfalse, lend;
    int has_else;

    test_form = gc_car(args);
    then_form = gc_car(gc_cdr(args));
    has_else = !IS_NIL(gc_cdr(gc_cdr(args)));
    else_form = has_else ? gc_car(gc_cdr(gc_cdr(args))) : VAL_NIL;

    ltrue = new_label();
    lfalse = new_label();
    lend = new_label();

    if (is_pair(test_form) && is_symbol(gc_car(test_form))) {
        int ir_cmp = -1;

        if (is_sym(gc_car(test_form), "="))
            ir_cmp = IR_CMPEQ;
        else if (is_sym(gc_car(test_form), "<"))
            ir_cmp = IR_CMPLTS;
        else if (is_sym(gc_car(test_form), ">"))
            ir_cmp = IR_CMPGTS;
        else if (is_sym(gc_car(test_form), "<="))
            ir_cmp = IR_CMPLES;
        else if (is_sym(gc_car(test_form), ">="))
            ir_cmp = IR_CMPGES;

        if (ir_cmp >= 0) {
            val_t test_args = gc_cdr(test_form);
            int ta, tb, tcmp;

            ta = lower_expr(gc_car(test_args), 0);
            tb = lower_expr(gc_car(gc_cdr(test_args)), 0);
            ins = emit(ir_cmp);
            ins->dst = new_temp();
            ins->a = ta;
            ins->b = tb;
            tcmp = ins->dst;
            ins = emit(IR_BNZ);
            ins->a = tcmp;
            ins->label = ltrue;
            emit_jmp(lfalse);
            goto branches;
        }
    }

    /* General case: R5RS falsiness (only #f is false) */
    {
        int t_test, t_false, t_diff;

        t_test = lower_expr(test_form, 0);
        t_false = lower_const((long)VAL_FALSE);
        ins = emit(IR_SUB);
        ins->dst = new_temp();
        ins->a = t_test;
        ins->b = t_false;
        t_diff = ins->dst;
        ins = emit(IR_BNZ);
        ins->a = t_diff;
        ins->label = ltrue;
        emit_jmp(lfalse);
    }

branches:
    if (tail) {
        emit_label(ltrue);
        {
            int t_then = lower_expr(then_form, 1);

            if (t_then >= 0) {
                ins = emit(IR_RETV);
                ins->a = t_then;
            }
        }
        emit_label(lfalse);
        if (has_else) {
            int t_else = lower_expr(else_form, 1);

            if (t_else >= 0) {
                ins = emit(IR_RETV);
                ins->a = t_else;
            }
        } else {
            int z = lower_const(0);

            ins = emit(IR_RETV);
            ins->a = z;
        }
        return -1;
    }

    t_result = new_temp();

    emit_label(ltrue);
    {
        int t_then = lower_expr(then_form, 0);

        ins = emit(IR_MOV);
        ins->dst = t_result;
        ins->a = t_then;
    }
    emit_jmp(lend);

    emit_label(lfalse);
    if (has_else) {
        int t_else = lower_expr(else_form, 0);

        ins = emit(IR_MOV);
        ins->dst = t_result;
        ins->a = t_else;
    } else {
        ins = emit(IR_LIC);
        ins->dst = t_result;
        ins->imm = 0;
    }

    emit_label(lend);
    return t_result;
}

static int
lower_add(val_t args)
{
    struct ir_insn *ins;
    int ta, tb, tsum, tone;

    ta = lower_expr(gc_car(args), 0);
    tb = lower_expr(gc_car(gc_cdr(args)), 0);
    ins = emit(IR_ADD);
    ins->dst = new_temp();
    ins->a = ta;
    ins->b = tb;
    tsum = ins->dst;

    tone = lower_const(1);
    ins = emit(IR_SUB);
    ins->dst = new_temp();
    ins->a = tsum;
    ins->b = tone;
    return ins->dst;
}

static int
lower_sub(val_t args)
{
    struct ir_insn *ins;
    int ta;

    ta = lower_expr(gc_car(args), 0);

    /* Unary negation: (- a) = 2 - a */
    if (IS_NIL(gc_cdr(args))) {
        int t_two;

        t_two = lower_const(2);
        ins = emit(IR_SUB);
        ins->dst = new_temp();
        ins->a = t_two;
        ins->b = ta;
        return ins->dst;
    }

    /* Binary subtraction: (- a b) = a - b + 1 */
    {
        int tb, tdiff, tone;

        tb = lower_expr(gc_car(gc_cdr(args)), 0);
        ins = emit(IR_SUB);
        ins->dst = new_temp();
        ins->a = ta;
        ins->b = tb;
        tdiff = ins->dst;

        tone = lower_const(1);
        ins = emit(IR_ADD);
        ins->dst = new_temp();
        ins->a = tdiff;
        ins->b = tone;
        return ins->dst;
    }
}

/* (* a b) = (a>>2) * (b-1) + 1 */
static int
lower_mul(val_t args)
{
    struct ir_insn *ins;
    int ta, tb, t_two, t_x, t_one, t_b1, t_prod;

    ta = lower_expr(gc_car(args), 0);
    tb = lower_expr(gc_car(gc_cdr(args)), 0);

    t_two = lower_const(2);
    ins = emit(IR_SHRS);
    ins->dst = new_temp();
    ins->a = ta;
    ins->b = t_two;
    t_x = ins->dst;

    t_one = lower_const(1);
    ins = emit(IR_SUB);
    ins->dst = new_temp();
    ins->a = tb;
    ins->b = t_one;
    t_b1 = ins->dst;

    ins = emit(IR_MUL);
    ins->dst = new_temp();
    ins->a = t_x;
    ins->b = t_b1;
    t_prod = ins->dst;

    ins = emit(IR_ADD);
    ins->dst = new_temp();
    ins->a = t_prod;
    ins->b = t_one;
    return ins->dst;
}

static int
lower_call(val_t form, int tail)
{
    struct ir_insn *ins;
    val_t fn_form, arg_list, cur;
    int args[16];
    int nargs, i;
    int can_tail;

    fn_form = gc_car(form);
    arg_list = gc_cdr(form);
    nargs = 0;

    for (cur = arg_list; !IS_NIL(cur); cur = gc_cdr(cur)) {
        if (nargs >= 15)
            die("lower: too many arguments");
        args[nargs++] = lower_expr(gc_car(cur), 0);
    }

    if (is_symbol(fn_form)) {
        const char *name = sym_name(fn_form);
        struct lsym *ls = find_lsym(name);
        struct gsym *gs;

        /* Local variable: closure call */
        if (ls) {
            int tf, t_fn, total;

            ins = emit(IR_VLDL);
            ins->dst = new_temp();
            ins->slot = ls->slot;
            tf = ins->dst;
            t_fn = closure_fn(tf);
            total = nargs + 1;

            ins = emit(IR_ARG);
            ins->a = tf;
            ins->imm = 0;
            for (i = 0; i < nargs; i++) {
                ins = emit(IR_ARG);
                ins->a = args[i];
                ins->imm = i + 1;
            }

            can_tail = tail && total <= cur_fn->nparams;
            if (can_tail) {
                ins = emit(IR_TAILCALLI);
                ins->a = t_fn;
                ins->nargs = total;
                return -1;
            }
            ins = emit(IR_CALLI);
            ins->dst = new_temp();
            ins->a = t_fn;
            ins->nargs = total;
            return ins->dst;
        }

        gs = find_gsym(name);

        /* Direct call to named function: pass 0 as env */
        if (gs && gs->kind == GSYM_FUNC) {
            int total = nargs + 1;
            int t_zero = lower_const(0);

            ins = emit(IR_ARG);
            ins->a = t_zero;
            ins->imm = 0;
            for (i = 0; i < nargs; i++) {
                ins = emit(IR_ARG);
                ins->a = args[i];
                ins->imm = i + 1;
            }

            can_tail = tail && total <= cur_fn->nparams;
            if (can_tail) {
                ins = emit(IR_TAILCALL);
                ins->sym = arena_strdup(lower_arena, name);
                ins->nargs = total;
                return -1;
            }
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena, name);
            ins->nargs = total;
            return ins->dst;
        }

        /* Runtime function: no env */
        if (gs && gs->kind == GSYM_RUNTIME) {
            for (i = 0; i < nargs; i++) {
                ins = emit(IR_ARG);
                ins->a = args[i];
                ins->imm = i;
            }

            can_tail = tail && nargs <= cur_fn->nparams;
            if (can_tail) {
                ins = emit(IR_TAILCALL);
                ins->sym = arena_strdup(lower_arena, name);
                ins->nargs = nargs;
                return -1;
            }
            ins = emit(IR_CALL);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena, name);
            ins->nargs = nargs;
            return ins->dst;
        }

        /* Global variable: closure call */
        if (gs && gs->kind == GSYM_VAR) {
            int taddr, tf, t_fn, total;

            ins = emit(IR_VLEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena, name);
            taddr = ins->dst;
            ins = emit(IR_VLD);
            ins->dst = new_temp();
            ins->a = taddr;
            tf = ins->dst;
            t_fn = closure_fn(tf);
            total = nargs + 1;

            ins = emit(IR_ARG);
            ins->a = tf;
            ins->imm = 0;
            for (i = 0; i < nargs; i++) {
                ins = emit(IR_ARG);
                ins->a = args[i];
                ins->imm = i + 1;
            }

            can_tail = tail && total <= cur_fn->nparams;
            if (can_tail) {
                ins = emit(IR_TAILCALLI);
                ins->a = t_fn;
                ins->nargs = total;
                return -1;
            }
            ins = emit(IR_CALLI);
            ins->dst = new_temp();
            ins->a = t_fn;
            ins->nargs = total;
            return ins->dst;
        }
        die("lower: undefined '%s'", name);
    }

    /* Bare expression: closure call */
    {
        int tf, t_fn, total;

        tf = lower_expr(fn_form, 0);
        t_fn = closure_fn(tf);
        total = nargs + 1;

        ins = emit(IR_ARG);
        ins->a = tf;
        ins->imm = 0;
        for (i = 0; i < nargs; i++) {
            ins = emit(IR_ARG);
            ins->a = args[i];
            ins->imm = i + 1;
        }

        can_tail = tail && total <= cur_fn->nparams;
        if (can_tail) {
            ins = emit(IR_TAILCALLI);
            ins->a = t_fn;
            ins->nargs = total;
            return -1;
        }
        ins = emit(IR_CALLI);
        ins->dst = new_temp();
        ins->a = t_fn;
        ins->nargs = total;
        return ins->dst;
    }
}

#ifdef CC_WASM
/* Lift a reset body into its own function, a thunk the continuation runs.  The
   WasmFX lowering needs a bare funcref for cont.new, so the body must capture no
   locals (find_free turns up nothing but globals).  Returns the thunk's name;
   the lifted list compiles it in pass 4 like any lambda. */
static const char *
lift_reset_thunk(val_t body_expr)
{
    static int thunk_serial;
    struct lifted *l;
    char *fv[16];
    int nfv = 0;
    val_t params, body;

    /* (lambda (__resetarg) body_expr): the dummy param is the continuation's
       first-resume value, which a reset body ignores. */
    params = gc_cons(lower_heap, gc_symbol(lower_heap, "__resetarg"), VAL_NIL);
    body = gc_cons(lower_heap, body_expr, VAL_NIL);

    find_free(body, params, fv, &nfv, 16);
    if (nfv > 0)
        die("lower: a reset body that captures a local is not supported on WASM yet");

    l = arena_alloc(lower_arena, sizeof(*l));
    snprintf(l->name, sizeof(l->name), "__reset_thunk_%d", thunk_serial++);
    l->params = params;
    l->body = body;
    l->free_vars = NULL;
    l->nfree = 0;
    l->next = NULL;
    if (!lifted_head)
        lifted_head = l;
    else
        lifted_tail->next = l;
    lifted_tail = l;
    return arena_strdup(lower_arena, l->name);
}
#endif

static int
lower_reset(val_t args)
{
    struct ir_insn *ins;
#ifdef CC_WASM
    /* WasmFX lowering: run the lifted body thunk as a continuation under a
       $shift handler.  IR_MARK yields 0 (body returned, its value stored to the
       shadow slot) or the nonzero captured-continuation index; the handler is
       called with that index on the shift path.  See the backend and
       doc/internals/wasm-continuations.md. */
    const char *thunk;
    int vslot, tidx, tresult, lelse, lend;

    if (IS_NIL(args) || IS_NIL(gc_cdr(args)))
        die("lower: (reset body handler) requires two arguments");

    thunk = lift_reset_thunk(gc_car(args));
    vslot = alloc_slot(VWORD);          /* the body's normal-return value */
    tresult = new_temp();
    lelse = new_label();
    lend = new_label();

    ins = emit(IR_MARK);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, thunk);
    ins->slot = vslot;
    tidx = ins->dst;

    ins = emit(IR_BNZ);
    ins->a = tidx;
    ins->label = lelse;

    /* normal: the body returned without a shift; read its value from the slot */
    ins = emit(IR_LDL);
    ins->dst = tresult;
    ins->slot = vslot;
    ins = emit(IR_JMP);
    ins->label = lend;

    /* shift fired: call the handler with the continuation index */
    ins = emit(IR_LABEL);
    ins->label = lelse;
    {
        int tf, t_fn, tcall;

        tf = lower_expr(gc_car(gc_cdr(args)), 0);
        t_fn = closure_fn(tf);
        ins = emit(IR_ARG); ins->a = tf;   ins->imm = 0;
        ins = emit(IR_ARG); ins->a = tidx; ins->imm = 1;
        ins = emit(IR_CALLI);
        ins->dst = new_temp();
        ins->a = t_fn;
        ins->nargs = 2;
        tcall = ins->dst;
        ins = emit(IR_MOV);
        ins->dst = tresult;
        ins->a = tcall;
    }

    ins = emit(IR_LABEL);
    ins->label = lend;

    ins = emit(IR_CONT_UNWIND);
    ins->slot = vslot;

    return tresult;
#else
    val_t body_expr, handler_expr;
    int slot, tbuf, tbody, lelse, lend;
    int tresult;

    if (IS_NIL(args) || IS_NIL(gc_cdr(args)))
        die("lower: (reset body handler) requires two arguments");
    body_expr = gc_car(args);
    handler_expr = gc_car(gc_cdr(args));

    slot = alloc_slot(4 * VWORD);   /* mark: fp, sp, re-entry PC, arena */
    lelse = new_label();
    lend = new_label();
    tresult = new_temp();

    ins = emit(IR_MARK);
    ins->dst = new_temp();
    ins->slot = slot;
    ins->label = new_label();
    tbuf = ins->dst;

    /* if mark returned nonzero (shift fired), branch to handler */
    ins = emit(IR_BNZ);
    ins->a = tbuf;
    ins->label = lelse;

    /* First entry: evaluate body */
    tbody = lower_expr(body_expr, 0);
    ins = emit(IR_MOV);
    ins->dst = tresult;
    ins->a = tbody;
    ins = emit(IR_JMP);
    ins->label = lend;

    /* Else branch: shift fired, call handler with buffer */
    ins = emit(IR_LABEL);
    ins->label = lelse;

    {
        int tf, t_fn, tcall;

        tf = lower_expr(handler_expr, 0);
        t_fn = closure_fn(tf);
        ins = emit(IR_ARG);
        ins->a = tf;
        ins->imm = 0;
        ins = emit(IR_ARG);
        ins->a = tbuf;
        ins->imm = 1;
        ins = emit(IR_CALLI);
        ins->dst = new_temp();
        ins->a = t_fn;
        ins->nargs = 2;
        tcall = ins->dst;
        ins = emit(IR_MOV);
        ins->dst = tresult;
        ins->a = tcall;
    }

    ins = emit(IR_LABEL);
    ins->label = lend;

    /* The reset extent is over: every continuation captured within it is
     * dead (resumed and run to completion, or dropped).  Restore the
     * continuation arena to its mark-time high-water mark (saved by MARK
     * in the mark slot), reclaiming those buffers.  This gives the arena
     * stack discipline, so a program that resets repeatedly (a REPL, a
     * MUD turn) does not grow it without bound. */
    ins = emit(IR_CONT_UNWIND);
    ins->slot = slot;

    return tresult;
#endif
}

static int
lower_shift(void)
{
    struct ir_insn *ins;

    ins = emit(IR_CAPTURE);
    ins->dst = new_temp();
    return ins->dst;
}

static int
lower_resume(val_t args)
{
    struct ir_insn *ins;
    int tbuf, tval;

    if (IS_NIL(args) || IS_NIL(gc_cdr(args)))
        die("lower: (resume buf val) requires two arguments");
    tbuf = lower_expr(gc_car(args), 0);
    tval = lower_expr(gc_car(gc_cdr(args)), 0);

    ins = emit(IR_RESUME);
    ins->a = tbuf;
    ins->b = tval;
#ifdef CC_WASM
    /* On WASM a resume is a structured call that returns the continuation's
       completion value, so it has a usable result.  (Natively resume is a
       non-local transfer that never returns here, so the result is unused.) */
    ins->dst = new_temp();
    return ins->dst;
#else
    return new_temp();
#endif
}

/****************************************************************
 * Free variable analysis
 ****************************************************************/

static void
find_free(val_t expr, val_t params, char **fv, int *nfv, int max_fv)
{
    val_t p;
    int i;

    if (IS_NIL(expr) || IS_FIX(expr) ||
        expr == VAL_TRUE || expr == VAL_FALSE)
        return;
    if (is_symbol(expr)) {
        const char *name = sym_name(expr);

        for (p = params; !IS_NIL(p); p = gc_cdr(p))
            if (strcmp(sym_name(gc_car(p)), name) == 0)
                return;
        if (find_gsym(name))
            return;
        if (!find_lsym(name))
            return;
        for (i = 0; i < *nfv; i++)
            if (strcmp(fv[i], name) == 0)
                return;
        if (*nfv >= max_fv)
            die("lower: too many free variables");
        fv[(*nfv)++] = arena_strdup(lower_arena, name);
        return;
    }
    if (!is_pair(expr))
        return;
    if (is_sym(gc_car(expr), "lambda"))
        return;
    find_free(gc_car(expr), params, fv, nfv, max_fv);
    find_free(gc_cdr(expr), params, fv, nfv, max_fv);
}

/****************************************************************
 * Closure helpers
 *
 * Closure layout (bump-allocated from __heap), fields one value word (VWORD)
 * wide:
 *   offset 0:         type byte (OBJ_CLOSURE = 3)
 *   offset VWORD:     function pointer
 *   offset 2*VWORD:   free[0]
 *   offset 3*VWORD:   free[1]
 *   ...
 ****************************************************************/

static int
emit_closure(int fn_temp, int *free_vals, int nfree)
{
    struct ir_insn *ins;
    int t_ptr_addr, t_ptr, t_off, t_addr, t_new;
    int size = 2 * VWORD + VWORD * nfree;
    int i;

    ins = emit(IR_VLEA);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, "__heap_ptr");
    t_ptr_addr = ins->dst;

    ins = emit(IR_VLD);
    ins->dst = new_temp();
    ins->a = t_ptr_addr;
    t_ptr = ins->dst;

    {
        int tag = lower_const(3);
        ins = emit(IR_SB);
        ins->a = t_ptr;
        ins->b = tag;
    }

    t_off = lower_const(VWORD);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = t_ptr;
    ins->b = t_off;
    t_addr = ins->dst;
    ins = emit(IR_VST);
    ins->a = t_addr;
    ins->b = fn_temp;

    for (i = 0; i < nfree; i++) {
        t_off = lower_const(2 * VWORD + VWORD * i);
        ins = emit(IR_VADD);
        ins->dst = new_temp();
        ins->a = t_ptr;
        ins->b = t_off;
        t_addr = ins->dst;
        ins = emit(IR_VST);
        ins->a = t_addr;
        ins->b = free_vals[i];
    }

    t_off = lower_const(size);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = t_ptr;
    ins->b = t_off;
    t_new = ins->dst;
    ins = emit(IR_VST);
    ins->a = t_ptr_addr;
    ins->b = t_new;

    return t_ptr;
}

static int
closure_fn(int t_closure)
{
    struct ir_insn *ins;
    int t_off, t_addr;

    t_off = lower_const(VWORD);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = t_closure;
    ins->b = t_off;
    t_addr = ins->dst;
    ins = emit(IR_VLD);
    ins->dst = new_temp();
    ins->a = t_addr;
    return ins->dst;
}

static int
lower_lambda(val_t args)
{
    struct ir_insn *ins;
    struct lifted *l;
    char *fv[16];
    int fv_temps[16];
    int nfv = 0;
    int t_fn, i;

    l = arena_alloc(lower_arena, sizeof(*l));
    snprintf(l->name, sizeof(l->name), "__lambda_%d",
        lambda_serial++);
    l->params = gc_car(args);
    l->body = gc_cdr(args);
    l->next = NULL;

    find_free(l->body, l->params, fv, &nfv, 16);
    l->nfree = nfv;
    l->free_vars = NULL;
    if (nfv > 0) {
        l->free_vars = arena_alloc(lower_arena, nfv * sizeof(char *));
        memcpy(l->free_vars, fv, nfv * sizeof(char *));
    }

    if (!lifted_head)
        lifted_head = l;
    else
        lifted_tail->next = l;
    lifted_tail = l;

    for (i = 0; i < nfv; i++) {
        struct lsym *ls = find_lsym(fv[i]);

        if (!ls)
            die("lower: free var '%s' not found", fv[i]);
        ins = emit(IR_VLDL);
        ins->dst = new_temp();
        ins->slot = ls->slot;
        fv_temps[i] = ins->dst;
    }

    ins = emit(IR_VLEA);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, l->name);
    t_fn = ins->dst;

    return emit_closure(t_fn, fv_temps, nfv);
}

/****************************************************************
 * Pair and list operations
 *
 * Pairs are bump-allocated from __heap (BSS).  The car/cdr are value words
 * (VWORD) past a fixed 8-byte header:
 *   offset 0:       type byte (0 = pair, zero-init from BSS)
 *   offset 8:       car
 *   offset 8+VWORD: cdr
 *   8 + 2*VWORD bytes per pair
 ****************************************************************/

static int
emit_cons(int ta, int tb)
{
    struct ir_insn *ins;
    int t_ptr_addr, t_ptr, t_off, t_addr, t_new;

    ins = emit(IR_VLEA);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, "__heap_ptr");
    t_ptr_addr = ins->dst;

    ins = emit(IR_VLD);
    ins->dst = new_temp();
    ins->a = t_ptr_addr;
    t_ptr = ins->dst;

    t_off = lower_const(8);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = t_ptr;
    ins->b = t_off;
    t_addr = ins->dst;
    ins = emit(IR_VST);
    ins->a = t_addr;
    ins->b = ta;

    t_off = lower_const(8 + VWORD);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = t_ptr;
    ins->b = t_off;
    t_addr = ins->dst;
    ins = emit(IR_VST);
    ins->a = t_addr;
    ins->b = tb;

    t_off = lower_const(8 + 2 * VWORD);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = t_ptr;
    ins->b = t_off;
    t_new = ins->dst;
    ins = emit(IR_VST);
    ins->a = t_ptr_addr;
    ins->b = t_new;

    return t_ptr;
}

static int
lower_cons(val_t args)
{
    int ta, tb;

    ta = lower_expr(gc_car(args), 0);
    tb = lower_expr(gc_car(gc_cdr(args)), 0);
    return emit_cons(ta, tb);
}

static int
lower_car(val_t args)
{
    struct ir_insn *ins;
    int ta, t_off, t_addr;

    ta = lower_expr(gc_car(args), 0);
    t_off = lower_const(8);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = ta;
    ins->b = t_off;
    t_addr = ins->dst;
    ins = emit(IR_VLD);
    ins->dst = new_temp();
    ins->a = t_addr;
    return ins->dst;
}

static int
lower_cdr(val_t args)
{
    struct ir_insn *ins;
    int ta, t_off, t_addr;

    ta = lower_expr(gc_car(args), 0);
    t_off = lower_const(8 + VWORD);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = ta;
    ins->b = t_off;
    t_addr = ins->dst;
    ins = emit(IR_VLD);
    ins->dst = new_temp();
    ins->a = t_addr;
    return ins->dst;
}

static int
lower_null_p(val_t args)
{
    struct ir_insn *ins;
    int ta, t_cmp;

    ta = lower_expr(gc_car(args), 0);
    {
        int zero = lower_const(0);
        ins = emit(IR_CMPEQ);
        ins->dst = new_temp();
        ins->a = ta;
        ins->b = zero;
    }
    t_cmp = ins->dst;
    return bool_from_cmp(t_cmp);
}

static int
lower_pair_p(val_t args)
{
    struct ir_insn *ins;
    int ta, t_tag, t_ptrtag, t_nnil, t_both;

    ta = lower_expr(gc_car(args), 0);

    {
        int mask = lower_const(3);
        ins = emit(IR_AND);
        ins->dst = new_temp();
        ins->a = ta;
        ins->b = mask;
    }
    t_tag = ins->dst;

    {
        int zero = lower_const(0);
        ins = emit(IR_CMPEQ);
        ins->dst = new_temp();
        ins->a = t_tag;
        ins->b = zero;
    }
    t_ptrtag = ins->dst;

    {
        int zero = lower_const(0);
        ins = emit(IR_CMPNE);
        ins->dst = new_temp();
        ins->a = ta;
        ins->b = zero;
    }
    t_nnil = ins->dst;

    ins = emit(IR_AND);
    ins->dst = new_temp();
    ins->a = t_ptrtag;
    ins->b = t_nnil;
    t_both = ins->dst;

    return bool_from_cmp(t_both);
}

static int
lower_set_car(val_t args)
{
    struct ir_insn *ins;
    int ta, tb, t_off, t_addr;

    ta = lower_expr(gc_car(args), 0);
    tb = lower_expr(gc_car(gc_cdr(args)), 0);
    t_off = lower_const(8);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = ta;
    ins->b = t_off;
    t_addr = ins->dst;
    ins = emit(IR_VST);
    ins->a = t_addr;
    ins->b = tb;
    return lower_const(0);
}

static int
lower_set_cdr(val_t args)
{
    struct ir_insn *ins;
    int ta, tb, t_off, t_addr;

    ta = lower_expr(gc_car(args), 0);
    tb = lower_expr(gc_car(gc_cdr(args)), 0);
    t_off = lower_const(8 + VWORD);
    ins = emit(IR_VADD);
    ins->dst = new_temp();
    ins->a = ta;
    ins->b = t_off;
    t_addr = ins->dst;
    ins = emit(IR_VST);
    ins->a = t_addr;
    ins->b = tb;
    return lower_const(0);
}

static int
lower_list(val_t args)
{
    int vals[16];
    int nargs = 0;
    val_t cur;
    int i, result;

    if (IS_NIL(args))
        return lower_const(0);

    for (cur = args; !IS_NIL(cur); cur = gc_cdr(cur)) {
        if (nargs >= 16)
            die("lower: list with too many elements");
        vals[nargs++] = lower_expr(gc_car(cur), 0);
    }

    result = lower_const(0);
    for (i = nargs - 1; i >= 0; i--)
        result = emit_cons(vals[i], result);
    return result;
}

/****************************************************************
 * Output: display / newline / write-string
 *
 * TinScheme writes through the `write(fd, buf, len)` runtime seam (fd 1 =
 * stdout), which maps to WASI fd_write on WASM and to the start.S seam
 * natively.  A string literal is written directly from a rodata byte global.
 * Any other value goes through the emitted __scm_display helper, which inspects
 * the runtime tag and formats a fixnum, boolean, or nil.  The runtime has no
 * link step (scheme emits a complete module on WASM), so the formatter is
 * emitted as IR, not a C/asm runtime routine.
 ****************************************************************/

static int g_str_serial;
static int g_display_used;

/* Interned quoted symbols: one rodata object per distinct name, so quoting the
   same symbol twice yields the same address and eq? identity holds. */
#define MAX_QSYMS 256
static struct { char *name; char *gname; } g_qsyms[MAX_QSYMS];
static int g_nqsyms;

static int
is_string(val_t v)
{
    return IS_PTR(v) && PTR(v)->type == OBJ_STRING;
}

/* a rodata byte global holding `len` bytes of `s`; returns its symbol name */
static const char *
new_bytes_global(const char *s, int len)
{
    struct ir_global *g;
    char buf[32];
    char *data;

    if (len < 1)
        len = 1;
    snprintf(buf, sizeof(buf), "__scm_str_%d", g_str_serial++);
    g = arena_zalloc(lower_arena, sizeof(*g));
    g->name = arena_strdup(lower_arena, buf);
    g->base_type = IR_I8;
    g->arr_size = len;
    g->is_rodata = 1;
    data = arena_alloc(lower_arena, len);
    memcpy(data, s, len);
    g->init_string = data;
    g->init_strlen = len;
    g->next = cur_prog->globals;
    cur_prog->globals = g;
    return g->name;
}

/* write(1, buf, len): a 3-arg call to the runtime write seam */
static void
emit_write3(int t_buf, int t_len)
{
    struct ir_insn *ins;
    int t_fd = lower_const(1);

    ins = emit(IR_ARG); ins->a = t_fd;  ins->imm = 0;
    ins = emit(IR_ARG); ins->a = t_buf; ins->imm = 1;
    ins = emit(IR_ARG); ins->a = t_len; ins->imm = 2;
    ins = emit(IR_CALL);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, "write");
    ins->nargs = 3;
}

/* write a fixed byte string to stdout */
static void
emit_write_literal(const char *s, int len)
{
    const char *name = new_bytes_global(s, len);
    struct ir_insn *ins;
    int t_addr, t_len;

    ins = emit(IR_VLEA);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, name);
    t_addr = ins->dst;
    t_len = lower_const(len);
    emit_write3(t_addr, t_len);
}

static int
lower_display(val_t args)
{
    val_t a = gc_car(args);
    struct ir_insn *ins;

    if (is_string(a)) {
        struct gc_string *str = (struct gc_string *)PTR(a);
        emit_write_literal(str->data, str->len);
    } else {
        int tv = lower_expr(a, 0);

        g_display_used = 1;
        ins = emit(IR_ARG);
        ins->a = tv;
        ins->imm = 0;
        ins = emit(IR_CALL);
        ins->dst = new_temp();
        ins->sym = arena_strdup(lower_arena, "__scm_display");
        ins->nargs = 1;
    }
    return lower_const((long)VAL_NIL);
}

static int
lower_newline(void)
{
    emit_write_literal("\n", 1);
    return lower_const((long)VAL_NIL);
}

static int
lower_write_string(val_t args)
{
    val_t a = gc_car(args);
    struct gc_string *str;

    if (!is_string(a))
        die("lower: write-string expects a string literal");
    str = (struct gc_string *)PTR(a);
    emit_write_literal(str->data, str->len);
    return lower_const((long)VAL_NIL);
}

/* local IR builder shorthands for the display helper */
static int
eb(int op, int a, int b)
{
    struct ir_insn *x = emit(op);
    x->dst = new_temp();
    x->a = a;
    x->b = b;
    return x->dst;
}

static int
eldl(int slot)
{
    struct ir_insn *x = emit(IR_LDL);
    x->dst = new_temp();
    x->slot = slot;
    return x->dst;
}

static void
estl(int slot, int t)
{
    struct ir_insn *x = emit(IR_STL);
    x->a = t;
    x->slot = slot;
}

static void
ebnz(int t, int lab)
{
    struct ir_insn *x = emit(IR_BNZ);
    x->a = t;
    x->label = lab;
}

static int
evlea(const char *name)
{
    struct ir_insn *x = emit(IR_VLEA);
    x->dst = new_temp();
    x->sym = arena_strdup(lower_arena, name);
    return x->dst;
}

/* Emit __scm_display(v): format a tagged value to stdout.
 *   fixnum -> signed decimal (untag >>2, sign, digit loop into __scm_numbuf)
 *   nil/#f/#t -> the literal "()"/"#f"/"#t"
 *   any heap pointer -> "#<obj>" (generic pair/string display is future work)
 * Built as a standalone IR function (one value param in slot 0, no env),
 * called as __scm_display(v). */
static struct ir_func *
build_display_helper(void)
{
    struct ir_func *fn;
    struct ir_insn *ins;
    int slot_v, slot_i;
    int Lfix, Lnil, Lf, Lt, Lobj, Lend, Lneg, Lpos, Lzero, Lloop, Ldone;
    int v, t3, c1, c0, c2, c6, cmp, raw, neg, i, i2, v2, rem, quo, dch;
    int base, addr, startp, len, cur, isz, cont;
    struct ir_global *g;

    /* the decimal digit buffer */
    g = arena_zalloc(lower_arena, sizeof(*g));
    g->name = arena_strdup(lower_arena, "__scm_numbuf");
    g->base_type = IR_I8;
    g->arr_size = 24;
    g->next = cur_prog->globals;
    cur_prog->globals = g;

    fn = ir_new_func(lower_arena, "__scm_display");
    cur_fn = fn;
    nlsyms = 0;
    nslots = 0;
    add_lsym("__v");            /* param slot 0: the tagged value */
    fn->nparams = 1;
    slot_v = alloc_slot(4);     /* untagged working integer */
    slot_i = alloc_slot(4);     /* digit buffer index */

    ins = emit(IR_FUNC);
    ins->sym = arena_strdup(lower_arena, "__scm_display");
    ins->nargs = 1;

    Lfix = new_label(); Lnil = new_label(); Lf = new_label();
    Lt = new_label(); Lobj = new_label(); Lend = new_label();
    Lneg = new_label(); Lpos = new_label(); Lzero = new_label();
    Lloop = new_label(); Ldone = new_label();

    /* v = param (value word in slot 0) */
    ins = emit(IR_VLDL); ins->dst = new_temp(); ins->slot = 0;
    v = ins->dst;

    /* dispatch on tag */
    c1 = lower_const(1); c0 = lower_const(0);
    c2 = lower_const(2); c6 = lower_const(6);
    t3 = eb(IR_AND, v, lower_const(3));
    cmp = eb(IR_CMPEQ, t3, c1); ebnz(cmp, Lfix);
    cmp = eb(IR_CMPEQ, v, c0);  ebnz(cmp, Lnil);
    cmp = eb(IR_CMPEQ, v, c2);  ebnz(cmp, Lf);
    cmp = eb(IR_CMPEQ, v, c6);  ebnz(cmp, Lt);
    emit_jmp(Lobj);

    /* fixnum: untag, handle sign, print digits */
    emit_label(Lfix);
    raw = eb(IR_SHRS, v, lower_const(2));
    estl(slot_v, raw);
    cmp = eb(IR_CMPLTS, raw, lower_const(0));
    ebnz(cmp, Lneg);
    emit_jmp(Lpos);
    emit_label(Lneg);
    emit_write_literal("-", 1);
    neg = eb(IR_SUB, lower_const(0), raw);
    estl(slot_v, neg);
    emit_jmp(Lpos);
    emit_label(Lpos);
    cur = eldl(slot_v);
    isz = eb(IR_CMPEQ, cur, lower_const(0));
    ebnz(isz, Lzero);
    estl(slot_i, lower_const(24));
    emit_jmp(Lloop);

    emit_label(Lzero);
    dch = lower_const('0');
    base = evlea("__scm_numbuf");
    { struct ir_insn *x = emit(IR_SB); x->a = base; x->b = dch; }
    { int t_one = lower_const(1); emit_write3(base, t_one); }
    emit_jmp(Lend);

    emit_label(Lloop);
    i = eldl(slot_i);
    i2 = eb(IR_SUB, i, lower_const(1));
    estl(slot_i, i2);
    v2 = eldl(slot_v);
    rem = eb(IR_MODS, v2, lower_const(10));
    quo = eb(IR_DIVS, v2, lower_const(10));
    estl(slot_v, quo);
    dch = eb(IR_ADD, rem, lower_const('0'));
    base = evlea("__scm_numbuf");
    addr = eb(IR_ADD, base, i2);
    { struct ir_insn *x = emit(IR_SB); x->a = addr; x->b = dch; }
    cont = eb(IR_CMPEQ, quo, lower_const(0));
    ebnz(cont, Ldone);
    emit_jmp(Lloop);

    emit_label(Ldone);
    i = eldl(slot_i);
    base = evlea("__scm_numbuf");
    startp = eb(IR_ADD, base, i);
    len = eb(IR_SUB, lower_const(24), i);
    emit_write3(startp, len);
    emit_jmp(Lend);

    emit_label(Lnil); emit_write_literal("()", 2); emit_jmp(Lend);
    emit_label(Lf);   emit_write_literal("#f", 2); emit_jmp(Lend);
    emit_label(Lt);   emit_write_literal("#t", 2); emit_jmp(Lend);
    emit_label(Lobj); emit_write_literal("#<obj>", 6); emit_jmp(Lend);

    emit_label(Lend);
    ins = emit(IR_RETV);
    ins->a = lower_const((long)VAL_NIL);
    emit(IR_ENDF);
    finish_slots(fn);
    return fn;
}

/****************************************************************
 * Binding and control special forms
 *
 * let/let* bind local slots (reusing the lsym table, so an inner lambda
 * captures a let variable the same way it captures a parameter); cond/when/
 * unless/and/or/not desugar onto if-style branching and the #f-only falsiness
 * rule.  None need any runtime support.
 ****************************************************************/

static int evldl(int slot)
{
    struct ir_insn *x = emit(IR_VLDL);
    x->dst = new_temp();
    x->slot = slot;
    return x->dst;
}

static void evstl(int slot, int t)
{
    struct ir_insn *x = emit(IR_VSTL);
    x->a = t;
    x->slot = slot;
}

/* lower a sequence of body forms (an implicit begin); returns the last value */
static int
lower_body(val_t body, int tail)
{
    int t = lower_const((long)VAL_NIL);
    val_t cur;

    for (cur = body; !IS_NIL(cur); cur = gc_cdr(cur)) {
        int last = IS_NIL(gc_cdr(cur));
        t = lower_expr(gc_car(cur), last ? tail : 0);
    }
    return t;
}

static int
lower_let(val_t rest, int star)
{
    val_t binds = gc_car(rest);
    val_t body = gc_cdr(rest);
    int saved = nlsyms;
    int tmp[64], slot[64], n = 0;
    val_t b;
    int result, i;

    if (!star) {
        /* parallel: evaluate every init in the outer scope first */
        for (b = binds; !IS_NIL(b); b = gc_cdr(b)) {
            val_t pair = gc_car(b);
            if (n >= 64)
                die("lower: too many let bindings");
            tmp[n++] = lower_expr(gc_car(gc_cdr(pair)), 0);
        }
        n = 0;
        for (b = binds; !IS_NIL(b); b = gc_cdr(b)) {
            val_t pair = gc_car(b);
            add_lsym(sym_name(gc_car(pair)));
            slot[n] = lsyms[nlsyms - 1].slot;
            evstl(slot[n], tmp[n]);
            n++;
        }
    } else {
        /* sequential: each init sees the earlier bindings */
        for (b = binds; !IS_NIL(b); b = gc_cdr(b)) {
            val_t pair = gc_car(b);
            int t = lower_expr(gc_car(gc_cdr(pair)), 0);
            add_lsym(sym_name(gc_car(pair)));
            evstl(lsyms[nlsyms - 1].slot, t);
        }
    }
    (void)slot; (void)i;

    result = lower_body(body, 0);
    nlsyms = saved;             /* pop the let scope */
    return result;
}

static int
lower_not(val_t rest)
{
    int t = lower_expr(gc_car(rest), 0);
    /* #t when the operand was #f (only #f is false) */
    return bool_from_cmp(eb(IR_CMPEQ, t, lower_const((long)VAL_FALSE)));
}

/* and: () -> #t; else the first #f, or the last value.
 * or:  () -> #f; else the first non-#f, or the last value. */
static int
lower_and_or(val_t rest, int is_or)
{
    int rslot = alloc_slot(VWORD);
    int lend = new_label();
    val_t cur;

    if (IS_NIL(rest)) {
        evstl(rslot, lower_const((long)(is_or ? VAL_FALSE : VAL_TRUE)));
        return evldl(rslot);
    }
    for (cur = rest; !IS_NIL(cur); cur = gc_cdr(cur)) {
        int last = IS_NIL(gc_cdr(cur));
        int t = lower_expr(gc_car(cur), 0);
        evstl(rslot, t);
        if (!last) {
            int isfalse = eb(IR_CMPEQ, t, lower_const((long)VAL_FALSE));

            if (is_or) {
                /* stop at the first non-#f: fall through on #f, jump on true */
                int lnext = new_label();
                ebnz(isfalse, lnext);
                emit_jmp(lend);
                emit_label(lnext);
            } else {
                /* stop at the first #f */
                ebnz(isfalse, lend);
            }
        }
    }
    emit_label(lend);
    return evldl(rslot);
}

static int
lower_cond(val_t rest)
{
    int rslot = alloc_slot(VWORD);
    int lend = new_label();
    val_t cur;

    evstl(rslot, lower_const((long)VAL_NIL));    /* no clause matched */
    for (cur = rest; !IS_NIL(cur); cur = gc_cdr(cur)) {
        val_t clause = gc_car(cur);
        val_t test = gc_car(clause);
        val_t body = gc_cdr(clause);
        int lnext = new_label();

        if (is_sym(test, "else")) {
            evstl(rslot, lower_body(body, 0));
            emit_jmp(lend);
            break;
        }
        {
            int t = lower_expr(test, 0);
            int isfalse = eb(IR_CMPEQ, t, lower_const((long)VAL_FALSE));

            ebnz(isfalse, lnext);
            /* a (test) clause with no body yields the test value */
            evstl(rslot, IS_NIL(body) ? t : lower_body(body, 0));
            emit_jmp(lend);
        }
        emit_label(lnext);
    }
    emit_label(lend);
    return evldl(rslot);
}

static int
lower_when(val_t rest, int unless)
{
    int rslot = alloc_slot(VWORD);
    int lend = new_label();
    int lbody = new_label();
    int t = lower_expr(gc_car(rest), 0);
    int isfalse;

    evstl(rslot, lower_const((long)VAL_NIL));
    isfalse = eb(IR_CMPEQ, t, lower_const((long)VAL_FALSE));
    /* when: run body unless #f; unless: run body only when #f */
    if (!unless) {
        ebnz(isfalse, lend);
    } else {
        ebnz(isfalse, lbody);
        emit_jmp(lend);
    }
    emit_label(lbody);
    evstl(rslot, lower_body(gc_cdr(rest), 0));
    emit_label(lend);
    return evldl(rslot);
}

/****************************************************************
 * Numeric primitives
 *
 * Fixnums are tagged (n<<2 | 1), so a division-family op untags its operands,
 * computes, and retags.  Everything here is branchless so no value crosses a
 * basic-block boundary (the IR keeps temps single-block; cross-block values go
 * through slots).  quotient/remainder truncate toward zero (IR_DIVS/IR_MODS);
 * modulo floors (its result takes the divisor's sign).
 ****************************************************************/

static int
scm_untag(int t)
{
    return eb(IR_SHRS, t, lower_const(2));
}

/* Every emitting subexpression below is its own statement: a call with two
   side-effecting arguments would emit the two in an order C leaves unspecified,
   so host gcc and skj-cc-wasm (which disagree on argument evaluation order)
   would produce equivalent but byte-different modules and fail the self-host
   byte-identity check.  Sequencing the emits pins the order. */

static int
scm_retag(int r)
{
    int shl = eb(IR_SHL, r, lower_const(2));
    int one = lower_const(1);
    return eb(IR_OR, shl, one);
}

static int
lower_divop(val_t rest, int op)
{
    int ra = scm_untag(lower_expr(gc_car(rest), 0));
    int rb = scm_untag(lower_expr(gc_car(gc_cdr(rest)), 0));
    return scm_retag(eb(op, ra, rb));
}

static int
lower_modulo(val_t rest)
{
    int ra = scm_untag(lower_expr(gc_car(rest), 0));
    int rb = scm_untag(lower_expr(gc_car(gc_cdr(rest)), 0));
    int r = eb(IR_MODS, ra, rb);
    /* adjust toward the divisor's sign: when r != 0 and sign(r) != sign(b),
       add b.  Branchless: mask = -(adjust) is all-ones, else zero. */
    int xr = eb(IR_XOR, r, rb);
    int signdiff = eb(IR_CMPLTS, xr, lower_const(0));
    int iszero = eb(IR_CMPEQ, r, lower_const(0));
    int nonzero = eb(IR_XOR, iszero, lower_const(1));
    int adjust = eb(IR_AND, signdiff, nonzero);
    int mask = eb(IR_SUB, lower_const(0), adjust);     /* -adjust: all-ones or 0 */
    int addend = eb(IR_AND, rb, mask);
    return scm_retag(eb(IR_ADD, r, addend));
}

static int
lower_abs(val_t rest)
{
    int raw = scm_untag(lower_expr(gc_car(rest), 0));
    /* branchless: (x ^ (x>>31)) - (x>>31) */
    int sign = eb(IR_SHRS, raw, lower_const(31));
    int flip = eb(IR_XOR, raw, sign);
    return scm_retag(eb(IR_SUB, flip, sign));
}

static int
lower_minmax(val_t rest, int is_max)
{
    /* tagged fixnums keep fixnum order under a signed compare, so no untag is
       needed.  Branchless select: sel = (a^b) & -(a<b). */
    int a = lower_expr(gc_car(rest), 0);
    int b = lower_expr(gc_car(gc_cdr(rest)), 0);
    int lt = eb(IR_CMPLTS, a, b);
    int mask = eb(IR_SUB, lower_const(0), lt);          /* -(a<b) */
    int axb = eb(IR_XOR, a, b);
    int sel = eb(IR_AND, axb, mask);
    /* a<b: min=a, max=b; else min=b, max=a */
    return is_max ? eb(IR_XOR, a, sel) : eb(IR_XOR, b, sel);
}

/****************************************************************
 * Quoted data
 *
 * A quoted datum becomes runtime construction: immediates (fixnum, nil, #t/#f)
 * are their own machine word; a quoted symbol is an interned rodata object so
 * eq? identity holds; a quoted pair/list is built with runtime cons cells.
 ****************************************************************/

/* an interned symbol object: [type byte OBJ_SYMBOL][name][NUL], word-aligned so
   its address carries the heap-pointer tag (low 2 bits zero).  Returns the
   global's symbol name. */
static const char *
intern_symbol(const char *name)
{
    struct ir_global *g;
    char buf[32];
    char *data;
    int nl = (int)strlen(name);
    int i;

    for (i = 0; i < g_nqsyms; i++)
        if (strcmp(g_qsyms[i].name, name) == 0)
            return g_qsyms[i].gname;
    if (g_nqsyms >= MAX_QSYMS)
        die("lower: too many quoted symbols");

    snprintf(buf, sizeof(buf), "__scm_sym_%d", g_nqsyms);
    g = arena_zalloc(lower_arena, sizeof(*g));
    g->name = arena_strdup(lower_arena, buf);
    g->base_type = IR_I8;
    g->arr_size = 1 + nl + 1;
    g->align = VWORD;
    g->is_rodata = 1;
    data = arena_alloc(lower_arena, g->arr_size);
    data[0] = (char)OBJ_SYMBOL;
    memcpy(data + 1, name, nl);
    data[1 + nl] = '\0';
    g->init_string = data;
    g->init_strlen = g->arr_size;
    g->next = cur_prog->globals;
    cur_prog->globals = g;

    g_qsyms[g_nqsyms].name = arena_strdup(lower_arena, name);
    g_qsyms[g_nqsyms].gname = g->name;
    g_nqsyms++;
    return g->name;
}

static int
lower_quote_symbol(const char *name)
{
    const char *g = intern_symbol(name);
    struct ir_insn *ins = emit(IR_VLEA);
    ins->dst = new_temp();
    ins->sym = arena_strdup(lower_arena, g);
    return ins->dst;
}

static int
lower_quote_datum(val_t v)
{
    if (IS_FIX(v) || IS_NIL(v) || IS_IMM(v))
        return lower_const((long)v);
    if (is_symbol(v))
        return lower_quote_symbol(sym_name(v));
    if (is_pair(v)) {
        int ca = lower_quote_datum(gc_car(v));
        int cd = lower_quote_datum(gc_cdr(v));
        return emit_cons(ca, cd);
    }
    die("lower: cannot quote this datum");
    return 0;
}

static int
lower_eq(val_t args)
{
    int a = lower_expr(gc_car(args), 0);
    int b = lower_expr(gc_car(gc_cdr(args)), 0);
    return bool_from_cmp(eb(IR_CMPEQ, a, b));
}

/****************************************************************
 * Expression lowering
 ****************************************************************/

static int
lower_expr(val_t expr, int tail)
{
    struct ir_insn *ins;

    if (IS_FIX(expr))
        return lower_const((long)GET_FIX(expr) * 4 + 1);

    if (expr == VAL_TRUE)
        return lower_const((long)VAL_TRUE);
    if (expr == VAL_FALSE)
        return lower_const((long)VAL_FALSE);
    if (IS_NIL(expr))
        return lower_const(0);

    if (is_symbol(expr)) {
        const char *name = sym_name(expr);
        struct lsym *ls = find_lsym(name);
        struct gsym *gs;

        if (ls) {
            ins = emit(IR_VLDL);
            ins->dst = new_temp();
            ins->slot = ls->slot;
            return ins->dst;
        }
        gs = find_gsym(name);
        if (!gs)
            die("lower: undefined '%s'", name);
        if (gs->kind == GSYM_FUNC) {
            char cname[288];

            /* A reference to a global function is a closure with no free
             * variables, i.e. a constant { tag, code-ptr }.  Point at one
             * static closure per function rather than bump-allocating an
             * identical copy from __heap on every evaluation (which, inside
             * a loop, leaks the closure heap turn by turn). */
            gs->needs_closure = 1;
            snprintf(cname, sizeof(cname), "__closure_%s", name);
            ins = emit(IR_VLEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena, cname);
            return ins->dst;
        }
        /* Global variable: LEA + LD */
        {
            int taddr;

            ins = emit(IR_VLEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena, name);
            taddr = ins->dst;
            ins = emit(IR_VLD);
            ins->dst = new_temp();
            ins->a = taddr;
            return ins->dst;
        }
    }

    if (is_pair(expr)) {
        val_t head = gc_car(expr);
        val_t rest = gc_cdr(expr);

        if (is_sym(head, "if"))
            return lower_if(rest, tail);
        if (is_sym(head, "+"))
            return lower_add(rest);
        if (is_sym(head, "-"))
            return lower_sub(rest);
        if (is_sym(head, "*"))
            return lower_mul(rest);
        if (is_sym(head, "="))
            return lower_cmp(rest, IR_CMPEQ);
        if (is_sym(head, "<"))
            return lower_cmp(rest, IR_CMPLTS);
        if (is_sym(head, ">"))
            return lower_cmp(rest, IR_CMPGTS);
        if (is_sym(head, "<="))
            return lower_cmp(rest, IR_CMPLES);
        if (is_sym(head, ">="))
            return lower_cmp(rest, IR_CMPGES);
        if (is_sym(head, "lambda"))
            return lower_lambda(rest);
        if (is_sym(head, "begin")) {
            int t = lower_const(0);
            val_t cur;

            for (cur = rest; !IS_NIL(cur); cur = gc_cdr(cur)) {
                int is_last = IS_NIL(gc_cdr(cur));

                t = lower_expr(gc_car(cur),
                    is_last ? tail : 0);
            }
            return t;
        }
        if (is_sym(head, "quote"))
            return lower_quote_datum(gc_car(rest));

        if (is_sym(head, "reset"))
            return lower_reset(rest);
        if (is_sym(head, "shift"))
            return lower_shift();
        if (is_sym(head, "resume"))
            return lower_resume(rest);

        if (is_sym(head, "cons"))
            return lower_cons(rest);
        if (is_sym(head, "car"))
            return lower_car(rest);
        if (is_sym(head, "cdr"))
            return lower_cdr(rest);
        if (is_sym(head, "null?"))
            return lower_null_p(rest);
        if (is_sym(head, "pair?"))
            return lower_pair_p(rest);
        if (is_sym(head, "set-car!"))
            return lower_set_car(rest);
        if (is_sym(head, "set-cdr!"))
            return lower_set_cdr(rest);
        if (is_sym(head, "list"))
            return lower_list(rest);

        if (is_sym(head, "display"))
            return lower_display(rest);
        if (is_sym(head, "newline"))
            return lower_newline();
        if (is_sym(head, "write-string"))
            return lower_write_string(rest);

        if (is_sym(head, "let"))
            return lower_let(rest, 0);
        if (is_sym(head, "let*"))
            return lower_let(rest, 1);
        if (is_sym(head, "cond"))
            return lower_cond(rest);
        if (is_sym(head, "when"))
            return lower_when(rest, 0);
        if (is_sym(head, "unless"))
            return lower_when(rest, 1);
        if (is_sym(head, "and"))
            return lower_and_or(rest, 0);
        if (is_sym(head, "or"))
            return lower_and_or(rest, 1);
        if (is_sym(head, "not"))
            return lower_not(rest);

        if (is_sym(head, "quotient"))
            return lower_divop(rest, IR_DIVS);
        if (is_sym(head, "remainder"))
            return lower_divop(rest, IR_MODS);
        if (is_sym(head, "modulo"))
            return lower_modulo(rest);
        if (is_sym(head, "abs"))
            return lower_abs(rest);
        if (is_sym(head, "min"))
            return lower_minmax(rest, 0);
        if (is_sym(head, "max"))
            return lower_minmax(rest, 1);

        if (is_sym(head, "eq?"))
            return lower_eq(rest);

        return lower_call(expr, tail);
    }

    die("lower: unhandled expression type");
    return 0;
}

/****************************************************************
 * Function lowering
 ****************************************************************/

static void
finish_slots(struct ir_func *fn)
{
    fn->nslots = nslots;
    if (nslots > 0) {
        fn->slot_size = arena_alloc(lower_arena, nslots * sizeof(int));
        memcpy(fn->slot_size, slot_sizes,
            nslots * sizeof(int));
    } else {
        fn->slot_size = NULL;
    }
}

static struct ir_func *
lower_function(const char *name, val_t params, val_t body,
    char **free_vars, int nfree)
{
    struct ir_func *fn;
    struct ir_insn *ins;
    val_t cur;
    int nparams = 0;
    int t_result = -1;
    int i;

    fn = ir_new_func(lower_arena, name);
    cur_fn = fn;
    nlsyms = 0;
    nslots = 0;

    add_lsym("__env");
    nparams++;
    for (cur = params; !IS_NIL(cur); cur = gc_cdr(cur)) {
        add_lsym(sym_name(gc_car(cur)));
        nparams++;
    }
    fn->nparams = nparams;

    for (i = 0; i < nfree; i++)
        add_lsym(free_vars[i]);

    ins = emit(IR_FUNC);
    ins->sym = arena_strdup(lower_arena, name);
    ins->nargs = nparams;

    if (nfree > 0) {
        int t_env;

        ins = emit(IR_VLDL);
        ins->dst = new_temp();
        ins->slot = 0;
        t_env = ins->dst;
        for (i = 0; i < nfree; i++) {
            struct lsym *ls = find_lsym(free_vars[i]);
            int t_off, t_addr, t_val;

            t_off = lower_const(2 * VWORD + VWORD * i);
            ins = emit(IR_VADD);
            ins->dst = new_temp();
            ins->a = t_env;
            ins->b = t_off;
            t_addr = ins->dst;
            ins = emit(IR_VLD);
            ins->dst = new_temp();
            ins->a = t_addr;
            t_val = ins->dst;
            ins = emit(IR_VSTL);
            ins->a = t_val;
            ins->slot = ls->slot;
        }
    }

    for (cur = body; !IS_NIL(cur); cur = gc_cdr(cur)) {
        int is_last = IS_NIL(gc_cdr(cur));

        t_result = lower_expr(gc_car(cur), is_last ? 1 : 0);
    }

    if (t_result >= 0) {
        ins = emit(IR_RETV);
        ins->a = t_result;
    }

    emit(IR_ENDF);
    finish_slots(fn);
    return fn;
}

/****************************************************************
 * Top-level form classification
 *
 * Returns 1 for (define (name params...) body...),
 *         2 for (define name expr),
 *         0 for other forms.
 ****************************************************************/

static int
classify_define(val_t form, val_t *name, val_t *params,
                val_t *body, val_t *init)
{
    val_t binding;

    if (!is_pair(form) || !is_sym(gc_car(form), "define"))
        return 0;
    binding = gc_car(gc_cdr(form));
    if (is_pair(binding)) {
        *name = gc_car(binding);
        *params = gc_cdr(binding);
        *body = gc_cdr(gc_cdr(form));
        return 1;
    }
    if (is_symbol(binding)) {
        *name = binding;
        *init = gc_car(gc_cdr(gc_cdr(form)));
        return 2;
    }
    die("lower: malformed define");
    return 0;
}

/****************************************************************
 * Entry point
 ****************************************************************/

struct ir_program *
scm_lower(struct arena *a, struct gc_heap *h, val_t program)
{
    struct ir_program *prog;
    struct ir_func **ftail;
    val_t cur;

    lower_arena = a;
#ifdef CC_WASM
    lower_heap = h;
#endif
    prog = arena_zalloc(lower_arena, sizeof(*prog));
    cur_prog = prog;
    ngsyms = 0;
    g_display_used = 0;
    g_str_serial = 0;
    g_nqsyms = 0;
    lifted_head = NULL;
    lifted_tail = NULL;
    lambda_serial = 0;

    /* Pass 1: register all global names */
    for (cur = program; !IS_NIL(cur); cur = gc_cdr(cur)) {
        val_t form = gc_car(cur);
        val_t name = VAL_NIL, params = VAL_NIL;
        val_t body = VAL_NIL, init = VAL_NIL;
        int kind = classify_define(form, &name, &params,
                                   &body, &init);

        if (kind == 1) {
            int np = 0;
            val_t pcur;

            for (pcur = params; !IS_NIL(pcur);
                 pcur = gc_cdr(pcur))
                np++;
            add_gsym(sym_name(name), GSYM_FUNC, np);
        } else if (kind == 2) {
            add_gsym(sym_name(name), GSYM_VAR, 0);
        }
    }

    if (!find_gsym("write"))
        add_gsym("write", GSYM_RUNTIME, 3);
    if (!find_gsym("exit"))
        add_gsym("exit", GSYM_RUNTIME, 1);

    /* Create IR globals for variable definitions */
    for (cur = program; !IS_NIL(cur); cur = gc_cdr(cur)) {
        val_t form = gc_car(cur);
        val_t name = VAL_NIL, params = VAL_NIL;
        val_t body = VAL_NIL, init = VAL_NIL;

        if (classify_define(form, &name, &params,
                            &body, &init) == 2) {
            struct ir_global *g;

            g = arena_zalloc(lower_arena, sizeof(*g));
            g->name = arena_strdup(lower_arena, sym_name(name));
            g->base_type = IR_VBASE;
            g->next = prog->globals;
            prog->globals = g;
        }
    }

    /* Heap for bump-allocated pairs (cons cells) */
    {
        struct ir_global *g;

        g = arena_zalloc(lower_arena, sizeof(*g));
        g->name = arena_strdup(lower_arena, "__heap");
        g->base_type = IR_I8;
        g->arr_size = 8192;
        g->next = prog->globals;
        prog->globals = g;

        g = arena_zalloc(lower_arena, sizeof(*g));
        g->name = arena_strdup(lower_arena, "__heap_ptr");
        g->base_type = IR_VBASE;
        g->next = prog->globals;
        prog->globals = g;
    }

    ftail = &prog->funcs;

    /* Pass 2: lower defined functions */
    for (cur = program; !IS_NIL(cur); cur = gc_cdr(cur)) {
        val_t form = gc_car(cur);
        val_t name = VAL_NIL, params = VAL_NIL;
        val_t body = VAL_NIL, init = VAL_NIL;

        if (classify_define(form, &name, &params,
                            &body, &init) == 1) {
            struct ir_func *fn;

            fn = lower_function(sym_name(name),
                params, body, NULL, 0);
            *ftail = fn;
            ftail = &fn->next;
        }
    }

    /* Pass 3: generate main — init globals, eval top-level exprs,
     * untag last result for exit code */
    {
        struct ir_func *fn;
        struct ir_insn *ins;
        int t_last = -1;

        fn = ir_new_func(lower_arena, "main");
        cur_fn = fn;
        fn->nparams = 0;
        nlsyms = 0;
        nslots = 0;

        ins = emit(IR_FUNC);
        ins->sym = arena_strdup(lower_arena, "main");
        ins->nargs = 0;

        /* init heap pointer */
        {
            int t_heap, t_addr;

            ins = emit(IR_VLEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena, "__heap");
            t_heap = ins->dst;
            ins = emit(IR_VLEA);
            ins->dst = new_temp();
            ins->sym = arena_strdup(lower_arena, "__heap_ptr");
            t_addr = ins->dst;
            ins = emit(IR_VST);
            ins->a = t_addr;
            ins->b = t_heap;
        }

        for (cur = program; !IS_NIL(cur);
             cur = gc_cdr(cur)) {
            val_t form = gc_car(cur);
            val_t name = VAL_NIL, params = VAL_NIL;
            val_t body = VAL_NIL, init = VAL_NIL;
            int kind = classify_define(form, &name,
                &params, &body, &init);

            if (kind == 1) {
                continue;
            } else if (kind == 2) {
                int tval, taddr;

                tval = lower_expr(init, 0);
                ins = emit(IR_VLEA);
                ins->dst = new_temp();
                ins->sym = arena_strdup(lower_arena, sym_name(name));
                taddr = ins->dst;
                ins = emit(IR_VST);
                ins->a = taddr;
                ins->b = tval;
            } else {
                t_last = lower_expr(form, 0);
            }
        }

        if (t_last >= 0) {
            int t_two, t_untagged;

            t_two = lower_const(2);
            ins = emit(IR_SHRS);
            ins->dst = new_temp();
            ins->a = t_last;
            ins->b = t_two;
            t_untagged = ins->dst;
            ins = emit(IR_RETV);
            ins->a = t_untagged;
        } else {
            int z = lower_const(0);

            ins = emit(IR_RETV);
            ins->a = z;
        }

        emit(IR_ENDF);
        finish_slots(fn);
        *ftail = fn;
        ftail = &fn->next;
    }

    /* Pass 4: compile lifted lambdas */
    while (lifted_head) {
        struct lifted *l = lifted_head;
        struct ir_func *fn;

        lifted_head = l->next;
        fn = lower_function(l->name, l->params, l->body,
            l->free_vars, l->nfree);
        *ftail = fn;
        ftail = &fn->next;
        }

    /* One static closure per global function referenced as a value:
     * a { type byte 3, pad, code pointer } image (matching emit_closure's
     * layout, 2*VWORD bytes), so a bare function reference is a constant
     * address rather than a fresh __heap allocation. */
    {
        int gi;

        for (gi = 0; gi < ngsyms; gi++) {
            struct ir_global *g;
            struct ir_init *tag, *fnp;
            char cname[288];

            if (gsyms[gi].kind != GSYM_FUNC || !gsyms[gi].needs_closure)
                continue;

            tag = arena_zalloc(lower_arena, sizeof(*tag));
            tag->offset = 0;
            tag->size = 1;
            tag->ival = 3;                  /* OBJ_CLOSURE */

            fnp = arena_zalloc(lower_arena, sizeof(*fnp));
            fnp->offset = VWORD;
            fnp->size = VWORD;
            fnp->sym = arena_strdup(lower_arena, gsyms[gi].name);
            tag->next = fnp;

            snprintf(cname, sizeof(cname), "__closure_%s", gsyms[gi].name);
            g = arena_zalloc(lower_arena, sizeof(*g));
            g->name = arena_strdup(lower_arena, cname);
            g->base_type = IR_I8;
            g->arr_size = 2 * VWORD;
            g->align = VWORD;
            g->is_local = 1;
            g->inits = tag;
            g->next = prog->globals;
            prog->globals = g;
        }
    }

    free(gsyms);
    gsyms = NULL;
    ngsyms = gsym_cap = 0;

    /* emit the display helper last, after every body (including lifted
     * lambdas) has had a chance to reference it */
    if (g_display_used) {
        struct ir_func *fn = build_display_helper();
        *ftail = fn;
        ftail = &fn->next;
    }

    free(lsyms);
    lsyms = NULL;
    nlsyms = lsym_cap = 0;

    free(slot_sizes);
    slot_sizes = NULL;
    nslots = slot_cap = 0;

    return prog;
}
