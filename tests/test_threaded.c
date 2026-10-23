/* test_threaded.c : IR-level integration test for computed-goto codegen.
   Exercises the two threaded-dispatch opcodes IR_LABADDR (address of a local
   code label into a temp) and IR_IJMP (indirect jump through a temp), the
   indirect-threading / gforth-NEXT primitive, without any front end.  It builds
   a 3-entry table of label addresses in a global array, selects one by a
   runtime index, jumps through it, and returns 0 only when the chosen label ran
   (self-checking: a wrong landing returns 1). */

#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
new_lic(struct ir_func *fn, long val)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_LIC);

    i->dst = t;
    i->imm = val;
    return t;
}

static int
new_binop(struct ir_func *fn, int op, int a, int b)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, op);

    i->dst = t;
    i->a = a;
    i->b = b;
    return t;
}

static int
new_lea(struct ir_func *fn, const char *sym)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_LEA);

    i->dst = t;
    i->sym = xstrdup(sym);
    return t;
}

static int
new_labaddr(struct ir_func *fn, int label)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_LABADDR);

    i->dst = t;
    i->label = label;
    return t;
}

static int
new_lw(struct ir_func *fn, int addr)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_LW);

    i->dst = t;
    i->a = addr;
    return t;
}

static void
emit_sw(struct ir_func *fn, int addr, int val)
{
    struct ir_insn *i = ir_emit(fn, IR_SW);

    i->a = addr;
    i->b = val;
}

static void
emit_ijmp(struct ir_func *fn, int addr)
{
    struct ir_insn *i = ir_emit(fn, IR_IJMP);

    i->a = addr;
}

static void
emit_label(struct ir_func *fn, int label)
{
    struct ir_insn *i = ir_emit(fn, IR_LABEL);

    i->label = label;
}

static void
emit_retv(struct ir_func *fn, int val)
{
    struct ir_insn *i = ir_emit(fn, IR_RETV);

    i->a = val;
}

static struct ir_func *
build_test(struct arena *a)
{
    struct ir_func *fn;
    int labels[3];
    int base, ak, addr, idx, off, eaddr, tgt;
    int k;

    fn = ir_new_func(a, "main");
    fn->nparams = 0;
    fn->nslots = 0;

    labels[0] = ir_new_label(fn);
    labels[1] = ir_new_label(fn);
    labels[2] = ir_new_label(fn);

    ir_emit(fn, IR_FUNC);

    /* g_disp[k] = &&labels[k] */
    for (k = 0; k < 3; k++) {
        ak = new_labaddr(fn, labels[k]);
        base = new_lea(fn, "g_disp");
        if (k == 0) {
            emit_sw(fn, base, ak);
        } else {
            addr = new_binop(fn, IR_ADD, base, new_lic(fn, k * 4));
            emit_sw(fn, addr, ak);
        }
    }

    /* jump through g_disp[2], a runtime-indexed load (the builder does not
       constant-fold, so the index flows through a register). */
    idx = new_lic(fn, 2);
    off = new_binop(fn, IR_SHL, idx, new_lic(fn, 2));   /* idx * 4 */
    base = new_lea(fn, "g_disp");
    eaddr = new_binop(fn, IR_ADD, base, off);
    tgt = new_lw(fn, eaddr);
    emit_ijmp(fn, tgt);

    /* only the third label (the selected one) returns success */
    emit_label(fn, labels[0]);
    emit_retv(fn, new_lic(fn, 1));
    emit_label(fn, labels[1]);
    emit_retv(fn, new_lic(fn, 1));
    emit_label(fn, labels[2]);
    emit_retv(fn, new_lic(fn, 0));

    ir_emit(fn, IR_ENDF);
    return fn;
}

int
main(int argc, char **argv)
{
    struct arena a;
    struct ir_program prog;
    struct ir_func *fn;
    struct ir_global *g;
    FILE *out;
    const char *outpath = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outpath = argv[++i];
    }
    if (!outpath)
        die("usage: test_threaded -o <output.s>");

    arena_init(&a);
    fn = build_test(&a);
    regalloc(fn);

    g = xmalloc(sizeof *g);
    memset(g, 0, sizeof *g);
    g->name = xstrdup("g_disp");
    g->base_type = IR_I32;
    g->arr_size = 3;
    g->next = NULL;

    prog.funcs = fn;
    prog.globals = g;

    out = fopen(outpath, "w");
    if (!out)
        die("cannot open %s", outpath);
    target_emit(out, &prog);
    fclose(out);
    return 0;
}
