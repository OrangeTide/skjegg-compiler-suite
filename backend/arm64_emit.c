/* arm64_emit.c : AArch64 (ARM64) back-end, emits GAS-syntax assembly */
/*
 * Stack-based calling convention (matches the other backends' pattern):
 *   - Args pushed right-to-left on the stack, one 8-byte slot each; caller
 *     pops.  The pushed region is kept 16-byte aligned (AArch64 requires a
 *     16-aligned sp).
 *   - Return value in x0/w0.
 *   - Callee-save: x19..x28, x29 (fp), x30 (lr), sp.
 *   - Scratch: x0..x17.  This backend reserves x9/x10 for spill reloads and
 *     x15 for out-of-range frame-address materialisation.
 *
 * ILP32-style addressing
 * ----------------------
 * AArch64 is a 64-bit machine, but the shared IR was designed for 32-bit
 * pointers (address arithmetic runs through 32-bit IR_ADD).  So this backend
 * treats every address as a 32-bit value living in the w-view of a register:
 * a w-write zeroes the upper 32 bits, and the x-view of the same register is
 * then the correct zero-extended (< 4GB) address to feed a load/store.  The
 * runtime (start_arm64.S) switches to a low-memory stack and heap so every
 * live address really does fit in 32 bits.  A `long long` (IR_I64) is the one
 * value that uses the full 64-bit x-view.
 *
 * Frame layout (after prologue, x29 = frame pointer, matching the RV layout):
 *
 *      x29 + 16 + 8*i   param i (pushed by caller before bl)
 *      x29 + 8          saved x30 (lr)
 *      x29 + 0          saved old x29
 *      x29 - locals     bottom of locals
 *      below locals     spill slots (8 bytes each)
 *      sp + 0..72       saved x19..x28 (10 regs)
 */

#include "ir.h"

#include <stdio.h>
#include <string.h>

#define NSAVED 10

/*
 * Register name tables.  Indices 0-1 are scratch (x9/x10), indices 2-11 are
 * the allocatable callee-save registers x19-x28, matching FIRST_REG=2,
 * NUM_REGS=10 in regalloc_arm64.c.
 */
static const char *xr[] = {
    "x9", "x10",
    "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28",
};
static const char *wr[] = {
    "w9", "w10",
    "w19", "w20", "w21", "w22", "w23",
    "w24", "w25", "w26", "w27", "w28",
};

/*
 * Float register table.  Indices 0-1 are scratch (d0/d1), indices 2-9 are the
 * allocatable callee-save registers d8-d15, matching FP_FIRST_REG=2,
 * FP_NUM_REGS=8 in regalloc_arm64.c.  (AArch64 preserves only the low 64 bits
 * of d8-d15 across a call, which is exactly a double.)
 */
static const char *dr[] = {
    "d0", "d1",
    "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15",
};
/* single-precision (s) view of the same v-registers, for IR_F32 ops */
static const char *sr[] = {
    "s0", "s1",
    "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
};

#ifdef CC_PSABI
/****************************************************************
 * AAPCS64 calling convention (cc, P1: fixed scalar/ptr/float args)
 *
 * Integer/pointer args -> x0..x7; float/double -> d0..d7; the rest on a
 * 16-byte-aligned stack.  Return in x0 / d0.  A register param gets an 8-byte
 * "home" slot just below x29 that the prologue spills it into, so the slot-
 * based lowering downstream is unchanged; an overflow param is read from the
 * incoming stack at x29 + 16 + 8*k.
 ****************************************************************/
static const char *aapcs_ix[] = { "x0", "x1", "x2", "x3",
                                  "x4", "x5", "x6", "x7" };
static const char *aapcs_iw[] = { "w0", "w1", "w2", "w3",
                                  "w4", "w5", "w6", "w7" };
static const char *aapcs_d[]  = { "d0", "d1", "d2", "d3",
                                  "d4", "d5", "d6", "d7" };
static const char *aapcs_s[]  = { "s0", "s1", "s2", "s3",
                                  "s4", "s5", "s6", "s7" };

/* where a parameter is passed.  A scalar is one slot; a register aggregate is
   1-2 x-slots (INTEGER) or 1-4 d-slots (a double HFA).  is_reg is set only
   when every slot fits (AAPCS64's all-or-nothing rule); otherwise the whole
   parameter is on the incoming stack. */
struct aapcs_ploc {
    int is_reg;
    int neb;            /* slot / register count (1-4) */
    int hwords;         /* 8-byte home slots the struct occupies */
    int stride;         /* byte stride between slots (8, or 4 for a float HFA) */
    int hidx;           /* first home-slot index (reg params) */
    int soff;           /* first stack-slot index (stack params) */
    int cls[4];         /* class per slot: 0 INTEGER (x), 1 double (d), 2 float (s) */
    int ridx[4];        /* register index within its class per slot */
};

static void
aapcs_param(struct ir_func *fn, int slot, struct aapcs_ploc *out)
{
    int ix = 0, dx = 0, nstack = 0, nreg = 0, i, j;

    for (i = 0; ; i++) {
        struct aapcs_ploc p;
        int neb = fn->param_neb ? fn->param_neb[i] : 1;
        int need_i = 0, need_f = 0;
        if (i == 0 && fn->ret_neb == -1) {
            /* the hidden indirect-result parameter rides x8, homed like any
               register param but not consuming a normal argument register */
            p.neb = 1;
            p.hwords = 1;
            p.stride = 8;
            p.is_reg = 1;
            p.hidx = nreg++;
            p.soff = 0;
            p.cls[0] = -3;              /* x8 marker */
            p.ridx[0] = 0;
            if (i == slot) { *out = p; return; }
            continue;
        }
        if (neb < 1) neb = 1;
        p.neb = neb;
        for (j = 0; j < neb; j++) {
            p.cls[j] = fn->param_cls ? fn->param_cls[4 * i + j] : 0;
            if (p.cls[j] == 1 || p.cls[j] == 2) need_f++; else need_i++;
        }
        /* a float HFA packs 4-byte singles; every other register aggregate is
           8-byte slots */
        p.stride = (p.cls[0] == 2) ? 4 : 8;
        p.hwords = (neb * p.stride + 7) / 8;
        if (ix + need_i <= 8 && dx + need_f <= 8) {
            p.is_reg = 1;
            p.hidx = nreg;
            p.soff = 0;
            for (j = 0; j < neb; j++)
                p.ridx[j] = (p.cls[j] == 1 || p.cls[j] == 2) ? dx++ : ix++;
            nreg += p.hwords;
        } else {
            p.is_reg = 0;
            p.hidx = 0;
            p.soff = nstack;
            nstack += p.hwords;
        }
        if (i == slot) {
            *out = p;
            return;
        }
    }
}

/* number of register home slots (8 bytes each) below x29 */
static int
aapcs_nreg_params(struct ir_func *fn)
{
    struct aapcs_ploc p;
    int i, n = 0;
    for (i = 0; i < fn->nparams; i++) {
        aapcs_param(fn, i, &p);
        if (p.is_reg)
            n += p.hwords;
    }
    return n;
}
#endif

/* bytes reserved just below x29 for register-param home slots (0 unless the
   AAPCS64 convention is in effect) */
static int
param_home(struct ir_func *fn)
{
#ifdef CC_PSABI
    return aapcs_nreg_params(fn) * 8;
#else
    (void)fn;
    return 0;
#endif
}

