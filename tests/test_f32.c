/* test_f32.c : IR-level integration test for native single-precision (IR_F32).
 *
 * Every float opcode carries FWIDTH_F32 in its imm, so the backend selects the
 * single-precision SSE form (addss, ucomiss, cvtsi2ss, ...).  Also exercises
 * the cross-width conversions IR_F32TOF64 / IR_F64TOF32.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

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

/* int -> F32 */
static int
new_itof32(struct ir_func *fn, int src)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_ITOF);
    i->dst = t;
    i->a = src;
    i->imm = FWIDTH_F32;
    return t;
}

/* F32 -> int (truncating) */
static int
new_f32toi(struct ir_func *fn, int src)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_FTOI);
    i->dst = t;
    i->a = src;
    i->imm = FWIDTH_F32;
    return t;
}

static int
new_fbinop32(struct ir_func *fn, int op, int a, int b)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, op);
    i->dst = t;
    i->a = a;
    i->b = b;
    i->imm = FWIDTH_F32;
    return t;
}

static int
new_funop32(struct ir_func *fn, int op, int a)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, op);
    i->dst = t;
    i->a = a;
    i->imm = FWIDTH_F32;
    return t;
}

static int
new_conv(struct ir_func *fn, int op, int a)
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
    int i10, i3, i4, f10, f3, f4, r, ti, cmp, fslot;

    fn = ir_new_func(a, "main");
    fn->nparams = 0;
    fn->nslots = 1;
    fn->slot_size = xmalloc(sizeof(int));
    fn->slot_size[0] = 8;              /* one float slot (8-byte uniform) */

    lfail = ir_new_label(fn);
    ir_emit(fn, IR_FUNC);

    i10 = new_lic(fn, 10);
    i3  = new_lic(fn, 3);
    i4  = new_lic(fn, 4);
    f10 = new_itof32(fn, i10);
    f3  = new_itof32(fn, i3);
    f4  = new_itof32(fn, i4);

    /* addss: 10 + 3 = 13 */
    r = new_fbinop32(fn, IR_FADD, f10, f3);
    ti = new_f32toi(fn, r);
    cmp = new_cmpeq(fn, ti, new_lic(fn, 13));
    emit_bz(fn, cmp, lfail);

    /* subss: 13 - 10 = 3 */
    r = new_fbinop32(fn, IR_FSUB, r, f10);
    ti = new_f32toi(fn, r);
    cmp = new_cmpeq(fn, ti, i3);
    emit_bz(fn, cmp, lfail);

    /* mulss: 3 * 4 = 12 */
    r = new_fbinop32(fn, IR_FMUL, f3, f4);
    ti = new_f32toi(fn, r);
    cmp = new_cmpeq(fn, ti, new_lic(fn, 12));
    emit_bz(fn, cmp, lfail);

    /* divss: 12 / 4 = 3 */
    r = new_fbinop32(fn, IR_FDIV, r, f4);
    ti = new_f32toi(fn, r);
    cmp = new_cmpeq(fn, ti, i3);
    emit_bz(fn, cmp, lfail);

    /* fneg: -(3) = -3 */
    r = new_funop32(fn, IR_FNEG, f3);
    ti = new_f32toi(fn, r);
    cmp = new_cmpeq(fn, ti, new_lic(fn, -3));
    emit_bz(fn, cmp, lfail);

    /* fabs: |-3| = 3 */
    r = new_funop32(fn, IR_FABS, r);
    ti = new_f32toi(fn, r);
    cmp = new_cmpeq(fn, ti, i3);
    emit_bz(fn, cmp, lfail);

    /* ucomiss: 3 < 10 true, 10 < 3 false, 3 == 3 true */
    cmp = new_fbinop32(fn, IR_FCMPLT, f3, f10);
    emit_bz(fn, cmp, lfail);
    cmp = new_fbinop32(fn, IR_FCMPLT, f10, f3);
    emit_bnz(fn, cmp, lfail);
    cmp = new_fbinop32(fn, IR_FCMPEQ, f3, f3);
    emit_bz(fn, cmp, lfail);

    /* F32 -> F64 -> F32 round-trip of 10.0 stays 10 */
    {
        int d = new_conv(fn, IR_F32TOF64, f10);   /* double 10.0 */
        int s = new_conv(fn, IR_F64TOF32, d);     /* single 10.0 */
        ti = new_f32toi(fn, s);
        cmp = new_cmpeq(fn, ti, i10);
        emit_bz(fn, cmp, lfail);
    }

    /* FSTL / FLDL: store F32 to a slot, load it back */
    {
        struct ir_insn *st = ir_emit(fn, IR_FSTL);
        st->a = f4;
        st->slot = 0;
        st->imm = FWIDTH_F32;

        fslot = ir_new_temp(fn);
        {
            struct ir_insn *ld = ir_emit(fn, IR_FLDL);
            ld->dst = fslot;
            ld->slot = 0;
            ld->imm = FWIDTH_F32;
        }
        ti = new_f32toi(fn, fslot);
        cmp = new_cmpeq(fn, ti, i4);
        emit_bz(fn, cmp, lfail);
    }

    emit_retv(fn, new_lic(fn, 0));
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

    util_set_progname("test_f32");
    for (k = 1; k < argc; k++) {
        if (strcmp(argv[k], "-o") == 0 && k + 1 < argc)
            outpath = argv[++k];
        else
            die("usage: test_f32 [-o output.s]");
    }

    arena_init(&a);
    fn = build_test(&a);
    regalloc(fn);
    prog.funcs = fn;
    prog.globals = NULL;

    out = outpath ? fopen(outpath, "w") : stdout;
    if (!out)
        die("cannot open %s", outpath);
    target_emit(out, &prog);
    if (outpath)
        fclose(out);

    arena_free(&a);
    return 0;
}
