/* test_i64.c : IR-level integration test for 64-bit integer codegen */
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
new_lic64(struct ir_func *fn, int64_t val)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_LIC64);

    i->dst = t;
    i->imm = (long)val;
    return t;
}

static int
new_binop64(struct ir_func *fn, int op, int a, int b)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, op);

    i->dst = t;
    i->a = a;
    i->b = b;
    return t;
}

static int
new_unop64(struct ir_func *fn, int op, int a)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, op);

    i->dst = t;
    i->a = a;
    return t;
}

static int
new_cmp64(struct ir_func *fn, int op, int a, int b)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, op);

    i->dst = t;
    i->a = a;
    i->b = b;
    return t;
}

static int
new_trunc64(struct ir_func *fn, int a)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_TRUNC64);

    i->dst = t;
    i->a = a;
    return t;
}

static int
new_sext64(struct ir_func *fn, int a)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_SEXT64);

    i->dst = t;
    i->a = a;
    return t;
}

static int
new_zext64(struct ir_func *fn, int a)
{
    int t = ir_new_temp(fn);
    struct ir_insn *i = ir_emit(fn, IR_ZEXT64);

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

static void
emit_retv64(struct ir_func *fn, int val)
{
    struct ir_insn *i = ir_emit(fn, IR_RETV64);

    i->a = val;
}

static struct ir_func *
build_add_one64(struct arena *a)
{
    struct ir_func *fn;
    int param, one, result;

    fn = ir_new_func(a, "add_one64");
    fn->nparams = 1;
    fn->nslots = 1;
    fn->slot_size = xmalloc(sizeof(int));
    fn->slot_size[0] = 8;

    ir_emit(fn, IR_FUNC);

    param = ir_new_temp(fn);
    {
        struct ir_insn *ld = ir_emit(fn, IR_LDL64);
        ld->dst = param;
        ld->slot = 0;
    }
    one = new_lic64(fn, 1);
    result = new_binop64(fn, IR_ADD64, param, one);
    emit_retv64(fn, result);

    ir_emit(fn, IR_ENDF);
    return fn;
}

static struct ir_func *
build_test(struct arena *a)
{
    struct ir_func *fn;
    int lfail;
    int a64, b64, r64, cmp;
    int i32;

    fn = ir_new_func(a, "main");
    fn->nparams = 0;
    fn->nslots = 1;
    fn->slot_size = xmalloc(sizeof(int));
    fn->slot_size[0] = 8;

    lfail = ir_new_label(fn);

    ir_emit(fn, IR_FUNC);

    /* --- ADD64: 0x100000002 + 0x200000003 = 0x300000005 --- */
    a64 = new_lic64(fn, 0x100000002LL);
    b64 = new_lic64(fn, 0x200000003LL);
    r64 = new_binop64(fn, IR_ADD64, a64, b64);
    /* check lo word = 5 */
    i32 = new_trunc64(fn, r64);
    cmp = new_cmpeq(fn, i32, new_lic(fn, 5));
    emit_bz(fn, cmp, lfail);
    /* check hi word via shift: (r >> 32) should be 3, use compare instead */
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0x300000005LL));
    emit_bz(fn, cmp, lfail);

    /* --- ADD64 with carry: 0xFFFFFFFF + 1 = 0x100000000 --- */
    a64 = new_lic64(fn, 0xFFFFFFFFLL);
    b64 = new_lic64(fn, 1);
    r64 = new_binop64(fn, IR_ADD64, a64, b64);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0x100000000LL));
    emit_bz(fn, cmp, lfail);

    /* --- SUB64: 0x300000005 - 0x100000002 = 0x200000003 --- */
    a64 = new_lic64(fn, 0x300000005LL);
    b64 = new_lic64(fn, 0x100000002LL);
    r64 = new_binop64(fn, IR_SUB64, a64, b64);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0x200000003LL));
    emit_bz(fn, cmp, lfail);

    /* --- SUB64 with borrow: 0x100000000 - 1 = 0xFFFFFFFF --- */
    a64 = new_lic64(fn, 0x100000000LL);
    b64 = new_lic64(fn, 1);
    r64 = new_binop64(fn, IR_SUB64, a64, b64);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0xFFFFFFFFLL));
    emit_bz(fn, cmp, lfail);

    /* --- NEG64: -(1) = -1 = 0xFFFFFFFFFFFFFFFF --- */
    a64 = new_lic64(fn, 1);
    r64 = new_unop64(fn, IR_NEG64, a64);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, -1LL));
    emit_bz(fn, cmp, lfail);

    /* --- AND64 --- */
    a64 = new_lic64(fn, 0xFF00FF00FF00FF00LL);
    b64 = new_lic64(fn, 0x0F0F0F0F0F0F0F0FLL);
    r64 = new_binop64(fn, IR_AND64, a64, b64);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0x0F000F000F000F00LL));
    emit_bz(fn, cmp, lfail);

    /* --- OR64 --- */
    a64 = new_lic64(fn, 0xF000000000000000LL);
    b64 = new_lic64(fn, 0x000000000000000FLL);
    r64 = new_binop64(fn, IR_OR64, a64, b64);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, (int64_t)0xF00000000000000FLL));
    emit_bz(fn, cmp, lfail);

    /* --- XOR64 --- */
    a64 = new_lic64(fn, 0xAAAAAAAA55555555LL);
    b64 = new_lic64(fn, 0x5555555555555555LL);
    r64 = new_binop64(fn, IR_XOR64, a64, b64);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, (int64_t)0xFFFFFFFF00000000LL));
    emit_bz(fn, cmp, lfail);

    /* --- SEXT64: sign-extend -1 (0xFFFFFFFF) -> 0xFFFFFFFFFFFFFFFF --- */
    i32 = new_lic(fn, -1);
    r64 = new_sext64(fn, i32);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, -1LL));
    emit_bz(fn, cmp, lfail);

    /* --- ZEXT64: zero-extend 0xFFFFFFFF -> 0x00000000FFFFFFFF --- */
    i32 = new_lic(fn, -1);
    r64 = new_zext64(fn, i32);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0xFFFFFFFFLL));
    emit_bz(fn, cmp, lfail);

    /* --- CMP64LTS: -1 < 0 should be true --- */
    a64 = new_lic64(fn, -1LL);
    b64 = new_lic64(fn, 0);
    cmp = new_cmp64(fn, IR_CMP64LTS, a64, b64);
    emit_bz(fn, cmp, lfail);

    /* --- CMP64LTS: 0 < -1 should be false --- */
    cmp = new_cmp64(fn, IR_CMP64LTS, b64, a64);
    emit_bnz(fn, cmp, lfail);

    /* --- CMP64LTU: -1 (big unsigned) > 0, so NOT less-than --- */
    cmp = new_cmp64(fn, IR_CMP64LTU, a64, b64);
    emit_bnz(fn, cmp, lfail);

    /* --- CMP64GTS: equal hi, differ in lo (signed) --- */
    /* 0x00000001_00000002 > 0x00000001_00000001 */
    a64 = new_lic64(fn, 0x100000002LL);
    b64 = new_lic64(fn, 0x100000001LL);
    cmp = new_cmp64(fn, IR_CMP64GTS, a64, b64);
    emit_bz(fn, cmp, lfail);

    /* --- CMP64NE --- */
    cmp = new_cmp64(fn, IR_CMP64NE, a64, b64);
    emit_bz(fn, cmp, lfail);

    /* --- MUL64: 0x10000 * 0x10000 = 0x100000000 --- */
    a64 = new_lic64(fn, 0x10000LL);
    b64 = new_lic64(fn, 0x10000LL);
    r64 = new_binop64(fn, IR_MUL64, a64, b64);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0x100000000LL));
    emit_bz(fn, cmp, lfail);

    /* --- MUL64: 7 * 6 = 42 (small values) --- */
    a64 = new_lic64(fn, 7);
    b64 = new_lic64(fn, 6);
    r64 = new_binop64(fn, IR_MUL64, a64, b64);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 42));
    emit_bz(fn, cmp, lfail);

    /* --- SHL64: 1 << 32 = 0x100000000 --- */
    a64 = new_lic64(fn, 1);
    i32 = new_lic(fn, 32);
    r64 = new_binop64(fn, IR_SHL64, a64, i32);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0x100000000LL));
    emit_bz(fn, cmp, lfail);

    /* --- SHL64: 0xFF << 4 = 0xFF0 --- */
    a64 = new_lic64(fn, 0xFF);
    i32 = new_lic(fn, 4);
    r64 = new_binop64(fn, IR_SHL64, a64, i32);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0xFF0LL));
    emit_bz(fn, cmp, lfail);

    /* --- SHRS64: -8 >> 2 = -2 (arithmetic) --- */
    a64 = new_lic64(fn, -8LL);
    i32 = new_lic(fn, 2);
    r64 = new_binop64(fn, IR_SHRS64, a64, i32);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, -2LL));
    emit_bz(fn, cmp, lfail);

    /* --- SHRU64: 0x8000000000000000 >> 1 = 0x4000000000000000 --- */
    a64 = new_lic64(fn, (int64_t)0x8000000000000000LL);
    i32 = new_lic(fn, 1);
    r64 = new_binop64(fn, IR_SHRU64, a64, i32);
    cmp = new_cmp64(fn, IR_CMP64EQ, r64, new_lic64(fn, 0x4000000000000000LL));
    emit_bz(fn, cmp, lfail);

    /* --- CMP64LES: -1 <= 0 (true) --- */
    a64 = new_lic64(fn, -1LL);
    b64 = new_lic64(fn, 0);
    cmp = new_cmp64(fn, IR_CMP64LES, a64, b64);
    emit_bz(fn, cmp, lfail);

    /* --- CMP64LES: 5 <= 5 (true, equal) --- */
    a64 = new_lic64(fn, 5);
    b64 = new_lic64(fn, 5);
    cmp = new_cmp64(fn, IR_CMP64LES, a64, b64);
    emit_bz(fn, cmp, lfail);

    /* --- CMP64LES: 1 <= 0 (false) --- */
    a64 = new_lic64(fn, 1);
    b64 = new_lic64(fn, 0);
    cmp = new_cmp64(fn, IR_CMP64LES, a64, b64);
    emit_bnz(fn, cmp, lfail);

    /* --- CMP64GES: 0 >= -1 (true) --- */
    a64 = new_lic64(fn, 0);
    b64 = new_lic64(fn, -1LL);
    cmp = new_cmp64(fn, IR_CMP64GES, a64, b64);
    emit_bz(fn, cmp, lfail);

    /* --- CMP64GES: 5 >= 5 (true, equal) --- */
    a64 = new_lic64(fn, 5);
    cmp = new_cmp64(fn, IR_CMP64GES, a64, a64);
    emit_bz(fn, cmp, lfail);

    /* --- CMP64LEU: 0 <= 0xFFFFFFFFFFFFFFFF (true) --- */
    a64 = new_lic64(fn, 0);
    b64 = new_lic64(fn, -1LL);
    cmp = new_cmp64(fn, IR_CMP64LEU, a64, b64);
    emit_bz(fn, cmp, lfail);

    /* --- CMP64GEU: 0xFFFFFFFFFFFFFFFF >= 0 (true) --- */
    cmp = new_cmp64(fn, IR_CMP64GEU, b64, a64);
    emit_bz(fn, cmp, lfail);

    /* --- CMP64GTU: 0xFFFFFFFFFFFFFFFF > 0 (true) --- */
    cmp = new_cmp64(fn, IR_CMP64GTU, b64, a64);
    emit_bz(fn, cmp, lfail);

    /* --- CMP64GTU: 0 > 0xFFFFFFFFFFFFFFFF (false) --- */
    cmp = new_cmp64(fn, IR_CMP64GTU, a64, b64);
    emit_bnz(fn, cmp, lfail);

    /* --- STL64/LDL64: store to slot and load back --- */
    {
        struct ir_insn *st;
        int loaded;

        a64 = new_lic64(fn, 0xDEADBEEF12345678LL);
        st = ir_emit(fn, IR_STL64);
        st->a = a64;
        st->slot = 0;

        loaded = ir_new_temp(fn);
        {
            struct ir_insn *ld = ir_emit(fn, IR_LDL64);
            ld->dst = loaded;
            ld->slot = 0;
        }
        cmp = new_cmp64(fn, IR_CMP64EQ, loaded, new_lic64(fn, 0xDEADBEEF12345678LL));
        emit_bz(fn, cmp, lfail);
    }

    /* --- ST64/LD64: store to global and load back via pointer --- */
    {
        struct ir_insn *lea, *st;
        int ptr, loaded;

        a64 = new_lic64(fn, 0xCAFEBABE87654321LL);
        ptr = ir_new_temp(fn);
        lea = ir_emit(fn, IR_LEA);
        lea->dst = ptr;
        lea->sym = xstrdup("g_i64buf");

        st = ir_emit(fn, IR_ST64);
        st->a = ptr;
        st->b = a64;

        loaded = ir_new_temp(fn);
        {
            int ptr2 = ir_new_temp(fn);
            struct ir_insn *lea2 = ir_emit(fn, IR_LEA);
            lea2->dst = ptr2;
            lea2->sym = xstrdup("g_i64buf");

            struct ir_insn *ld = ir_emit(fn, IR_LD64);
            ld->dst = loaded;
            ld->a = ptr2;
        }
        cmp = new_cmp64(fn, IR_CMP64EQ, loaded, new_lic64(fn, 0xCAFEBABE87654321LL));
        emit_bz(fn, cmp, lfail);
    }

    /* --- ARG64 + CALL64: call add_one64(0x100000041) = 0x100000042 --- */
    {
        struct ir_insn *arg_insn, *call_insn;
        int result64;

        a64 = new_lic64(fn, 0x100000041LL);
        arg_insn = ir_emit(fn, IR_ARG64);
        arg_insn->a = a64;

        result64 = ir_new_temp(fn);
        call_insn = ir_emit(fn, IR_CALL64);
        call_insn->dst = result64;
        call_insn->sym = xstrdup("add_one64");
        call_insn->nargs = 1;

        cmp = new_cmp64(fn, IR_CMP64EQ, result64, new_lic64(fn, 0x100000042LL));
        emit_bz(fn, cmp, lfail);
    }

    /* --- ARG64 + CALLI64: indirect call to add_one64 --- */
    {
        struct ir_insn *lea, *arg_insn, *call_insn;
        int fptr, result64;

        a64 = new_lic64(fn, 0x200000099LL);
        arg_insn = ir_emit(fn, IR_ARG64);
        arg_insn->a = a64;

        fptr = ir_new_temp(fn);
        lea = ir_emit(fn, IR_LEA);
        lea->dst = fptr;
        lea->sym = xstrdup("add_one64");

        result64 = ir_new_temp(fn);
        call_insn = ir_emit(fn, IR_CALLI64);
        call_insn->dst = result64;
        call_insn->a = fptr;
        call_insn->nargs = 1;

        cmp = new_cmp64(fn, IR_CMP64EQ, result64, new_lic64(fn, 0x20000009aLL));
        emit_bz(fn, cmp, lfail);
    }

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
        die("usage: test_i64 -o <output.s>");

    arena_init(&a);
    fn = build_test(&a);
    regalloc(fn);

    struct ir_func *helper = build_add_one64(&a);
    regalloc(helper);
    fn->next = helper;

    struct ir_global *g = xmalloc(sizeof *g);
    memset(g, 0, sizeof *g);
    g->name = xstrdup("g_i64buf");
    g->base_type = IR_I32;
    g->arr_size = 2;
    g->next = NULL;

    prog.funcs = fn;
    prog.globals = g;

    out = fopen(outpath, "w");
    if (!out)
        die("cannot open %s", outpath);
    target_emit(out, &prog);
    fclose(out);

    arena_free(&a);
    return 0;
}