/* AAPCS64 register save area for a variadic function: 8 x-regs (64) + 8
   v-regs at a 16-byte stride (128) = 192 bytes, placed just below the param
   homes; va_start points into it */
static int
va_save(struct ir_func *fn)
{
#ifdef CC_PSABI
    return fn->is_variadic ? 192 : 0;
#else
    (void)fn;
    return 0;
#endif
}

static int
frame_reserve(struct ir_func *fn)
{
    return param_home(fn) + va_save(fn);
}

/****************************************************************
 * Frame layout helpers
 ****************************************************************/

static int
locals_size(struct ir_func *fn)
{
    int i, s;

    s = 0;
    for (i = fn->nparams; i < fn->nslots; i++) {
        int sz = (fn->slot_size[i] + 7) & ~7;
        s += sz;
    }
    return s;
}

static int
slot_offset(struct ir_func *fn, int slot)
{
    int i, off;

    if (slot < fn->nparams) {
#ifdef CC_PSABI
        /* AAPCS64: a register param lives in its home slot(s) below x29, an
           overflow param on the incoming stack at x29 + 16 + 8*k.  A struct's
           base is the lowest of its homes so slot j sits at base + 8*j. */
        struct aapcs_ploc p;
        aapcs_param(fn, slot, &p);
        if (p.is_reg)
            return -8 * (p.hidx + p.hwords);
        return 16 + 8 * p.soff;
#else
        return 16 + 8 * slot;
#endif
    }
    off = 0;
    for (i = fn->nparams; i <= slot; i++) {
        int sz = (fn->slot_size[i] + 7) & ~7;
        off += sz;
    }
    return -frame_reserve(fn) - off;
}

static int
frame_size(struct ir_func *fn)
{
    int f = frame_reserve(fn) + locals_size(fn) +
            fn->nspills * 8 + fn->nfspills * 8;
    return (f + 15) & ~15;   /* keep the frame 16-byte aligned */
}

static int
spill_byte_offset(struct ir_func *fn, int temp)
{
    return -frame_reserve(fn) - locals_size(fn) - (fn->temp_spill[temp] + 8);
}

/* float spill area sits just below the integer spill area */
static int
fspill_byte_offset(struct ir_func *fn, int temp)
{
    return -frame_reserve(fn) - locals_size(fn) - fn->nspills * 8
           - (fn->temp_spill[temp] + 8);
}

/* AAPCS64: d8-d15 are callee-saved (dr[] indices 2..9).  A function that uses
   any of them as an allocatable temp must preserve them, or it clobbers a
   caller's live values.  fsave_mask is the set actually used; fsave_bytes is
   its footprint.  Gated to the psABI cc build (the stack-convention tools do
   not interoperate with foreign code, so their frame layout is left alone). */
static int
fsave_mask(struct ir_func *fn)
{
#ifdef CC_PSABI
    struct ir_insn *i;
    int mask = 0;
    for (i = fn->head; i; i = i->next)
        if (i->dst >= 0 && ir_op_is_float_def(i->op)) {
            int r = fn->temp_reg[i->dst];
            if (r >= 2 && r <= 9)
                mask |= 1 << r;
        }
    return mask;
#else
    (void)fn;
    return 0;
#endif
}

/* footprint of the saved d-regs, rounded up to 16 so the frame stays aligned
   (16 + NSAVED*8 = 96 and frame_size are already multiples of 16) */
static int
fsave_bytes(struct ir_func *fn)
{
    int m = fsave_mask(fn), n = 0;
    while (m) { n += m & 1; m >>= 1; }
    return (n * 8 + 15) & ~15;
}

/****************************************************************
 * Immediate / memory emission helpers
 *
 * AArch64 has tight immediate ranges: load/store offsets are a scaled 12-bit
 * unsigned field or an unscaled signed 9-bit field, and add/sub immediates
 * are 12 bits.  These helpers pick a valid encoding and fall back to
 * materialising the value through x15 when nothing else fits.
 ****************************************************************/

/* dst = src + imm, for any signed imm.  dst/src are x-view register names. */
static void
addimm(FILE *out, const char *dst, const char *src, long imm)
{
    const char *op;
    long a;

    if (imm == 0) {
        if (strcmp(dst, src) != 0)
            fprintf(out, "\tmov %s, %s\n", dst, src);
        return;
    }
    op = imm > 0 ? "add" : "sub";
    a = imm > 0 ? imm : -imm;
    if (a <= 4095)
        fprintf(out, "\t%s %s, %s, #%ld\n", op, dst, src, a);
    else if ((a & 0xfff) == 0 && (a >> 12) <= 4095)
        fprintf(out, "\t%s %s, %s, #%ld, lsl #12\n", op, dst, src, a >> 12);
    else {
        fprintf(out, "\tldr x15, =%ld\n", imm);
        fprintf(out, "\tadd %s, %s, x15\n", dst, src);
    }
}

/* <ldr/str> reg, [base, #off] for a possibly out-of-range signed off.
 * store: 1 = str, 0 = ldr.  wide: 1 = 64-bit (x), 0 = 32-bit (w). */
static void
memop(FILE *out, int store, const char *reg, const char *base, int off)
{
    if (off >= -256 && off <= 255) {
        fprintf(out, "\t%s %s, [%s, #%d]\n",
            store ? "stur" : "ldur", reg, base, off);
    } else if (off >= 0 && off <= 16380 && (off & 7) == 0) {
        fprintf(out, "\t%s %s, [%s, #%d]\n",
            store ? "str" : "ldr", reg, base, off);
    } else {
        addimm(out, "x15", base, off);
        fprintf(out, "\t%s %s, [x15]\n", store ? "str" : "ldr", reg);
    }
}

/****************************************************************
 * Temp -> register materialisation
 *
 * rs/rd/wd operate on the 32-bit (w) view for the integer core.
 * The *x variants operate on the 64-bit (x) view for IR_I64 opcodes.
 * ra returns the x-view of an address-holding temp (a 32-bit value whose
 * high bits are zero) for use as a load/store base.
 ****************************************************************/

static const char *
rs(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return wr[r];
    memop(out, 0, wr[scratch], "x29", spill_byte_offset(fn, t));
    return wr[scratch];
}

static const char *
rsx(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return xr[r];
    memop(out, 1, xr[scratch], "x29", spill_byte_offset(fn, t));
    return xr[scratch];
}

static const char *
ra(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return xr[r];
    memop(out, 0, wr[scratch], "x29", spill_byte_offset(fn, t));
    return xr[scratch];
}

static const char *
rd(struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];
    return r >= 0 ? wr[r] : wr[scratch];
}

static const char *
rdx(struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];
    return r >= 0 ? xr[r] : xr[scratch];
}

/* store a 32-bit result to its spill slot (if spilled) */
static void
wd32(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    memop(out, 1, reg, "x29", spill_byte_offset(fn, t));
}

/* store a 64-bit result to its spill slot (if spilled) */
static void
wd64(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    memop(out, 1, reg, "x29", spill_byte_offset(fn, t));
}

/* float temp reload / dest / store. f32 picks the single (s) register view;
 * memop formats the operand like a GPR one, so the fp spill area is reused
 * (F32 spills into the low 4 bytes of its 8-byte slot). */
static const char *
frs_w(FILE *out, struct ir_func *fn, int t, int scratch, int f32)
{
    const char **tbl = f32 ? sr : dr;
    int r = fn->temp_reg[t];

    if (r >= 0)
        return tbl[r];
    memop(out, 0, tbl[scratch], "x29", fspill_byte_offset(fn, t));
    return tbl[scratch];
}

