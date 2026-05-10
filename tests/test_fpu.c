/* test_fpu.c : IR-level integration test for ColdFire float codegen */

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
new_itof(struct ir_func *fn, int src)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_ITOF);

    i->dst = t;
    i->a = src;
    return t;
}

static int
new_ftoi(struct ir_func *fn, int src)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_FTOI);

    i->dst = t;
    i->a = src;
    return t;
}

static int
new_fbinop(struct ir_func *fn, int op, int a, int b)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, op);

    i->dst = t;
    i->a = a;
    i->b = b;
    return t;
}

static int
new_funop(struct ir_func *fn, int op, int a)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, op);

    i->dst = t;
    i->a = a;
    return t;
}

static int
new_cmpeq(struct ir_func *fn, int a, int b)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_CMPEQ);

    i->dst = t;
    i->a = a;
    i->b = b;
    return t;
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
    int i10, i3, i4;
    int f10, f3, f4;
    int fadd_r, fsub_r, fmul_r, fdiv_r, fneg_r, fabs_r;
    int ti, cmp;
    int fslot;

    fn = ir_new_func(a, "main");
    fn->nparams = 0;
    fn->nslots = 1;
    fn->slot_size = xmalloc(sizeof(int));
    fn->slot_size[0] = 8;

    lfail = ir_new_label(fn);

    ir_emit(fn, IR_FUNC);

    /* integer constants */
    i10 = new_lic(fn, 10);
    i3 = new_lic(fn, 3);
    i4 = new_lic(fn, 4);

    /* convert to float */
    f10 = new_itof(fn, i10);
    f3 = new_itof(fn, i3);
    f4 = new_itof(fn, i4);

    /* fadd: 10.0 + 3.0 = 13.0 → 13 */
    fadd_r = new_fbinop(fn, IR_FADD, f10, f3);
    ti = new_ftoi(fn, fadd_r);
    cmp = new_cmpeq(fn, ti, new_lic(fn, 13));
    emit_bz(fn, cmp, lfail);

    /* fsub: 13.0 - 10.0 = 3.0 → 3 */
    fsub_r = new_fbinop(fn, IR_FSUB, fadd_r, f10);
    ti = new_ftoi(fn, fsub_r);
    cmp = new_cmpeq(fn, ti, i3);
    emit_bz(fn, cmp, lfail);

    /* fmul: 3.0 * 4.0 = 12.0 → 12 */
    fmul_r = new_fbinop(fn, IR_FMUL, f3, f4);
    ti = new_ftoi(fn, fmul_r);
    cmp = new_cmpeq(fn, ti, new_lic(fn, 12));
    emit_bz(fn, cmp, lfail);

    /* fdiv: 12.0 / 3.0 = 4.0 → 4 */
    fdiv_r = new_fbinop(fn, IR_FDIV, fmul_r, f3);
    ti = new_ftoi(fn, fdiv_r);
    cmp = new_cmpeq(fn, ti, i4);
    emit_bz(fn, cmp, lfail);

    /* fneg: -(3.0) = -3.0 → -3 */
    fneg_r = new_funop(fn, IR_FNEG, f3);
    ti = new_ftoi(fn, fneg_r);
    cmp = new_cmpeq(fn, ti, new_lic(fn, -3));
    emit_bz(fn, cmp, lfail);

    /* fabs: |(-3.0)| = 3.0 → 3 */
    fabs_r = new_funop(fn, IR_FABS, fneg_r);
    ti = new_ftoi(fn, fabs_r);
    cmp = new_cmpeq(fn, ti, i3);
    emit_bz(fn, cmp, lfail);

    /* fcmpeq true: 3.0 == 3.0 */
    cmp = new_fbinop(fn, IR_FCMPEQ, fabs_r, f3);
    emit_bz(fn, cmp, lfail);

    /* fcmpeq false: 3.0 == 10.0 */
    cmp = new_fbinop(fn, IR_FCMPEQ, f3, f10);
    emit_bnz(fn, cmp, lfail);

    /* fcmplt true: 3.0 < 10.0 */
    cmp = new_fbinop(fn, IR_FCMPLT, f3, f10);
    emit_bz(fn, cmp, lfail);

    /* fcmplt false: 10.0 < 3.0 */
    cmp = new_fbinop(fn, IR_FCMPLT, f10, f3);
    emit_bnz(fn, cmp, lfail);

    /* fcmple true (equal): 3.0 <= 3.0 */
    cmp = new_fbinop(fn, IR_FCMPLE, fabs_r, f3);
    emit_bz(fn, cmp, lfail);

    /* fcmple true (less): 3.0 <= 10.0 */
    cmp = new_fbinop(fn, IR_FCMPLE, f3, f10);
    emit_bz(fn, cmp, lfail);

    /* fcmple false: 10.0 <= 3.0 */
    cmp = new_fbinop(fn, IR_FCMPLE, f10, f3);
    emit_bnz(fn, cmp, lfail);

    /* fstl/fldl: store 10.0 to slot, load back, check */
    {
        struct ir_insn *st = ir_emit(fn, IR_FSTL);
        st->a = f10;
        st->slot = 0;

        fslot = ir_new_temp(fn);
        {
            struct ir_insn *ld = ir_emit(fn, IR_FLDL);
            ld->dst = fslot;
            ld->slot = 0;
        }
        ti = new_ftoi(fn, fslot);
        cmp = new_cmpeq(fn, ti, i10);
        emit_bz(fn, cmp, lfail);
    }

    /* pass */
    emit_retv(fn, new_lic(fn, 0));

    /* fail */
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
    int k;

    util_set_progname("test_fpu");

    for (k = 1; k < argc; k++) {
        if (strcmp(argv[k], "-o") == 0 && k + 1 < argc)
            outpath = argv[++k];
        else
            die("usage: test_fpu [-o output.s]");
    }

    arena_init(&a);

    fn = build_test(&a);
    regalloc(fn);

    prog.funcs = fn;
    prog.globals = NULL;

    if (outpath) {
        out = fopen(outpath, "w");
        if (!out)
            die("cannot open %s", outpath);
    } else {
        out = stdout;
    }

    target_emit(out, &prog);

    if (outpath)
        fclose(out);

    arena_free(&a);

    return 0;
}
