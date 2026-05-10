/* test_ops.c : IR-level integration test for unsigned 32-bit ops */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

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
new_cmpeq(struct ir_func *fn, int a, int b)
{
    return new_binop(fn, IR_CMPEQ, a, b);
}

static void
emit_bz(struct ir_func *fn, int cond, int label)
{
    struct ir_insn *i = ir_emit(fn, IR_BZ);

    i->a = cond;
    i->label = label;
}

static void
emit_bnz(struct ir_func *fn, int cond, int label)
{
    struct ir_insn *i = ir_emit(fn, IR_BNZ);

    i->a = cond;
    i->label = label;
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
    int lfail;
    int va, vb, r, cmp;

    fn = ir_new_func(a, "main");
    fn->nparams = 0;
    fn->nslots = 0;
    fn->slot_size = NULL;

    lfail = ir_new_label(fn);

    ir_emit(fn, IR_FUNC);

    /* --- DIVU: 100 / 7 = 14 --- */
    va = new_lic(fn, 100);
    vb = new_lic(fn, 7);
    r = new_binop(fn, IR_DIVU, va, vb);
    cmp = new_cmpeq(fn, r, new_lic(fn, 14));
    emit_bz(fn, cmp, lfail);

    /* --- DIVU: 0xFFFFFFFF / 2 = 0x7FFFFFFF --- */
    va = new_lic(fn, -1);
    vb = new_lic(fn, 2);
    r = new_binop(fn, IR_DIVU, va, vb);
    cmp = new_cmpeq(fn, r, new_lic(fn, 0x7FFFFFFF));
    emit_bz(fn, cmp, lfail);

    /* --- MODU: 100 % 7 = 2 --- */
    va = new_lic(fn, 100);
    vb = new_lic(fn, 7);
    r = new_binop(fn, IR_MODU, va, vb);
    cmp = new_cmpeq(fn, r, new_lic(fn, 2));
    emit_bz(fn, cmp, lfail);

    /* --- MODU: 0xFFFFFFFF % 2 = 1 --- */
    va = new_lic(fn, -1);
    vb = new_lic(fn, 2);
    r = new_binop(fn, IR_MODU, va, vb);
    cmp = new_cmpeq(fn, r, new_lic(fn, 1));
    emit_bz(fn, cmp, lfail);

    /* --- SHRU: 0x80000000 >> 1 = 0x40000000 --- */
    va = new_lic(fn, (long)0x80000000U);
    vb = new_lic(fn, 1);
    r = new_binop(fn, IR_SHRU, va, vb);
    cmp = new_cmpeq(fn, r, new_lic(fn, 0x40000000));
    emit_bz(fn, cmp, lfail);

    /* --- SHRU: 0xFF000000 >> 24 = 0xFF --- */
    va = new_lic(fn, (long)0xFF000000U);
    vb = new_lic(fn, 24);
    r = new_binop(fn, IR_SHRU, va, vb);
    cmp = new_cmpeq(fn, r, new_lic(fn, 0xFF));
    emit_bz(fn, cmp, lfail);

    /* --- CMPLTU: 0 < 1 (true) --- */
    va = new_lic(fn, 0);
    vb = new_lic(fn, 1);
    cmp = new_binop(fn, IR_CMPLTU, va, vb);
    emit_bz(fn, cmp, lfail);

    /* --- CMPLTU: 1 < 0 (false) --- */
    cmp = new_binop(fn, IR_CMPLTU, vb, va);
    emit_bnz(fn, cmp, lfail);

    /* --- CMPLTU: 0x7FFFFFFF < 0x80000000 (true unsigned) --- */
    va = new_lic(fn, 0x7FFFFFFF);
    vb = new_lic(fn, (long)0x80000000U);
    cmp = new_binop(fn, IR_CMPLTU, va, vb);
    emit_bz(fn, cmp, lfail);

    /* --- CMPLEU: 5 <= 5 (true) --- */
    va = new_lic(fn, 5);
    cmp = new_binop(fn, IR_CMPLEU, va, va);
    emit_bz(fn, cmp, lfail);

    /* --- CMPLEU: 5 <= 4 (false) --- */
    vb = new_lic(fn, 4);
    cmp = new_binop(fn, IR_CMPLEU, va, vb);
    emit_bnz(fn, cmp, lfail);

    /* --- CMPGTU: 0x80000000 > 0x7FFFFFFF (true unsigned) --- */
    va = new_lic(fn, (long)0x80000000U);
    vb = new_lic(fn, 0x7FFFFFFF);
    cmp = new_binop(fn, IR_CMPGTU, va, vb);
    emit_bz(fn, cmp, lfail);

    /* --- CMPGTU: 5 > 5 (false) --- */
    va = new_lic(fn, 5);
    cmp = new_binop(fn, IR_CMPGTU, va, va);
    emit_bnz(fn, cmp, lfail);

    /* --- CMPGEU: 5 >= 5 (true) --- */
    cmp = new_binop(fn, IR_CMPGEU, va, va);
    emit_bz(fn, cmp, lfail);

    /* --- CMPGEU: 4 >= 5 (false) --- */
    vb = new_lic(fn, 4);
    cmp = new_binop(fn, IR_CMPGEU, vb, va);
    emit_bnz(fn, cmp, lfail);

    /* --- CMPGEU: 0xFFFFFFFF >= 0 (true unsigned) --- */
    va = new_lic(fn, -1);
    vb = new_lic(fn, 0);
    cmp = new_binop(fn, IR_CMPGEU, va, vb);
    emit_bz(fn, cmp, lfail);

    /* success: return 0 */
    emit_retv(fn, new_lic(fn, 0));

    /* fail: return 1 */
    emit_label(fn, lfail);
    emit_retv(fn, new_lic(fn, 1));

    ir_emit(fn, IR_ENDF);
    return fn;
}

int
main(int argc, char **argv)
{
    struct arena a;
    struct ir_program prog;
    struct ir_func *fn;
    FILE *out;
    const char *outpath = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outpath = argv[++i];
    }
    if (!outpath)
        die("usage: test_ops -o <output.s>");

    arena_init(&a);
    fn = build_test(&a);
    regalloc(fn);

    prog.funcs = fn;
    prog.globals = NULL;

    out = fopen(outpath, "w");
    if (!out)
        die("cannot open %s", outpath);
    target_emit(out, &prog);
    fclose(out);

    arena_free(&a);
    return 0;
}