static const char *
frd_w(struct ir_func *fn, int t, int scratch, int f32)
{
    const char **tbl = f32 ? sr : dr;
    int r = fn->temp_reg[t];
    return r >= 0 ? tbl[r] : tbl[scratch];
}

/* double-only convenience wrappers (the common case) */
#define frs(o, f, t, s) frs_w((o), (f), (t), (s), 0)
#define frd(f, t, s)    frd_w((f), (t), (s), 0)

static void
fwd(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    memop(out, 1, reg, "x29", fspill_byte_offset(fn, t));
}

/****************************************************************
 * Binary / unary / compare helpers
 ****************************************************************/

static void
emit_binop(FILE *out, struct ir_func *fn, struct ir_insn *i,
           const char *mnem)
{
    const char *sa, *sb, *sd;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    sd = rd(fn, i->dst, 0);
    fprintf(out, "\t%s %s, %s, %s\n", mnem, sd, sa, sb);
    wd32(out, fn, i->dst, sd);
}

static void
emit_binop64(FILE *out, struct ir_func *fn, struct ir_insn *i,
             const char *mnem)
{
    const char *sa, *sb, *sd;

    sa = rsx(out, fn, i->a, 0);
    sb = rsx(out, fn, i->b, 1);
    sd = rdx(fn, i->dst, 0);
    fprintf(out, "\t%s %s, %s, %s\n", mnem, sd, sa, sb);
    wd64(out, fn, i->dst, sd);
}

static void
emit_unop(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *mnem)
{
    const char *sa, *sd;

    sa = rs(out, fn, i->a, 0);
    sd = rd(fn, i->dst, 0);
    fprintf(out, "\t%s %s, %s\n", mnem, sd, sa);
    wd32(out, fn, i->dst, sd);
}

static void
emit_fbinop(FILE *out, struct ir_func *fn, struct ir_insn *i,
            const char *mnem)
{
    int f32 = i->imm == FWIDTH_F32;
    const char *sa, *sb, *sd;

    sa = frs_w(out, fn, i->a, 0, f32);
    sb = frs_w(out, fn, i->b, 1, f32);
    sd = frd_w(fn, i->dst, 0, f32);
    fprintf(out, "\t%s %s, %s, %s\n", mnem, sd, sa, sb);
    fwd(out, fn, i->dst, sd);
}

static void
emit_funop(FILE *out, struct ir_func *fn, struct ir_insn *i,
           const char *mnem)
{
    int f32 = i->imm == FWIDTH_F32;
    const char *sa, *sd;

    sa = frs_w(out, fn, i->a, 0, f32);
    sd = frd_w(fn, i->dst, 0, f32);
    fprintf(out, "\t%s %s, %s\n", mnem, sd, sa);
    fwd(out, fn, i->dst, sd);
}

static const char *
cmp_cond(int op)
{
    switch (op) {
    case IR_CMPEQ:  return "eq";
    case IR_CMPNE:  return "ne";
    case IR_CMPLTS: return "lt";
    case IR_CMPLES: return "le";
    case IR_CMPGTS: return "gt";
    case IR_CMPGES: return "ge";
    case IR_CMPLTU: return "lo";
    case IR_CMPLEU: return "ls";
    case IR_CMPGTU: return "hi";
    case IR_CMPGEU: return "hs";
    default:        return "eq";
    }
}

static const char *
cmp64_cond(int op)
{
    switch (op) {
    case IR_CMP64EQ:  return "eq";
    case IR_CMP64NE:  return "ne";
    case IR_CMP64LTS: return "lt";
    case IR_CMP64LES: return "le";
    case IR_CMP64GTS: return "gt";
    case IR_CMP64GES: return "ge";
    case IR_CMP64LTU: return "lo";
    case IR_CMP64LEU: return "ls";
    case IR_CMP64GTU: return "hi";
    case IR_CMP64GEU: return "hs";
    default:          return "eq";
    }
}

/****************************************************************
 * Per-instruction emission
 ****************************************************************/

static int arg_temps[16];
static int arg_is_i64[16];
static int arg_is_float[16];
static int arg_f32[16];         /* a float arg is single (s-reg) vs double (d) */
static int arg_mem[16];         /* >0: a by-value struct arg of this byte size */
static int narg;
#ifdef CC_PSABI
static int x8_arg = -1;         /* AAPCS64 indirect-result pointer temp, or -1 */
#endif
static int label_prefix;

/* return kind for emit_call_flush */
enum { RET_I32, RET_I64, RET_FLOAT };

static void
emit_load(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *mnem)
{
    const char *sa, *sd;

    sa = ra(out, fn, i->a, 0);
    sd = rd(fn, i->dst, 0);
    fprintf(out, "\t%s %s, [%s]\n", mnem, sd, sa);
    wd32(out, fn, i->dst, sd);
}

static void
emit_store(FILE *out, struct ir_func *fn, struct ir_insn *i,
           const char *mnem)
{
    const char *sa, *sb;

    sa = ra(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    fprintf(out, "\t%s %s, [%s]\n", mnem, sb, sa);
}

static void
emit_prologue(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int fs = fsave_bytes(fn);           /* 16-aligned */
    int fmask = fsave_mask(fn);
    int total = 16 + frame + NSAVED * 8 + fs;
    int k, m;

    /* Layout from sp up: saved d-regs (fs), saved x19-x28 (NSAVED*8), frame,
       old x29, lr.  Keeping the d-saves at the bottom leaves the x-reg x29-
       relative offsets (and the epilogue) unchanged. */
    addimm(out, "sp", "sp", -total);
    memop(out, 1, "x30", "sp", fs + NSAVED * 8 + frame + 8);
    memop(out, 1, "x29", "sp", fs + NSAVED * 8 + frame);
    addimm(out, "x29", "sp", fs + NSAVED * 8 + frame);
    for (k = 0; k < NSAVED; k++)
        memop(out, 1, xr[k + 2], "sp", fs + k * 8);
    for (k = 2, m = 0; k <= 9; k++)
        if (fmask & (1 << k))
            memop(out, 1, dr[k], "sp", m++ * 8);
#ifdef CC_PSABI
    /* AAPCS64: spill register params into their home slots below x29.  A
       struct parameter stores each slot j at base + 8*j (base = lowest home),
       so its fields read back contiguously. */
    for (k = 0; k < fn->nparams; k++) {
        struct aapcs_ploc p;
        int j;
        aapcs_param(fn, k, &p);
        if (!p.is_reg)
            continue;
        if (p.cls[0] == -3) {           /* the x8 indirect-result pointer */
            memop(out, 1, "x8", "x29", -8 * (p.hidx + 1));
            continue;
        }
        for (j = 0; j < p.neb; j++) {
            int off = -8 * (p.hidx + p.hwords) + p.stride * j;
            if (p.cls[j] == 2)          /* float HFA slot -> s-register */
                memop(out, 1, aapcs_s[p.ridx[j]], "x29", off);
            else if (p.cls[j] == 1)     /* double slot -> d-register */
                memop(out, 1, aapcs_d[p.ridx[j]], "x29", off);
            else                        /* integer slot -> x-register */
                memop(out, 1, aapcs_ix[p.ridx[j]], "x29", off);
        }
    }
    /* variadic: save all arg registers to the register save area (base at
       x29 - frame_reserve) so va_arg can walk them.  The v-regs use a 16-byte
       stride (AAPCS64); cc reads the low 8 bytes (a double) of each slot. */
    if (fn->is_variadic) {
        int base = -frame_reserve(fn);
        for (k = 0; k < 8; k++)
            memop(out, 1, aapcs_ix[k], "x29", base + 8 * k);
        for (k = 0; k < 8; k++)
            memop(out, 1, aapcs_d[k], "x29", base + 64 + 16 * k);
    }
#endif
}

static void
emit_epilogue_no_ret(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int fs = fsave_bytes(fn);
    int fmask = fsave_mask(fn);
    int saved_base = -(NSAVED * 8 + frame);
    int k, m;

    for (k = 0; k < NSAVED; k++)
        memop(out, 0, xr[k + 2], "x29", saved_base + k * 8);
    /* restore the callee-saved d-regs (at the bottom of the frame) */
    for (k = 2, m = 0; k <= 9; k++)
        if (fmask & (1 << k))
            memop(out, 0, dr[k], "x29", -(fs + NSAVED * 8 + frame) + m++ * 8);
    memop(out, 0, "x30", "x29", 8);
    memop(out, 0, "x9", "x29", 0);
    addimm(out, "sp", "x29", 16);
    fprintf(out, "\tmov x29, x9\n");
}

static void
emit_epilogue(FILE *out, struct ir_func *fn)
{
    emit_epilogue_no_ret(out, fn);
    fprintf(out, "\tret\n");
}

static void
emit_call_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                int indirect, int retkind)
{
    int k, off;

#ifdef CC_PSABI
    {
        /* AAPCS64: place args in x0..x7 / d0..d7, overflow on a 16-aligned
           stack.  Register targets are disjoint from the allocatable regs
           (x19-x28 / d8-d15), so each source moves straight into its target
           with no parallel-move hazard. */
        int treg[16], onstk[16], ssize[16], iu = 0, du = 0, stack_bytes = 0;
        for (k = 0; k < narg; k++) {
            onstk[k] = 0;
            ssize[k] = 0;
            if (arg_mem[k] > 0) {           /* by-value struct: always on stack */
                onstk[k] = 1;
                ssize[k] = (arg_mem[k] + 7) & ~7;
            } else if (arg_is_float[k]) {
                if (du < 8) treg[k] = du++; else { onstk[k] = 1; ssize[k] = 8; }
            } else {
                if (iu < 8) treg[k] = iu++; else { onstk[k] = 1; ssize[k] = 8; }
            }
            stack_bytes += ssize[k];
        }
        int stkb = (stack_bytes + 15) & ~15;
        if (stkb > 0)
            addimm(out, "sp", "sp", -stkb);
        off = 0;
        for (k = 0; k < narg; k++) {
            if (!onstk[k])
                continue;
            if (arg_mem[k] > 0) {
                /* copy the struct's bytes into a stack block (a register
                   struct that did not fit; sizes are a multiple of 4) */
                int t = arg_temps[k], r = fn->temp_reg[t];
                int o = 0, rem = arg_mem[k];
                const char *src;
                if (r >= 0) {
                    src = xr[r];
                } else {
                    memop(out, 0, "x9", "x29", spill_byte_offset(fn, t));
                    src = "x9";
                }
                while (rem >= 8) {
                    memop(out, 0, "x10", src, o);
                    memop(out, 1, "x10", "sp", off + o);
                    o += 8; rem -= 8;
                }
                if (rem >= 4) {
                    memop(out, 0, "w10", src, o);
                    memop(out, 1, "w10", "sp", off + o);
                }
            } else if (arg_is_float[k])
                memop(out, 1, frs_w(out, fn, arg_temps[k], 0, arg_f32[k]),
                    "sp", off);
            else if (arg_is_i64[k])
                memop(out, 1, rsx(out, fn, arg_temps[k], 0), "sp", off);
            else
                memop(out, 1, rs(out, fn, arg_temps[k], 0), "sp", off);
            off += ssize[k];
        }
        for (k = 0; k < narg; k++) {
            int t = arg_temps[k], r;
            if (onstk[k])
                continue;
            r = fn->temp_reg[t];
            if (arg_is_float[k]) {
                const char *tgt = arg_f32[k] ? aapcs_s[treg[k]]
                                             : aapcs_d[treg[k]];
                if (r >= 0)
                    fprintf(out, "\tfmov %s, %s\n", tgt,
                        arg_f32[k] ? sr[r] : dr[r]);
                else
                    memop(out, 0, tgt, "x29", fspill_byte_offset(fn, t));
            } else if (arg_is_i64[k]) {
                if (r >= 0)
                    fprintf(out, "\tmov %s, %s\n", aapcs_ix[treg[k]], xr[r]);
                else
                    memop(out, 0, aapcs_ix[treg[k]], "x29",
                        spill_byte_offset(fn, t));
            } else {
                if (r >= 0)
                    fprintf(out, "\tmov %s, %s\n", aapcs_iw[treg[k]], wr[r]);
                else
                    memop(out, 0, aapcs_iw[treg[k]], "x29",
                        spill_byte_offset(fn, t));
            }
        }
        if (x8_arg >= 0) {
            /* the indirect-result pointer goes in x8 (not an arg register) */
            int r = fn->temp_reg[x8_arg];
            if (r >= 0)
                fprintf(out, "\tmov x8, %s\n", xr[r]);
            else
                memop(out, 0, "x8", "x29", spill_byte_offset(fn, x8_arg));
            x8_arg = -1;
        }
        if (indirect) {
            /* materialise the target into x16 (a scratch, not an arg reg)
               after placing the args */
            const char *sa = ra(out, fn, i->a, 0);
            fprintf(out, "\tmov x16, %s\n", sa);
            fprintf(out, "\tblr x16\n");
        } else {
            fprintf(out, "\tbl %s\n", i->sym);
        }
        if (stkb > 0)
            addimm(out, "sp", "sp", stkb);
        narg = 0;
    }
#else
    int push_bytes = (narg * 8 + 15) & ~15;
    if (push_bytes > 0)
        addimm(out, "sp", "sp", -push_bytes);
    off = 0;
    for (k = 0; k < narg; k++) {
        if (arg_is_float[k]) {
            const char *sa = frs(out, fn, arg_temps[k], 0);
            memop(out, 1, sa, "sp", off);
        } else if (arg_is_i64[k]) {
            const char *sa = rsx(out, fn, arg_temps[k], 0);
            memop(out, 1, sa, "sp", off);
        } else {
            const char *sa = rs(out, fn, arg_temps[k], 0);
            memop(out, 1, sa, "sp", off);
        }
        off += 8;
    }
    if (indirect) {
        const char *sa = ra(out, fn, i->a, 0);
        fprintf(out, "\tblr %s\n", sa);
    } else {
        fprintf(out, "\tbl %s\n", i->sym);
    }
    if (push_bytes > 0)
        addimm(out, "sp", "sp", push_bytes);
    narg = 0;
#endif

    if (i->dst >= 0) {
        if (retkind == RET_FLOAT) {
            const char *sd = frd(fn, i->dst, 0);
            if (strcmp(sd, "d0") != 0)
                fprintf(out, "\tfmov %s, d0\n", sd);
            fwd(out, fn, i->dst, sd);
        } else if (retkind == RET_I64) {
            const char *sd = rdx(fn, i->dst, 0);
            if (strcmp(sd, "x0") != 0)
                fprintf(out, "\tmov %s, x0\n", sd);
            wd64(out, fn, i->dst, sd);
        } else {
            const char *sd = rd(fn, i->dst, 0);
            if (strcmp(sd, "w0") != 0)
                fprintf(out, "\tmov %s, w0\n", sd);
            wd32(out, fn, i->dst, sd);
        }
    }
}

static void
emit_tailcall_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                    int indirect)
{
    int k;

    for (k = 0; k < narg; k++) {
        const char *sa = rs(out, fn, arg_temps[k], 0);
        memop(out, 1, sa, "x29", 16 + 8 * k);
    }
    if (indirect) {
        const char *sa = ra(out, fn, i->a, 0);
        fprintf(out, "\tmov x11, %s\n", sa);
    }
    narg = 0;
    emit_epilogue_no_ret(out, fn);
    if (indirect)
        fprintf(out, "\tbr x11\n");
    else
        fprintf(out, "\tb %s\n", i->sym);
}

static void
emit_insn(FILE *out, struct ir_func *fn, struct ir_insn *i)
{
    switch (i->op) {
    case IR_NOP:
        break;

    case IR_LIC: {
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tldr %s, =%ld\n", sd, i->imm);
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_LEA: {
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tldr %s, =%s\n", sd, i->sym);
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_ADL: {
        int r = fn->temp_reg[i->dst];
        const char *dx = r >= 0 ? xr[r] : xr[0];
        addimm(out, dx, "x29", slot_offset(fn, i->slot));
        if (r < 0)
            memop(out, 1, wr[0], "x29", spill_byte_offset(fn, i->dst));
        break;
    }

    case IR_MOV: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_ADD:  emit_binop(out, fn, i, "add");  break;
    case IR_SUB:  emit_binop(out, fn, i, "sub");  break;
    case IR_MUL:  emit_binop(out, fn, i, "mul");  break;
    case IR_AND:  emit_binop(out, fn, i, "and");  break;
    case IR_OR:   emit_binop(out, fn, i, "orr");  break;
    case IR_XOR:  emit_binop(out, fn, i, "eor");  break;
    case IR_SHL:  emit_binop(out, fn, i, "lsl");  break;
    case IR_SHRS: emit_binop(out, fn, i, "asr");  break;
    case IR_SHRU: emit_binop(out, fn, i, "lsr");  break;
    case IR_DIVS: emit_binop(out, fn, i, "sdiv"); break;
    case IR_DIVU: emit_binop(out, fn, i, "udiv"); break;

    case IR_MODS: case IR_MODU: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\t%s w15, %s, %s\n",
            i->op == IR_MODS ? "sdiv" : "udiv", sa, sb);
        fprintf(out, "\tmsub %s, w15, %s, %s\n", sd, sb, sa);
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_NEG: emit_unop(out, fn, i, "neg"); break;
    case IR_NOT: emit_unop(out, fn, i, "mvn"); break;

    case IR_LB:  emit_load(out, fn, i, "ldrb");  break;
    case IR_LBS: emit_load(out, fn, i, "ldrsb"); break;
    case IR_LH:  emit_load(out, fn, i, "ldrh");  break;
    case IR_LHS: emit_load(out, fn, i, "ldrsh"); break;
    case IR_LW:  emit_load(out, fn, i, "ldr");   break;

    case IR_SB: emit_store(out, fn, i, "strb"); break;
    case IR_SH: emit_store(out, fn, i, "strh"); break;
    case IR_SW: emit_store(out, fn, i, "str");  break;

    case IR_LDL: {
        const char *sd = rd(fn, i->dst, 0);
        memop(out, 0, sd, "x29", slot_offset(fn, i->slot));
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_STL: {
        const char *sa = rs(out, fn, i->a, 0);
        memop(out, 1, sa, "x29", slot_offset(fn, i->slot));
        break;
    }

    case IR_ALLOCA: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rdx(fn, i->dst, 0);
        fprintf(out, "\tadd w9, %s, #15\n", sa);
        fprintf(out, "\tbic w9, w9, #15\n");        /* round up to 16 */
        fprintf(out, "\tsub sp, sp, x9\n");
        fprintf(out, "\tmov %s, sp\n", sd);
        wd64(out, fn, i->dst, sd);
        break;
    }

    case IR_CMPEQ:
    case IR_CMPNE:
    case IR_CMPLTS: case IR_CMPLES: case IR_CMPGTS: case IR_CMPGES:
    case IR_CMPLTU: case IR_CMPLEU: case IR_CMPGTU: case IR_CMPGEU: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp %s, %s\n", sa, sb);
        fprintf(out, "\tcset %s, %s\n", sd, cmp_cond(i->op));
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_JMP:
        fprintf(out, "\tb .L%d_%d\n", label_prefix, i->label);
        break;
    case IR_BZ: {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tcbz %s, .L%d_%d\n",
            sa, label_prefix, i->label);
        break;
    }
    case IR_BNZ: {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tcbnz %s, .L%d_%d\n",
            sa, label_prefix, i->label);
        break;
    }
    case IR_LABEL:
        fprintf(out, ".L%d_%d:\n", label_prefix, i->label);
        break;

    case IR_ARG:
        if (narg >= 16)
            die("arm64_emit: too many args");
        arg_is_i64[narg] = 0;
        arg_is_float[narg] = 0;
        arg_mem[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_ARG64:
        if (narg >= 16)
            die("arm64_emit: too many args");
        arg_is_i64[narg] = 1;
        arg_is_float[narg] = 0;
        arg_mem[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
#ifdef CC_PSABI
    case IR_ARG_MEM:
        if (narg >= 16)
            die("arm64_emit: too many args");
        arg_is_i64[narg] = 0;
        arg_is_float[narg] = 0;
        arg_mem[narg] = i->imm;         /* by-value struct byte size */
        arg_temps[narg++] = i->a;       /* the struct's address */
        break;
    case IR_ARG_X8:
        x8_arg = i->a;          /* placed into x8 by the next call flush */
        break;
#endif
    case IR_CALL:
        emit_call_flush(out, fn, i, 0, RET_I32);
        break;
    case IR_CALLI:
        emit_call_flush(out, fn, i, 1, RET_I32);
        break;
    case IR_TAILCALL:
        emit_tailcall_flush(out, fn, i, 0);
        break;
    case IR_TAILCALLI:
        emit_tailcall_flush(out, fn, i, 1);
        break;

    case IR_RET:
        emit_epilogue(out, fn);
        break;
    case IR_RETV: {
        const char *sa = rs(out, fn, i->a, 0);
        if (strcmp(sa, "w0") != 0)
            fprintf(out, "\tmov w0, %s\n", sa);
        emit_epilogue(out, fn);
        break;
    }

    /* ---- Delimited continuations (copy-the-stack-segment model) ----
     * Metadata (mark slot, buffer header) is stored as 32-bit words: fp, sp
     * and the re-entry PC are all low-memory (< 4GB) addresses under the
     * ILP32 model, reloaded zero-extended in the runtime. See start_arm64.S. */

    case IR_MARK: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        memop(out, 1, "w29", "x29", off);           /* mark[0] = fp */
        fprintf(out, "\tmov x9, sp\n");
        memop(out, 1, "w9", "x29", off + 4);         /* mark[1] = sp */
        fprintf(out, "\tldr w9, =.Lmark%d_%d\n", label_prefix, i->label);
        memop(out, 1, "w9", "x29", off + 8);         /* mark[2] = re-entry PC */
        addimm(out, "x9", "x29", off);               /* x9 = &mark */
        fprintf(out, "\tldr x10, =__cont_mark_sp\n");
        fprintf(out, "\tstr w9, [x10]\n");           /* __cont_mark_sp = &mark */
        fprintf(out, "\tmov w0, #0\n");              /* first time: result = 0 */
        fprintf(out, ".Lmark%d_%d:\n", label_prefix, i->label);
        if (strcmp(sd, "w0") != 0)
            fprintf(out, "\tmov %s, w0\n", sd);      /* dst = 0, or buf on re-entry */
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_CAPTURE: {
        const char *sd = rd(fn, i->dst, 0);
        int k;
        /* push x19-x28 so they are part of the captured stack segment */
        fprintf(out, "\tsub sp, sp, #%d\n", NSAVED * 8);
        for (k = 0; k < NSAVED; k++)
            fprintf(out, "\tstr %s, [sp, #%d]\n", xr[k + 2], k * 8);
        fprintf(out, "\tbl __cont_capture\n");
        /* reached only on resume: pop x19-x28 back from the restored segment */
        for (k = 0; k < NSAVED; k++)
            fprintf(out, "\tldr %s, [sp, #%d]\n", xr[k + 2], k * 8);
        fprintf(out, "\tadd sp, sp, #%d\n", NSAVED * 8);
        if (strcmp(sd, "w0") != 0)
            fprintf(out, "\tmov %s, w0\n", sd);      /* dst = buf, or value on resume */
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_RESUME: {
        const char *sa = rs(out, fn, i->a, 0);       /* buf */
        const char *sb = rs(out, fn, i->b, 1);       /* value */
        fprintf(out, "\tsub sp, sp, #16\n");
        memop(out, 1, sa, "sp", 0);
        memop(out, 1, sb, "sp", 8);
        fprintf(out, "\tbl __cont_resume\n");        /* does not return here */
        break;
    }

    /* ---- Floating point (IEEE 754 double, hardware) ---- */

    case IR_FADD: emit_fbinop(out, fn, i, "fadd"); break;
    case IR_FSUB: emit_fbinop(out, fn, i, "fsub"); break;
    case IR_FMUL: emit_fbinop(out, fn, i, "fmul"); break;
    case IR_FDIV: emit_fbinop(out, fn, i, "fdiv"); break;
    case IR_FNEG: emit_funop(out, fn, i, "fneg"); break;
    case IR_FABS: emit_funop(out, fn, i, "fabs"); break;

    case IR_FCMPEQ: case IR_FCMPLT: case IR_FCMPLE: {
        /* fcmp condition codes chosen so NaN (unordered) yields false:
         * eq (Z), less-than mi (N), less-or-equal ls (C clear or Z). */
        int f32 = i->imm == FWIDTH_F32;
        const char *sa = frs_w(out, fn, i->a, 0, f32);
        const char *sb = frs_w(out, fn, i->b, 1, f32);
        const char *sd = rd(fn, i->dst, 0);         /* result is an int bool */
        const char *cond = i->op == IR_FCMPEQ ? "eq"
                         : i->op == IR_FCMPLT ? "mi"
                         :                      "ls";
        fprintf(out, "\tfcmp %s, %s\n", sa, sb);
        fprintf(out, "\tcset %s, %s\n", sd, cond);
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_ITOF: {
        const char *sa = rs(out, fn, i->a, 0);       /* int w-view */
        const char *sd = frd_w(fn, i->dst, 0, i->imm == FWIDTH_F32);
        fprintf(out, "\tscvtf %s, %s\n", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FTOI: {
        const char *sa = frs_w(out, fn, i->a, 0, i->imm == FWIDTH_F32);
        const char *sd = rd(fn, i->dst, 0);          /* int w-view */
        fprintf(out, "\tfcvtzs %s, %s\n", sd, sa);   /* round toward zero */
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_F32TOF64: {                              /* single -> double */
        const char *sa = frs_w(out, fn, i->a, 0, 1);
        const char *sd = frd_w(fn, i->dst, 0, 0);
        fprintf(out, "\tfcvt %s, %s\n", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_F64TOF32: {                              /* double -> single */
        const char *sa = frs_w(out, fn, i->a, 0, 0);
        const char *sd = frd_w(fn, i->dst, 0, 1);
        fprintf(out, "\tfcvt %s, %s\n", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FLDL: {
        const char *sd = frd_w(fn, i->dst, 0, i->imm == FWIDTH_F32);
        memop(out, 0, sd, "x29", slot_offset(fn, i->slot));
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSTL: {
        const char *sa = frs_w(out, fn, i->a, 0, i->imm == FWIDTH_F32);
        memop(out, 1, sa, "x29", slot_offset(fn, i->slot));
        break;
    }

    case IR_FLD: {
        const char *sa = ra(out, fn, i->a, 0);
        const char *sd = frd_w(fn, i->dst, 0, i->imm == FWIDTH_F32);
        fprintf(out, "\tldr %s, [%s]\n", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSD: {
        const char *sa = ra(out, fn, i->a, 0);
        const char *sb = frs_w(out, fn, i->b, 0, i->imm == FWIDTH_F32);
        fprintf(out, "\tstr %s, [%s]\n", sb, sa);
        break;
    }

    case IR_FLS: {                                   /* load single -> double */
        const char *sa = ra(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst, 0);
        fprintf(out, "\tldr s0, [%s]\n", sa);
        fprintf(out, "\tfcvt %s, s0\n", sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSS: {                                   /* double -> single, store */
        const char *sa = ra(out, fn, i->a, 0);
        const char *sb = frs(out, fn, i->b, 0);
        fprintf(out, "\tfcvt s0, %s\n", sb);
        fprintf(out, "\tstr s0, [%s]\n", sa);
        break;
    }

    case IR_FLH: {                                   /* load half -> double */
        const char *sa = ra(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst, 0);
        fprintf(out, "\tldr h0, [%s]\n", sa);        /* native fcvt, no helper */
        fprintf(out, "\tfcvt %s, h0\n", sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSH: {                                   /* double -> half, store */
        const char *sa = ra(out, fn, i->a, 0);
        const char *sb = frs(out, fn, i->b, 0);
        fprintf(out, "\tfcvt h0, %s\n", sb);
        fprintf(out, "\tstr h0, [%s]\n", sa);
        break;
    }

    case IR_FRETV: {
        const char *sa = frs(out, fn, i->a, 0);
        if (strcmp(sa, "d0") != 0)
            fprintf(out, "\tfmov d0, %s\n", sa);
        emit_epilogue(out, fn);
        break;
    }

    case IR_FARG:
        if (narg >= 16)
            die("arm64_emit: too many args");
        arg_is_i64[narg] = 0;
        arg_is_float[narg] = 1;
        arg_f32[narg] = (i->imm == FWIDTH_F32);   /* single vs double */
        arg_mem[narg] = 0;
        arg_temps[narg++] = i->a;
        break;

    case IR_FCALL:
        emit_call_flush(out, fn, i, 0, RET_FLOAT);
        break;
    case IR_FCALLI:
        emit_call_flush(out, fn, i, 1, RET_FLOAT);
        break;

    /* ---- I64 opcodes: native single-register 64-bit ---- */

    case IR_LIC64: {
        const char *sd = rdx(fn, i->dst, 0);
        fprintf(out, "\tldr %s, =%ld\n", sd, i->imm);
        wd64(out, fn, i->dst, sd);
        break;
    }

    case IR_ADD64: emit_binop64(out, fn, i, "add"); break;
    case IR_SUB64: emit_binop64(out, fn, i, "sub"); break;
    case IR_MUL64: emit_binop64(out, fn, i, "mul"); break;
    case IR_AND64: emit_binop64(out, fn, i, "and"); break;
    case IR_OR64:  emit_binop64(out, fn, i, "orr"); break;
    case IR_XOR64: emit_binop64(out, fn, i, "eor"); break;
    case IR_SHL64: emit_binop64(out, fn, i, "lsl"); break;
    case IR_SHRS64: emit_binop64(out, fn, i, "asr"); break;
    case IR_SHRU64: emit_binop64(out, fn, i, "lsr"); break;

    case IR_NEG64: {
        const char *sa = rsx(out, fn, i->a, 0);
        const char *sd = rdx(fn, i->dst, 0);
        fprintf(out, "\tneg %s, %s\n", sd, sa);
        wd64(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64EQ: case IR_CMP64NE:
    case IR_CMP64LTS: case IR_CMP64LES:
    case IR_CMP64GTS: case IR_CMP64GES:
    case IR_CMP64LTU: case IR_CMP64LEU:
    case IR_CMP64GTU: case IR_CMP64GEU: {
        const char *sa = rsx(out, fn, i->a, 0);
        const char *sb = rsx(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp %s, %s\n", sa, sb);
        fprintf(out, "\tcset %s, %s\n", sd, cmp64_cond(i->op));
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_LD64: {
        const char *sa = ra(out, fn, i->a, 0);
        const char *sd = rdx(fn, i->dst, 1);
        fprintf(out, "\tldr %s, [%s]\n", sd, sa);
        wd64(out, fn, i->dst, sd);
        break;
    }

    case IR_ST64: {
        const char *sa = ra(out, fn, i->a, 0);
        const char *sb = rsx(out, fn, i->b, 1);
        fprintf(out, "\tstr %s, [%s]\n", sb, sa);
        break;
    }

    case IR_LDL64: {
        const char *sd = rdx(fn, i->dst, 0);
        memop(out, 0, sd, "x29", slot_offset(fn, i->slot));
        wd64(out, fn, i->dst, sd);
        break;
    }

    case IR_STL64: {
        const char *sa = rsx(out, fn, i->a, 0);
        memop(out, 1, sa, "x29", slot_offset(fn, i->slot));
        break;
    }

    case IR_LEA64: {                            /* 64-bit symbol address */
        const char *sd = rdx(fn, i->dst, 0);
        fprintf(out, "\tldr %s, =%s\n", sd, i->sym);
        wd64(out, fn, i->dst, sd);
        break;
    }

    case IR_ADL64: {                            /* 64-bit local-slot address */
        const char *sd = rdx(fn, i->dst, 0);
        addimm(out, sd, "x29", slot_offset(fn, i->slot));
        wd64(out, fn, i->dst, sd);
        break;
    }

    case IR_SEXT64: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rdx(fn, i->dst, 0);
        fprintf(out, "\tsxtw %s, %s\n", sd, sa);
        wd64(out, fn, i->dst, sd);
        break;
    }

    case IR_ZEXT64: {
        const char *sa = rs(out, fn, i->a, 0);      /* w-view source */
        int r = fn->temp_reg[i->dst];
        const char *dw = r >= 0 ? wr[r] : wr[0];    /* w-view of dest */
        const char *dx = r >= 0 ? xr[r] : xr[0];    /* x-view of dest */
        /* writing the w-view zero-extends into the full x register */
        fprintf(out, "\tmov %s, %s\n", dw, sa);
        wd64(out, fn, i->dst, dx);
        break;
    }

    case IR_TRUNC64: {
        int ar = fn->temp_reg[i->a];
        const char *sw;
        const char *sd = rd(fn, i->dst, 0);
        if (ar >= 0) {
            sw = wr[ar];                            /* low 32 bits of source */
        } else {
            memop(out, 0, wr[1], "x29", spill_byte_offset(fn, i->a));
            sw = wr[1];
        }
        if (strcmp(sd, sw) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sw);
        wd32(out, fn, i->dst, sd);
        break;
    }

    case IR_CALL64:
        emit_call_flush(out, fn, i, 0, RET_I64);
        break;
    case IR_CALLI64:
        emit_call_flush(out, fn, i, 1, RET_I64);
        break;

#ifdef CC_PSABI
    case IR_VA_START: {
        /* fill the AAPCS64 va_list at [i->a]:
             +0  __stack   : first incoming stack vararg
             +8  __gr_top  : past the GP save area
             +16 __vr_top  : past the VR save area
             +24 __gr_offs : -(8 - named GP regs) * 8  (negative, counts up)
             +28 __vr_offs : -(8 - named FP regs) * 16 */
        int ng = 0, nf = 0, ns = 0, k, j;
        int fr, r;
        const char *ap;
        for (k = 0; k < fn->nparams; k++) {
            struct aapcs_ploc p;
            aapcs_param(fn, k, &p);
            if (p.cls[0] == -3)         /* the x8 result pointer, not an arg */
                continue;
            if (!p.is_reg) {
                /* an overflow param occupies hwords 8-byte stack slots (a
                   float HFA has more slots than home words) */
                ns += p.hwords;
                continue;
            }
            for (j = 0; j < p.neb; j++) {
                if (p.cls[j] == 1 || p.cls[j] == 2) nf++;
                else ng++;
            }
        }
        fr = frame_reserve(fn);
        r = fn->temp_reg[i->a];
        if (r >= 0) {
            ap = xr[r];
        } else {
            memop(out, 0, "x10", "x29", spill_byte_offset(fn, i->a));
            ap = "x10";
        }
        addimm(out, "x9", "x29", 16 + 8 * ns);   /* __stack */
        memop(out, 1, "x9", ap, 0);
        addimm(out, "x9", "x29", -fr + 64);       /* __gr_top */
        memop(out, 1, "x9", ap, 8);
        addimm(out, "x9", "x29", -fr + 64 + 128); /* __vr_top */
        memop(out, 1, "x9", ap, 16);
        fprintf(out, "\tmov w9, #%d\n\tneg w9, w9\n", (8 - ng) * 8);
        memop(out, 1, "w9", ap, 24);              /* __gr_offs */
        fprintf(out, "\tmov w9, #%d\n\tneg w9, w9\n", (8 - nf) * 16);
        memop(out, 1, "w9", ap, 28);              /* __vr_offs */
        break;
    }
    case IR_RETV_AGG: {
        /* pack the struct at [i->a] into the return registers: each slot goes
           to x0/x1 (integer), d0..d3 (double HFA), or s0..s3 (float HFA) per
           fn->ret_cls; a float HFA packs 4-byte singles */
        int r = fn->temp_reg[i->a];
        int j, ii = 0, ff = 0;
        int stride = (fn->ret_cls[0] == 2) ? 4 : 8;
        const char *ab;
        if (r >= 0) {
            ab = xr[r];
        } else {
            memop(out, 0, "x10", "x29", spill_byte_offset(fn, i->a));
            ab = "x10";
        }
        for (j = 0; j < fn->ret_neb; j++) {
            if (fn->ret_cls[j] == 2)
                memop(out, 0, aapcs_s[ff++], ab, stride * j);
            else if (fn->ret_cls[j] == 1)
                memop(out, 0, aapcs_d[ff++], ab, stride * j);
            else
                memop(out, 0, aapcs_ix[ii++], ab, stride * j);
        }
        emit_epilogue(out, fn);
        break;
    }
    case IR_CALL_AGG:
    case IR_CALLI_AGG: {
        /* make the call, then store the returned struct's register slots into
           the dest slot; imm holds a 2-bit class per slot (0 int, 1 double, 2
           float) at bits 3+2*j, a float HFA packing 4-byte singles */
        int neb = i->imm & 7;
        int j, ii = 0, ff = 0, base;
        int stride = (((i->imm >> 3) & 3) == 2) ? 4 : 8;
        emit_call_flush(out, fn, i, i->op == IR_CALLI_AGG, RET_I64);
        base = slot_offset(fn, i->slot);
        for (j = 0; j < neb; j++) {
            int c = (i->imm >> (3 + 2 * j)) & 3;
            if (c == 2)
                memop(out, 1, aapcs_s[ff++], "x29", base + stride * j);
            else if (c == 1)
                memop(out, 1, aapcs_d[ff++], "x29", base + stride * j);
            else
                memop(out, 1, aapcs_ix[ii++], "x29", base + stride * j);
        }
        break;
    }
#endif

    case IR_RETV64: {
        const char *sa = rsx(out, fn, i->a, 0);
        if (strcmp(sa, "x0") != 0)
            fprintf(out, "\tmov x0, %s\n", sa);
        emit_epilogue(out, fn);
        break;
    }

    case IR_FUNC:
    case IR_ENDF:
        break;

    default:
        die("arm64_emit: unhandled op %d", i->op);
    }
}

/****************************************************************
 * Per-function emission
 ****************************************************************/

static int fn_serial;

static void
emit_function(FILE *out, struct ir_func *fn)
{
    struct ir_insn *i;
    struct ir_insn *last = NULL;

    narg = 0;
    label_prefix = ++fn_serial;

    fprintf(out, "\n\t.text\n\t.align 2\n");
    if (!fn->is_local)
        fprintf(out, "\t.globl %s\n", fn->name);
    fprintf(out, "%s:\n", fn->name);
    emit_prologue(out, fn);

    for (i = fn->head; i; i = i->next) {
        emit_insn(out, fn, i);
        if (i->op != IR_FUNC && i->op != IR_ENDF &&
            i->op != IR_LABEL)
            last = i;
    }

    if (!last || (last->op != IR_RET && last->op != IR_RETV &&
        last->op != IR_RETV64 &&
        last->op != IR_TAILCALL && last->op != IR_TAILCALLI))
        emit_epilogue(out, fn);

    /* dump this function's literal pool while it is still in range */
    fprintf(out, "\t.ltorg\n");
}

/****************************************************************
 * Globals
 ****************************************************************/

static void
emit_string_bytes(FILE *out, const char *s, int n)
{
    int k;

    fputs("\t.ascii \"", out);
    for (k = 0; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c == '\\')
            fputs("\\\\", out);
        else if (c == '"')
            fputs("\\\"", out);
        else if (c == '\n')
            fputs("\\n", out);
        else if (c == '\t')
            fputs("\\t", out);
        else if (c == '\r')
            fputs("\\r", out);
        else if (c >= 0x20 && c < 0x7f)
            fputc(c, out);
        else
            fprintf(out, "\\%03o", c);
    }
    fputs("\"\n", out);
}

/* emit an aggregate global's byte image: pad to each item's offset, emit the
   sized value (.quad for 8-byte, little-endian), pad to the full size */
static void
emit_init_image(FILE *out, struct ir_global *g)
{
    struct ir_init *it;
    int cur = 0;

    for (it = g->inits; it; it = it->next) {
        if (it->offset > cur) {
            fprintf(out, "\t.space %d\n", it->offset - cur);
            cur = it->offset;
        }
        if (it->sym)
            fprintf(out, "\t%s %s\n", it->size == 8 ? ".quad" : ".word",
                it->sym);
        else if (it->size == 8)
            fprintf(out, "\t.quad 0x%016llx\n",
                (unsigned long long)(uint64_t)it->ival);
        else if (it->size == 2)
            fprintf(out, "\t.short 0x%04x\n", (unsigned)(it->ival & 0xFFFF));
        else if (it->size == 1)
            fprintf(out, "\t.byte 0x%02x\n", (unsigned)(it->ival & 0xFF));
        else
            fprintf(out, "\t.word 0x%08x\n", (unsigned)it->ival);
        cur += it->size;
    }
    if (g->arr_size > cur)
        fprintf(out, "\t.space %d\n", g->arr_size - cur);
}

static void
emit_globals(FILE *out, struct ir_program *prog)
{
    struct ir_global *g;

    fputs("\n\t.data\n", out);
    for (g = prog->globals; g; g = g->next) {
        int elsz;

        switch (g->base_type) {
        case IR_I8:  elsz = 1; break;
        case IR_I16: elsz = 2; break;
        case IR_I64: elsz = 8; break;
        case IR_F64: elsz = 8; break;
        default:     elsz = 4; break;
        }

        {
            /* arm64 aligns globals to 8; an _Alignas request may raise it.
               GAS .align is a power-of-two exponent. */
            int alb = 8, e = 0;
            if (g->align > alb)
                alb = g->align;
            while ((1 << e) < alb)
                e++;
            fprintf(out, "\t.align %d\n", e);
        }
        if (!g->is_local)
            fprintf(out, "\t.globl %s\n", g->name);
        fprintf(out, "%s:\n", g->name);
        if (g->inits) {
            emit_init_image(out, g);
        } else if (g->init_string) {
            emit_string_bytes(out, g->init_string,
                      g->init_strlen);
        } else if (g->init_count > 0) {
            int k;
            for (k = 0; k < g->init_count; k++) {
                if (g->init_syms && g->init_syms[k])
                    fprintf(out, "\t%s %s\n",
                        elsz == 8 ? ".quad" : ".word", g->init_syms[k]);
                else if (g->base_type == IR_I64 || g->base_type == IR_F64)
                    fprintf(out, "\t.quad %" PRId64 "\n",
                        g->init_ivals[k]);
                else if (elsz == 1)
                    fprintf(out, "\t.byte %" PRId64 "\n",
                        g->init_ivals[k]);
                else if (elsz == 2)
                    fprintf(out, "\t.short %" PRId64 "\n",
                        g->init_ivals[k]);
                else
                    fprintf(out, "\t.word %" PRId64 "\n",
                        g->init_ivals[k]);
            }
            if (g->arr_size > g->init_count)
                fprintf(out, "\t.space %d\n",
                    (g->arr_size - g->init_count) * elsz);
        } else {
            int sz = (g->arr_size > 0)
                 ? g->arr_size * elsz
                 : elsz;
            fprintf(out, "\t.space %d\n", sz);
        }
    }
}

/****************************************************************
 * Entry point
 ****************************************************************/

void
target_emit(FILE *out, struct ir_program *prog)
{
    struct ir_func *fn;

    fputs("// generated AArch64 (ARM64) assembly\n", out);
    for (fn = prog->funcs; fn; fn = fn->next)
        emit_function(out, fn);
    emit_globals(out, prog);
    /* mark the stack non-executable (AArch64 GAS uses %progbits, since @ opens
       a comment) so the GNU linker does not warn about the missing note */
    fputs("\n\t.section .note.GNU-stack,\"\",%progbits\n", out);
}
