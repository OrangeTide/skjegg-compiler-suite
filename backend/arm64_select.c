/* arm64_select.c : the shared AArch64 (AAPCS64) instruction selector (see
   arm64_select.h).

   Lifted from backend/arm64_emit.c's CC_PSABI path: the register model, frame
   geometry, AAPCS64 parameter classification and call marshalling, prologue/
   epilogue, and the per-opcode switch, all re-expressed against the
   backend/arm64_mc.h sink.  The AOT (text) and JIT (byte) paths supply the sink;
   this selection logic is shared.  The AArch64 parallel of backend/x86_select.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "arm64_mc.h"
#include "arm64_select.h"

#define NSAVED 10               /* callee-saved x19..x28 */
#define ARM64_MAX_ARGS 32

/****************************************************************
 * Register model
 *
 * The allocator hands out temp_reg as an index: 2..11 for the ten callee-saved
 * integer registers x19..x28, and 2..9 for the eight callee-saved FP registers
 * d8..d15 (FIRST_REG=2 in regalloc_arm64.c).  Index 0/1 are the scratch slots.
 * These tables map an index to the hardware register number; scratch uses x9/x10
 * (integer) and d0/d1 (float), and x15 is the address/immediate fallback.
 ****************************************************************/

static const int xreg[] = { 9, 10, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };
static const int freg[] = { 0, 1, 8, 9, 10, 11, 12, 13, 14, 15 };

#define SC0 9                   /* integer scratch A (x9)  */
#define SC1 10                  /* integer scratch B (x10) */
#define SCA 15                  /* address / immediate fallback (x15) */
#define SCQ 15                  /* quotient temp for modulo (w15/x15) */
#define FSC0 0                  /* float scratch A (d0/s0) */
#define FSC1 1                  /* float scratch B (d1/s1) */

/* Per-temp float storage format (1 = f32/single, 0 = f64): the C front end keeps
   `float` as f32 in registers while Kobold keeps every float as f64, so a single
   value crossing a call is converted only when the temp's format differs from the
   f32 wire width.  Set per function in arm64_select_func (see ir_classify_f32). */
static const unsigned char *g_f32;
static int src_is_f32(int t) { return g_f32 && t >= 0 && g_f32[t]; }

/****************************************************************
 * Frame geometry (ported verbatim from arm64_emit.c; pure computation)
 ****************************************************************/

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
        if (i == slot) { *out = p; return; }
    }
}

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

static int param_home(struct ir_func *fn) { return aapcs_nreg_params(fn) * 8; }
static int va_save(struct ir_func *fn) { return fn->is_variadic ? 192 : 0; }
static int frame_reserve(struct ir_func *fn) { return param_home(fn) + va_save(fn); }

static int
locals_size(struct ir_func *fn)
{
    int i, s = 0;
    for (i = fn->nparams; i < fn->nslots; i++)
        s += (fn->slot_size[i] + 7) & ~7;
    return s;
}

static int
slot_offset(struct ir_func *fn, int slot)
{
    int i, off;

    if (slot < fn->nparams) {
        struct aapcs_ploc p;
        aapcs_param(fn, slot, &p);
        if (p.is_reg)
            return -8 * (p.hidx + p.hwords);
        return 16 + 8 * p.soff;
    }
    off = 0;
    for (i = fn->nparams; i <= slot; i++)
        off += (fn->slot_size[i] + 7) & ~7;
    return -frame_reserve(fn) - off;
}

static int
frame_size(struct ir_func *fn)
{
    int f = frame_reserve(fn) + locals_size(fn) +
            fn->nspills * 8 + fn->nfspills * 8;
    return (f + 15) & ~15;
}

static int
spill_off(struct ir_func *fn, int t)
{
    return -frame_reserve(fn) - locals_size(fn) - (fn->temp_spill[t] + 8);
}

static int
fspill_off(struct ir_func *fn, int t)
{
    return -frame_reserve(fn) - locals_size(fn) - fn->nspills * 8
           - (fn->temp_spill[t] + 8);
}

static int
fsave_mask(struct ir_func *fn)
{
    struct ir_insn *i;
    int mask = 0;
    for (i = fn->head; i; i = i->next)
        if (i->dst >= 0 && ir_op_is_float_def(i->op)) {
            int r = fn->temp_reg[i->dst];
            if (r >= 2 && r <= 9)
                mask |= 1 << r;
        }
    return mask;
}

static int
fsave_bytes(struct ir_func *fn)
{
    int m = fsave_mask(fn), n = 0;
    while (m) { n += m & 1; m >>= 1; }
    return (n * 8 + 15) & ~15;
}

/****************************************************************
 * Temp -> register materialization (returns hardware register numbers)
 ****************************************************************/

/* Integer source in its w-view value (or reloaded low 32 bits into scratch). */
static int
rs(struct mc64 *m, struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];
    if (r >= 0)
        return xreg[r];
    mc64_ldrw(m, scr, MC64_FP, spill_off(fn, t));
    return scr;
}

/* Integer source in its x-view (full 64 bits: an i64 temp). */
static int
rsx(struct mc64 *m, struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];
    if (r >= 0)
        return xreg[r];
    mc64_ldrx(m, scr, MC64_FP, spill_off(fn, t));
    return scr;
}

/* An address-holding temp as a load/store base: a 32-bit value whose high bits
   are zero, reloaded zero-extended (ldrw) for a spill. */
static int
ra(struct mc64 *m, struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];
    if (r >= 0)
        return xreg[r];
    mc64_ldrw(m, scr, MC64_FP, spill_off(fn, t));
    return scr;
}

/* The register to compute a destination into: its own, or the scratch scr. */
static int
rd(struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];
    return r >= 0 ? xreg[r] : scr;
}

static void
wd32(struct mc64 *m, struct ir_func *fn, int t, int reg)
{
    if (fn->temp_reg[t] < 0)
        mc64_strw(m, reg, MC64_FP, spill_off(fn, t));
}

static void
wd64(struct mc64 *m, struct ir_func *fn, int t, int reg)
{
    if (fn->temp_reg[t] < 0)
        mc64_strx(m, reg, MC64_FP, spill_off(fn, t));
}

/* Float source register (v-number), reloaded into scratch if spilled. */
static int
frs(struct mc64 *m, struct ir_func *fn, int t, int scr, int f32)
{
    int r = fn->temp_reg[t];
    if (r >= 0)
        return freg[r];
    mc64_ldr_fp(m, f32 ? 2 : 3, scr, MC64_FP, fspill_off(fn, t));
    return scr;
}

static int
frd(struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];
    return r >= 0 ? freg[r] : scr;
}

static void
fwd(struct mc64 *m, struct ir_func *fn, int t, int reg, int f32)
{
    if (fn->temp_reg[t] < 0)
        mc64_str_fp(m, f32 ? 2 : 3, reg, MC64_FP, fspill_off(fn, t));
}

/****************************************************************
 * Condition codes
 ****************************************************************/

static int
cc_of(int op)
{
    switch (op) {
    case IR_CMPEQ:  case IR_CMP64EQ:  return MC64_EQ;
    case IR_CMPNE:  case IR_CMP64NE:  return MC64_NE;
    case IR_CMPLTS: case IR_CMP64LTS: return MC64_LT;
    case IR_CMPLES: case IR_CMP64LES: return MC64_LE;
    case IR_CMPGTS: case IR_CMP64GTS: return MC64_GT;
    case IR_CMPGES: case IR_CMP64GES: return MC64_GE;
    case IR_CMPLTU: case IR_CMP64LTU: return MC64_CC;   /* lo */
    case IR_CMPLEU: case IR_CMP64LEU: return MC64_LS;
    case IR_CMPGTU: case IR_CMP64GTU: return MC64_HI;
    case IR_CMPGEU: case IR_CMP64GEU: return MC64_CS;   /* hs */
    default: return -1;
    }
}

static int is_cmp32(int op) { return op >= IR_CMPEQ && op <= IR_CMPGEU; }

/****************************************************************
 * Prologue / epilogue
 ****************************************************************/

static void
prologue(struct mc64 *m, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int fs = fsave_bytes(fn);
    int fmask = fsave_mask(fn);
    int total = 16 + frame + NSAVED * 8 + fs;
    int k, mm;

    /* Layout from sp up: saved d-regs (fs), saved x19-x28 (NSAVED*8), frame,
       old x29, lr. */
    mc64_addimm(m, MC64_SP, MC64_SP, -total);
    mc64_strx(m, MC64_LR, MC64_SP, fs + NSAVED * 8 + frame + 8);
    mc64_strx(m, MC64_FP, MC64_SP, fs + NSAVED * 8 + frame);
    mc64_addimm(m, MC64_FP, MC64_SP, fs + NSAVED * 8 + frame);
    for (k = 0; k < NSAVED; k++)
        mc64_strx(m, xreg[k + 2], MC64_SP, fs + k * 8);
    for (k = 2, mm = 0; k <= 9; k++)
        if (fmask & (1 << k))
            mc64_str_fp(m, 3, freg[k], MC64_SP, mm++ * 8);

    /* AAPCS64: spill register params into their home slots below x29. */
    for (k = 0; k < fn->nparams; k++) {
        struct aapcs_ploc p;
        int j;
        aapcs_param(fn, k, &p);
        if (!p.is_reg)
            continue;
        if (p.cls[0] == -3) {           /* the x8 indirect-result pointer */
            mc64_strx(m, 8, MC64_FP, -8 * (p.hidx + 1));
            continue;
        }
        for (j = 0; j < p.neb; j++) {
            int off = -8 * (p.hidx + p.hwords) + p.stride * j;
            if (p.cls[j] == 2)
                mc64_str_fp(m, 2, p.ridx[j], MC64_FP, off);
            else if (p.cls[j] == 1)
                mc64_str_fp(m, 3, p.ridx[j], MC64_FP, off);
            else
                mc64_strx(m, p.ridx[j], MC64_FP, off);
        }
    }

    /* Variadic: save all argument registers to the register save area (base at
       x29 - frame_reserve) so va_arg can walk them. */
    if (fn->is_variadic) {
        int base = -frame_reserve(fn);
        for (k = 0; k < 8; k++)
            mc64_strx(m, k, MC64_FP, base + 8 * k);
        for (k = 0; k < 8; k++)
            mc64_str_fp(m, 3, k, MC64_FP, base + 64 + 16 * k);
    }
}

static void
epilogue_no_ret(struct mc64 *m, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int fs = fsave_bytes(fn);
    int fmask = fsave_mask(fn);
    int saved_base = -(NSAVED * 8 + frame);
    int k, mm;

    for (k = 0; k < NSAVED; k++)
        mc64_ldrx(m, xreg[k + 2], MC64_FP, saved_base + k * 8);
    for (k = 2, mm = 0; k <= 9; k++)
        if (fmask & (1 << k))
            mc64_ldr_fp(m, 3, freg[k], MC64_FP, -(fs + NSAVED * 8 + frame) + mm++ * 8);
    mc64_ldrx(m, MC64_LR, MC64_FP, 8);
    mc64_ldrx(m, 9, MC64_FP, 0);            /* old x29 into x9 */
    mc64_addimm(m, MC64_SP, MC64_FP, 16);
    mc64_mov(m, 1, MC64_FP, 9);             /* mov x29, x9 */
}

static void
epilogue(struct mc64 *m, struct ir_func *fn)
{
    epilogue_no_ret(m, fn);
    mc64_ret(m);
}

/****************************************************************
 * Call marshalling (AAPCS64)
 ****************************************************************/

enum { RET_I32, RET_I64, RET_FLOAT, RET_FAIL };
/* RET_FAIL: a failable call returns its value in x0 and its error in x1, which is
   spilled to the call's error slot; the value is then taken as a plain i32 result. */

/* Place the pending arguments per AAPCS64, make the call, restore the stack, and
   deliver the result.  Register targets (x0..x7 / d0..d7) are disjoint from the
   allocatable registers (x19-x28 / d8-d15), so each source moves straight into
   its target with no parallel-move hazard. */
static void
emit_call(struct mc64 *m, struct ir_func *fn, struct ir_insn *in,
          int *at, char *aisf, char *ai64, char *af32, int *amem, int narg,
          int x8_temp, int indirect, int retkind)
{
    int treg[ARM64_MAX_ARGS], onstk[ARM64_MAX_ARGS], ssize[ARM64_MAX_ARGS];
    int iu = 0, du = 0, stack_bytes = 0, stkb, off, k;

    for (k = 0; k < narg; k++) {
        onstk[k] = 0;
        ssize[k] = 0;
        if (amem[k] > 0) {
            onstk[k] = 1;
            ssize[k] = (amem[k] + 7) & ~7;
        } else if (aisf[k]) {
            if (du < 8) treg[k] = du++; else { onstk[k] = 1; ssize[k] = 8; }
        } else {
            if (iu < 8) treg[k] = iu++; else { onstk[k] = 1; ssize[k] = 8; }
        }
        stack_bytes += ssize[k];
    }
    stkb = (stack_bytes + 15) & ~15;
    if (stkb > 0)
        mc64_addimm(m, MC64_SP, MC64_SP, -stkb);

    /* stack arguments first (through scratch, not argument registers) */
    off = 0;
    for (k = 0; k < narg; k++) {
        if (!onstk[k])
            continue;
        if (amem[k] > 0) {
            int t = at[k], base, o = 0, rem = amem[k];
            base = ra(m, fn, t, SC0);
            while (rem >= 8) {
                mc64_ldrx(m, SC1, base, o);
                mc64_strx(m, SC1, MC64_SP, off + o);
                o += 8; rem -= 8;
            }
            if (rem >= 4) {
                mc64_ldrw(m, SC1, base, o);
                mc64_strw(m, SC1, MC64_SP, off + o);
            }
        } else if (aisf[k]) {
            /* a single arg is stored as f32: narrowed from an f64-format temp
               (Kobold), or stored directly when the temp is already f32 (C). */
            if (af32[k] && src_is_f32(at[k])) {
                int s = frs(m, fn, at[k], FSC0, 1);
                mc64_str_fp(m, 2, s, MC64_SP, off);
            } else if (af32[k]) {
                int s = frs(m, fn, at[k], FSC0, 0);
                mc64_fcvt(m, 2, 3, FSC0, s);
                mc64_str_fp(m, 2, FSC0, MC64_SP, off);
            } else {
                int s = frs(m, fn, at[k], FSC0, 0);
                mc64_str_fp(m, 3, s, MC64_SP, off);
            }
        } else if (ai64[k]) {
            mc64_strx(m, rsx(m, fn, at[k], SC0), MC64_SP, off);
        } else {
            mc64_strw(m, rs(m, fn, at[k], SC0), MC64_SP, off);
        }
        off += ssize[k];
    }

    /* register arguments */
    for (k = 0; k < narg; k++) {
        int t = at[k], r;
        if (onstk[k])
            continue;
        r = fn->temp_reg[t];
        if (aisf[k]) {
            /* a single arg narrows an f64-format temp to f32 (Kobold); a temp
               already f32 (C) moves as single, and a double moves as f64. */
            int narrow = af32[k] && !src_is_f32(t);
            int rw = (af32[k] && src_is_f32(t)) ? 2 : 3;
            if (r >= 0) {
                if (narrow)
                    mc64_fcvt(m, 2, 3, treg[k], freg[r]);
                else
                    mc64_fmov(m, rw, treg[k], freg[r]);
            } else {
                mc64_ldr_fp(m, rw, treg[k], MC64_FP, fspill_off(fn, t));
                if (narrow)
                    mc64_fcvt(m, 2, 3, treg[k], treg[k]);
            }
        } else if (ai64[k]) {
            if (r >= 0)
                mc64_mov(m, 1, treg[k], xreg[r]);
            else
                mc64_ldrx(m, treg[k], MC64_FP, spill_off(fn, t));
        } else {
            if (r >= 0)
                mc64_mov(m, 0, treg[k], xreg[r]);
            else
                mc64_ldrw(m, treg[k], MC64_FP, spill_off(fn, t));
        }
    }

    if (x8_temp >= 0) {
        int r = fn->temp_reg[x8_temp];
        if (r >= 0)
            mc64_mov(m, 1, 8, xreg[r]);
        else
            mc64_ldrx(m, 8, MC64_FP, spill_off(fn, x8_temp));
    }

    if (indirect) {
        int sa = ra(m, fn, in->a, SC0);
        mc64_mov(m, 1, 16, sa);         /* x16: a scratch, not an arg reg */
        mc64_blr(m, 16);
    } else {
        mc64_call_sym(m, in->sym);
    }

    if (stkb > 0)
        mc64_addimm(m, MC64_SP, MC64_SP, stkb);

    /* A failable call's error register (x1) goes to the error slot regardless of
       whether the value result is used; the value (x0) is then an i32 result. */
    if (retkind == RET_FAIL)
        mc64_strx(m, 1, MC64_FP, slot_offset(fn, in->slot));

    if (in->dst >= 0) {
        if (retkind == RET_FLOAT) {
            int sd;
            /* a single result comes back as f32 in d0; widen it to f64 only when
               the destination temp holds f64 format (Kobold), not when it stays
               single (C) */
            if (in->imm == FWIDTH_F32 && !src_is_f32(in->dst))
                mc64_fcvt(m, 3, 2, 0, 0);
            sd = frd(fn, in->dst, FSC0);
            if (sd != 0)
                mc64_fmov(m, 3, sd, 0);
            fwd(m, fn, in->dst, sd, 0);
        } else if (retkind == RET_I64) {
            int sd = rd(fn, in->dst, SC0);
            if (sd != 0)
                mc64_mov(m, 1, sd, 0);
            wd64(m, fn, in->dst, sd);
        } else {
            int sd = rd(fn, in->dst, SC0);
            if (sd != 0)
                mc64_mov(m, 0, sd, 0);
            wd32(m, fn, in->dst, sd);
        }
    }
}

/****************************************************************
 * Function emit
 ****************************************************************/

static int
il(struct mc64 *m, int *lbl, int n)
{
    if (lbl[n] < 0)
        lbl[n] = mc64_new_label(m);
    return lbl[n];
}

/* Guard a divisor for the JIT trap policy: if it is zero, raise DIV_ZERO. */
static void
div_guard(struct mc64 *m, int tp, int sf, int divisor)
{
    if (tp == ARM64_TRAP_GUARDED) {
        int over = mc64_new_label(m);
        mc64_cbnz(m, sf, divisor, over);
        mc64_trap_div_zero(m);
        mc64_label(m, over);
    }
}

/* Guard INT_MIN / -1 (and its modulo) for the guarded trap policy: a 32-bit
   sdiv yields INT_MIN silently, whose true result is unrepresentable, so raise
   OVERFLOW.  Only the 32-bit signed div/mod needs it.  SCQ is free here (the
   divide writes it afterward); AArch64 has no b.cond-to-label, so the compare
   is cmp + cset + cbnz, as the overflow path does. */
static void
div_overflow_guard(struct mc64 *m, int tp, int sa, int sb)
{
    if (tp == ARM64_TRAP_GUARDED) {
        int ok = mc64_new_label(m);
        mc64_movimm(m, 0, SCQ, 0xFFFFFFFFu);   /* -1 */
        mc64_cmp(m, 0, sb, SCQ);
        mc64_cset(m, 0, SCQ, MC64_NE);
        mc64_cbnz(m, 0, SCQ, ok);              /* divisor != -1: cannot overflow */
        mc64_movimm(m, 0, SCQ, 0x80000000u);   /* INT_MIN */
        mc64_cmp(m, 0, sa, SCQ);
        mc64_cset(m, 0, SCQ, MC64_NE);
        mc64_cbnz(m, 0, SCQ, ok);              /* dividend != INT_MIN: no overflow */
        mc64_trap_overflow(m);
        mc64_label(m, ok);
    }
}

/* A pin/clobber register name ("x8", "w0", "fp", "lr") to its hardware number,
   or -1.  w and x share a number; the width comes from the operand size. */
static int
a64_name_to_hw(const char *name)
{
    if (strcmp(name, "fp") == 0) return 29;
    if (strcmp(name, "lr") == 0) return 30;
    if ((name[0] == 'x' || name[0] == 'w') && name[1]) {
        int h = 0;
        const char *p = name + 1;
        while (*p >= '0' && *p <= '9')
            h = h * 10 + (*p++ - '0');
        if (*p == '\0' && h <= 30)
            return h;
    }
    return -1;
}

static int
asm_hw_clobbered(struct ir_asm_info *ai, int h)
{
    int k;
    for (k = 0; k < ai->nclobbers; k++)
        if (a64_name_to_hw(ai->clobbers[k]) == h)
            return 1;
    return 0;
}

static int
asm_hw_pinned(struct ir_asm_info *ai, int h)
{
    int k, n = ai->noutputs + ai->ninputs;
    for (k = 0; k < n; k++)
        if (ai->ops[k].pin_reg && a64_name_to_hw(ai->ops[k].pin_reg) == h)
            return 1;
    return 0;
}

/* The allocator index (temp_reg value) of hardware register `h`, or -1 if `h`
   is not allocatable. */
static int
a64_hw_to_index(int h)
{
    int k;
    for (k = 2; k < (int)(sizeof xreg / sizeof xreg[0]); k++)
        if (xreg[k] == h)
            return k;
    return -1;
}

/* Fill out[] (capacity max) with the allocator indices of the allocatable
   registers any extended asm in fn pins or clobbers, deduplicated. */
int
arm64_asm_reserved_regs(struct ir_func *fn, int *out, int max)
{
    struct ir_insn *i;
    int n = 0, k, j;

    for (i = fn->head; i; i = i->next) {
        struct ir_asm_info *ai = i->asm_info;
        if (i->op != IR_ASM || !ai)
            continue;
        for (k = 0; k < ai->noutputs + ai->ninputs; k++) {
            int h, idx;
            if (!ai->ops[k].pin_reg)
                continue;
            h = a64_name_to_hw(ai->ops[k].pin_reg);
            idx = h < 0 ? -1 : a64_hw_to_index(h);
            if (idx < 0)
                continue;
            for (j = 0; j < n && out[j] != idx; j++)
                ;
            if (j == n && n < max)
                out[n++] = idx;
        }
        for (k = 0; k < ai->nclobbers; k++) {
            int h = a64_name_to_hw(ai->clobbers[k]);
            int idx = h < 0 ? -1 : a64_hw_to_index(h);
            if (idx < 0)
                continue;
            for (j = 0; j < n && out[j] != idx; j++)
                ;
            if (j == n && n < max)
                out[n++] = idx;
        }
    }
    return n;
}

/* Extended inline asm.  A generic "r" operand goes in its allocated x-register
 * or a scratch materialised from the spill slot; a pinned operand goes in the
 * named physical register (its value moved in from the operand temp's home and,
 * for an output, back out).  Each %N is substituted with the register name at
 * the operand's width (x8 for an 8-byte operand, w8 for a 4-byte one) and the
 * template is emitted as one blob.  This is the syscall case: x0..x8 are not
 * allocatable, so the pins are free. */
static void
emit_asm_ext(struct mc64 *m, struct ir_func *fn, struct ir_insn *i)
{
    struct ir_asm_info *ai = i->asm_info;
    int n = ai->noutputs + ai->ninputs;
    static const int pool[] = { 9, 10, 11, 12, 13, 14 };
    int npool = (int)(sizeof pool / sizeof pool[0]);
    int ophw[32];
    int next = 0, k;
    const char *s;
    char buf[1024];
    size_t bp = 0;

    if (n > 32)
        die("arm64: too many asm operands");

    for (k = 0; k < n; k++) {
        struct ir_asm_op *op = &ai->ops[k];

        if (op->pin_reg) {
            int j, h = a64_name_to_hw(op->pin_reg);
            if (h < 0)
                die("arm64: unknown asm register %s", op->pin_reg);
            /* an allocatable pin is fine: the allocator reserved it (see
               arm64_asm_reserved_regs), so no temp lives there across the asm */
            for (j = 0; j < k; j++)
                if (ai->ops[j].pin_reg &&
                    a64_name_to_hw(ai->ops[j].pin_reg) == h)
                    die("arm64: two asm operands pinned to %s", op->pin_reg);
            ophw[k] = h;
        } else if (fn->temp_reg[op->temp] >= 0) {
            ophw[k] = xreg[fn->temp_reg[op->temp]];
        } else {
            ophw[k] = -1;
        }
    }
    for (k = 0; k < n; k++) {
        if (ophw[k] >= 0)
            continue;
        while (next < npool && (asm_hw_pinned(ai, pool[next]) ||
                                asm_hw_clobbered(ai, pool[next])))
            next++;
        if (next >= npool)
            die("arm64: too many spilled asm operands");
        ophw[k] = pool[next++];
    }

    /* materialise input values (home is 64-bit; use 64-bit moves/loads) */
    for (k = 0; k < n; k++) {
        struct ir_asm_op *op = &ai->ops[k];
        int home = fn->temp_reg[op->temp];

        if (op->is_output && !op->is_inout)
            continue;
        if (op->pin_reg) {
            if (home >= 0)
                mc64_mov(m, 1, ophw[k], xreg[home]);
            else
                mc64_ldrx(m, ophw[k], MC64_FP, spill_off(fn, op->temp));
        } else if (home < 0) {
            mc64_ldrx(m, ophw[k], MC64_FP, spill_off(fn, op->temp));
        }
    }

    /* build the template with %N substituted (width from the operand size) */
    for (s = i->sym; *s && bp + 1 < sizeof buf; s++) {
        if (*s != '%') {
            buf[bp++] = *s;
            continue;
        }
        s++;
        if (*s == '%') {
            buf[bp++] = '%';
        } else if (*s >= '0' && *s <= '9') {
            int idx = 0;
            char rn[8];
            const char *nm;
            while (*s >= '0' && *s <= '9')
                idx = idx * 10 + (*s++ - '0');
            s--;
            if (idx >= n)
                die("arm64: asm operand %%%d out of range", idx);
            snprintf(rn, sizeof rn, "%c%d",
                     ai->ops[idx].size >= 8 ? 'x' : 'w', ophw[idx]);
            nm = rn;
            while (*nm && bp + 1 < sizeof buf)
                buf[bp++] = *nm++;
        } else {
            buf[bp++] = '%';
            if (bp + 1 < sizeof buf)
                buf[bp++] = *s;
        }
    }
    buf[bp] = '\0';
    mc64_asm(m, buf);

    /* store outputs back */
    for (k = 0; k < ai->noutputs; k++) {
        struct ir_asm_op *op = &ai->ops[k];
        int home = fn->temp_reg[op->temp];

        if (op->pin_reg) {
            if (home >= 0)
                mc64_mov(m, 1, xreg[home], ophw[k]);
            else
                mc64_strx(m, ophw[k], MC64_FP, spill_off(fn, op->temp));
        } else if (home < 0) {
            mc64_strx(m, ophw[k], MC64_FP, spill_off(fn, op->temp));
        }
    }
}

int
arm64_select_func(struct mc64 *m, struct ir_func *fn, int tp,
                  char *err, size_t errlen)
{
    int *lbl;
    int at[ARM64_MAX_ARGS], amem[ARM64_MAX_ARGS], narg = 0, x8_temp = -1;
    char aisf[ARM64_MAX_ARGS], ai64[ARM64_MAX_ARGS], af32[ARM64_MAX_ARGS];
    char *is_float;
    unsigned char *f32fmt;
    int rc = 0;

    is_float = calloc((size_t)(fn->ntemps > 0 ? fn->ntemps : 1), 1);
    f32fmt = calloc((size_t)(fn->ntemps > 0 ? fn->ntemps : 1), 1);
    for (struct ir_insn *in = fn->head; in; in = in->next)
        if (in->dst >= 0 && in->dst < fn->ntemps)
            is_float[in->dst] = (char)ir_op_is_float_def(in->op);
    ir_classify_f32(fn, f32fmt);
    g_f32 = f32fmt;

    lbl = malloc((size_t)(fn->nlabels > 0 ? fn->nlabels : 1) * sizeof(int));
    for (int k = 0; k < fn->nlabels; k++)
        lbl[k] = -1;

    prologue(m, fn);

    for (struct ir_insn *in = fn->head; in && rc == 0; in = in->next) {
        int cc = cc_of(in->op);
        if (cc >= 0) {
            int c32 = is_cmp32(in->op);
            int sa = c32 ? rs(m, fn, in->a, SC0) : rsx(m, fn, in->a, SC0);
            int sb = c32 ? rs(m, fn, in->b, SC1) : rsx(m, fn, in->b, SC1);
            int sd = rd(fn, in->dst, SC0);
            mc64_cmp(m, c32 ? 0 : 1, sa, sb);
            mc64_cset(m, 0, sd, cc);
            wd32(m, fn, in->dst, sd);
            continue;
        }
        switch (in->op) {
        case IR_LOC:
            mc64_loc(m, (int)in->imm);
            break;
        case IR_NOP:
        case IR_MARK:
        case IR_FUNC:
        case IR_ENDF:
            break;
        case IR_LABEL:
            if (in->label >= 0)
                mc64_label(m, il(m, lbl, in->label));
            break;
        case IR_LIC: {
            int sd = rd(fn, in->dst, SC0);
            mc64_movimm(m, 0, sd, (uint64_t)(uint32_t)in->imm);
            wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_LIC64: {
            int sd = rd(fn, in->dst, SC0);
            mc64_movimm(m, 1, sd, (uint64_t)in->imm);
            wd64(m, fn, in->dst, sd);
            break;
        }
        case IR_LEA: {
            int sd = rd(fn, in->dst, SC0);
            mc64_lea_sym(m, 0, sd, in->sym);
            wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_LABADDR: {
            int sd = rd(fn, in->dst, SC0);
            mc64_lea_label(m, sd, il(m, lbl, in->label));
            wd32(m, fn, in->dst, sd);   /* ILP32: a 32-bit code address */
            break;
        }
        case IR_LABADDR64: {
            /* LP64 (skj-cc): a label address is a full 64-bit pointer, so the
               result spills at 64-bit width (the adr already writes the whole
               x-register). */
            int sd = rd(fn, in->dst, SC0);
            mc64_lea_label(m, sd, il(m, lbl, in->label));
            wd64(m, fn, in->dst, sd);
            break;
        }
        case IR_LEA64: {
            int sd = rd(fn, in->dst, SC0);
            mc64_lea_sym(m, 1, sd, in->sym);
            wd64(m, fn, in->dst, sd);
            break;
        }
        case IR_ADL: {
            int sd = rd(fn, in->dst, SC0);
            mc64_addimm(m, sd, MC64_FP, slot_offset(fn, in->slot));
            wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_ADL64: {
            int sd = rd(fn, in->dst, SC0);
            mc64_addimm(m, sd, MC64_FP, slot_offset(fn, in->slot));
            wd64(m, fn, in->dst, sd);
            break;
        }
        case IR_MOV:
            if (is_float[in->dst]) {
                int sa = frs(m, fn, in->a, FSC0, 0);
                int sd = frd(fn, in->dst, FSC0);
                if (sa != sd)
                    mc64_fmov(m, 3, sd, sa);
                fwd(m, fn, in->dst, sd, 0);
            } else {
                int sa = rs(m, fn, in->a, SC0);
                int sd = rd(fn, in->dst, SC0);
                if (sa != sd)
                    mc64_mov(m, 0, sd, sa);
                wd32(m, fn, in->dst, sd);
            }
            break;
        case IR_LB:  { int b = ra(m, fn, in->a, SC0), d = rd(fn, in->dst, SC0); mc64_ldrb(m, d, b, 0);  wd32(m, fn, in->dst, d); break; }
        case IR_LBS: { int b = ra(m, fn, in->a, SC0), d = rd(fn, in->dst, SC0); mc64_ldrsb(m, d, b, 0); wd32(m, fn, in->dst, d); break; }
        case IR_LH:  { int b = ra(m, fn, in->a, SC0), d = rd(fn, in->dst, SC0); mc64_ldrh(m, d, b, 0);  wd32(m, fn, in->dst, d); break; }
        case IR_LHS: { int b = ra(m, fn, in->a, SC0), d = rd(fn, in->dst, SC0); mc64_ldrsh(m, d, b, 0); wd32(m, fn, in->dst, d); break; }
        case IR_LW:  { int b = ra(m, fn, in->a, SC0), d = rd(fn, in->dst, SC1); mc64_ldrw(m, d, b, 0);  wd32(m, fn, in->dst, d); break; }
        case IR_LD64:{ int b = ra(m, fn, in->a, SC0), d = rd(fn, in->dst, SC1); mc64_ldrx(m, d, b, 0);  wd64(m, fn, in->dst, d); break; }
        case IR_SB:  { int b = ra(m, fn, in->a, SC0), s = rs(m, fn, in->b, SC1); mc64_strb(m, s, b, 0); break; }
        case IR_SH:  { int b = ra(m, fn, in->a, SC0), s = rs(m, fn, in->b, SC1); mc64_strh(m, s, b, 0); break; }
        case IR_SW:  { int b = ra(m, fn, in->a, SC0), s = rs(m, fn, in->b, SC1); mc64_strw(m, s, b, 0); break; }
        case IR_ST64:{ int b = ra(m, fn, in->a, SC0), s = rsx(m, fn, in->b, SC1); mc64_strx(m, s, b, 0); break; }
        case IR_LDL: {
            int sd = rd(fn, in->dst, SC0);
            mc64_ldrw(m, sd, MC64_FP, slot_offset(fn, in->slot));
            wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_STL: {
            int sa = rs(m, fn, in->a, SC0);
            mc64_strw(m, sa, MC64_FP, slot_offset(fn, in->slot));
            break;
        }
        case IR_LDL64: {
            int sd = rd(fn, in->dst, SC0);
            mc64_ldrx(m, sd, MC64_FP, slot_offset(fn, in->slot));
            wd64(m, fn, in->dst, sd);
            break;
        }
        case IR_STL64: {
            int sa = rsx(m, fn, in->a, SC0);
            mc64_strx(m, sa, MC64_FP, slot_offset(fn, in->slot));
            break;
        }
        case IR_ADD: case IR_SUB: case IR_MUL:
        case IR_AND: case IR_OR: case IR_XOR:
        case IR_ADD64: case IR_SUB64: case IR_MUL64:
        case IR_AND64: case IR_OR64: case IR_XOR64: {
            int w64 = in->op >= IR_ADD64;
            int sa = w64 ? rsx(m, fn, in->a, SC0) : rs(m, fn, in->a, SC0);
            int sb = w64 ? rsx(m, fn, in->b, SC1) : rs(m, fn, in->b, SC1);
            int sd = rd(fn, in->dst, SC0);
            int sf = w64 ? 1 : 0;
            switch (in->op) {
            case IR_ADD: case IR_ADD64: mc64_add(m, sf, sd, sa, sb); break;
            case IR_SUB: case IR_SUB64: mc64_sub(m, sf, sd, sa, sb); break;
            case IR_MUL: case IR_MUL64: mc64_mul(m, sf, sd, sa, sb); break;
            case IR_AND: case IR_AND64: mc64_and(m, sf, sd, sa, sb); break;
            case IR_OR:  case IR_OR64:  mc64_orr(m, sf, sd, sa, sb); break;
            case IR_XOR: case IR_XOR64: mc64_eor(m, sf, sd, sa, sb); break;
            }
            if (w64) wd64(m, fn, in->dst, sd); else wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_ADDO: case IR_SUBO: case IR_MULO: {
            /* Overflow-checked 32-bit signed arithmetic.  AArch64 exposes no
               flag-setting add/sub in this encoder, and multiply has no overflow
               flag at all, so the operands are sign-extended to 64 bits and the
               operation runs 64-bit (exact for any i32 inputs).  The i32 result
               overflows exactly when it differs from its own low-32 sign
               extension.  Under TRAP_HARDWARE the check is dropped (the C front
               end never emits these), leaving the bare arithmetic. */
            int sa = rs(m, fn, in->a, SC0);
            int sb = rs(m, fn, in->b, SC1);
            int sd = rd(fn, in->dst, SC0);
            mc64_sxtw(m, SC0, sa);
            mc64_sxtw(m, SC1, sb);
            switch (in->op) {
            case IR_ADDO: mc64_add(m, 1, SC0, SC0, SC1); break;
            case IR_SUBO: mc64_sub(m, 1, SC0, SC0, SC1); break;
            case IR_MULO: mc64_mul(m, 1, SC0, SC0, SC1); break;
            }
            if (in->label >= 0 || tp == ARM64_TRAP_GUARDED) {
                mc64_sxtw(m, SCQ, SC0);          /* sign-extend low 32 of result */
                mc64_cmp(m, 1, SC0, SCQ);
                mc64_cset(m, 0, SCQ, MC64_NE);   /* SCQ = 1 on overflow */
                if (in->label >= 0) {
                    /* explicit per-site trap (Excelsior's cold __exc_trap
                       trampoline): branch there on overflow. */
                    mc64_cbnz(m, 0, SCQ, il(m, lbl, in->label));
                } else {
                    int ok = mc64_new_label(m);
                    mc64_cbz(m, 0, SCQ, ok);     /* no overflow: skip the trap */
                    mc64_trap_overflow(m);
                    mc64_label(m, ok);
                }
            }
            mc64_mov(m, 0, sd, SC0);
            wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_SHL: case IR_SHRS: case IR_SHRU:
        case IR_SHL64: case IR_SHRS64: case IR_SHRU64: {
            int w64 = in->op == IR_SHL64 || in->op == IR_SHRS64 || in->op == IR_SHRU64;
            int sa = w64 ? rsx(m, fn, in->a, SC0) : rs(m, fn, in->a, SC0);
            int sb = w64 ? rsx(m, fn, in->b, SC1) : rs(m, fn, in->b, SC1);
            int sd = rd(fn, in->dst, SC0);
            int sf = w64 ? 1 : 0;
            switch (in->op) {
            case IR_SHL: case IR_SHL64:   mc64_lsl(m, sf, sd, sa, sb); break;
            case IR_SHRS: case IR_SHRS64: mc64_asr(m, sf, sd, sa, sb); break;
            case IR_SHRU: case IR_SHRU64: mc64_lsr(m, sf, sd, sa, sb); break;
            }
            if (w64) wd64(m, fn, in->dst, sd); else wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_DIVS: case IR_DIVU:
        case IR_DIVS64: case IR_DIVU64: {
            int w64 = in->op == IR_DIVS64 || in->op == IR_DIVU64;
            int uns = in->op == IR_DIVU || in->op == IR_DIVU64;
            int sf = w64 ? 1 : 0;
            int sa = w64 ? rsx(m, fn, in->a, SC0) : rs(m, fn, in->a, SC0);
            int sb = w64 ? rsx(m, fn, in->b, SC1) : rs(m, fn, in->b, SC1);
            int sd = rd(fn, in->dst, SC0);
            div_guard(m, tp, sf, sb);
            if (!uns && !w64) div_overflow_guard(m, tp, sa, sb);
            if (uns) mc64_udiv(m, sf, sd, sa, sb); else mc64_sdiv(m, sf, sd, sa, sb);
            if (w64) wd64(m, fn, in->dst, sd); else wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_MODS: case IR_MODU:
        case IR_MODS64: case IR_MODU64: {
            int w64 = in->op == IR_MODS64 || in->op == IR_MODU64;
            int uns = in->op == IR_MODU || in->op == IR_MODU64;
            int sf = w64 ? 1 : 0;
            int sa = w64 ? rsx(m, fn, in->a, SC0) : rs(m, fn, in->a, SC0);
            int sb = w64 ? rsx(m, fn, in->b, SC1) : rs(m, fn, in->b, SC1);
            int sd = rd(fn, in->dst, SC0);
            div_guard(m, tp, sf, sb);
            if (!uns && !w64) div_overflow_guard(m, tp, sa, sb);
            if (uns) mc64_udiv(m, sf, SCQ, sa, sb); else mc64_sdiv(m, sf, SCQ, sa, sb);
            mc64_msub(m, sf, sd, SCQ, sb, sa);      /* sd = sa - (sa/sb)*sb */
            if (w64) wd64(m, fn, in->dst, sd); else wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_NEG: case IR_NEG64: {
            int w64 = in->op == IR_NEG64;
            int sa = w64 ? rsx(m, fn, in->a, SC0) : rs(m, fn, in->a, SC0);
            int sd = rd(fn, in->dst, SC0);
            mc64_neg(m, w64 ? 1 : 0, sd, sa);
            if (w64) wd64(m, fn, in->dst, sd); else wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_NOT: {
            int sa = rs(m, fn, in->a, SC0);
            int sd = rd(fn, in->dst, SC0);
            mc64_mvn(m, 0, sd, sa);
            wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_SEXT64: {
            int sa = rs(m, fn, in->a, SC0);
            int sd = rd(fn, in->dst, SC0);
            mc64_sxtw(m, sd, sa);
            wd64(m, fn, in->dst, sd);
            break;
        }
        case IR_ZEXT64:
        case IR_TRUNC64: {
            /* a w-move zeroes the upper half (zext); truncation keeps the low
               32 bits.  Both are a 32-bit mov reading a into the destination. */
            int sa = rs(m, fn, in->a, SC0);
            int sd = rd(fn, in->dst, SC0);
            mc64_mov(m, 0, sd, sa);
            if (in->op == IR_ZEXT64) wd64(m, fn, in->dst, sd);
            else wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_JMP:
            mc64_b(m, il(m, lbl, in->label));
            break;
        case IR_IJMP: {
            int sa = rs(m, fn, in->a, SC0);    /* ILP32: 32-bit address operand */
            mc64_br(m, sa);
            break;
        }
        case IR_IJMP64: {
            /* LP64 (skj-cc): the label-address operand is a full 64-bit pointer,
               so it reloads at 64-bit width (rsx) rather than zero-extended. */
            int sa = rsx(m, fn, in->a, SC0);
            mc64_br(m, sa);
            break;
        }
        case IR_BZ: {
            int sa = rs(m, fn, in->a, SC0);
            mc64_cbz(m, 0, sa, il(m, lbl, in->label));
            break;
        }
        case IR_BNZ: {
            int sa = rs(m, fn, in->a, SC0);
            mc64_cbnz(m, 0, sa, il(m, lbl, in->label));
            break;
        }
        case IR_ARG: case IR_FARG: case IR_ARG64: case IR_ARG_MEM:
            if (narg < ARM64_MAX_ARGS) {
                at[narg] = in->a;
                aisf[narg] = (char)(in->op == IR_FARG);
                ai64[narg] = (char)(in->op == IR_ARG64);
                af32[narg] = (char)(in->op == IR_FARG && in->imm == FWIDTH_F32);
                amem[narg] = in->op == IR_ARG_MEM ? (int)in->imm : 0;
                narg++;
            }
            break;
        case IR_ARG_X8:
            x8_temp = in->a;
            break;
        case IR_CALL:
            emit_call(m, fn, in, at, aisf, ai64, af32, amem, narg, x8_temp, 0, RET_I32);
            narg = 0; x8_temp = -1;
            break;
        case IR_CALL64:
            emit_call(m, fn, in, at, aisf, ai64, af32, amem, narg, x8_temp, 0, RET_I64);
            narg = 0; x8_temp = -1;
            break;
        case IR_FCALL:
            emit_call(m, fn, in, at, aisf, ai64, af32, amem, narg, x8_temp, 0, RET_FLOAT);
            narg = 0; x8_temp = -1;
            break;
        case IR_CALLI:
            emit_call(m, fn, in, at, aisf, ai64, af32, amem, narg, x8_temp, 1, RET_I32);
            narg = 0; x8_temp = -1;
            break;
        case IR_CALL2:
            emit_call(m, fn, in, at, aisf, ai64, af32, amem, narg, x8_temp, 0, RET_FAIL);
            narg = 0; x8_temp = -1;
            break;
        case IR_CALLI2:
            emit_call(m, fn, in, at, aisf, ai64, af32, amem, narg, x8_temp, 1, RET_FAIL);
            narg = 0; x8_temp = -1;
            break;
        case IR_CALLI64:
            emit_call(m, fn, in, at, aisf, ai64, af32, amem, narg, x8_temp, 1, RET_I64);
            narg = 0; x8_temp = -1;
            break;
        case IR_FCALLI:
            emit_call(m, fn, in, at, aisf, ai64, af32, amem, narg, x8_temp, 1, RET_FLOAT);
            narg = 0; x8_temp = -1;
            break;
        case IR_CALL_AGG: case IR_CALLI_AGG: {
            int neb = (int)(in->imm & 7);
            int stride = (((int)(in->imm >> 3) & 3) == 2) ? 4 : 8;
            int j, ii = 0, ff = 0, base;
            emit_call(m, fn, in, at, aisf, ai64, af32, amem, narg, x8_temp,
                      in->op == IR_CALLI_AGG, RET_I64);
            narg = 0; x8_temp = -1;
            base = slot_offset(fn, in->slot);
            for (j = 0; j < neb; j++) {
                int c = (int)(in->imm >> (3 + 2 * j)) & 3;
                if (c == 2)      mc64_str_fp(m, 2, ff++, MC64_FP, base + stride * j);
                else if (c == 1) mc64_str_fp(m, 3, ff++, MC64_FP, base + stride * j);
                else             mc64_strx(m, ii++, MC64_FP, base + stride * j);
            }
            break;
        }
        case IR_FLD: {
            int f32 = in->imm == FWIDTH_F32;
            int b = ra(m, fn, in->a, SC0), d = frd(fn, in->dst, FSC0);
            mc64_ldr_fp(m, f32 ? 2 : 3, d, b, 0);
            fwd(m, fn, in->dst, d, f32);
            break;
        }
        case IR_FSD: {
            int f32 = in->imm == FWIDTH_F32;
            int b = ra(m, fn, in->a, SC0), s = frs(m, fn, in->b, FSC0, f32);
            mc64_str_fp(m, f32 ? 2 : 3, s, b, 0);
            break;
        }
        case IR_FLS: {
            int b = ra(m, fn, in->a, SC0), d = frd(fn, in->dst, FSC0);
            mc64_ldr_fp(m, 2, FSC0, b, 0);
            mc64_fcvt(m, 3, 2, d, FSC0);
            fwd(m, fn, in->dst, d, 0);
            break;
        }
        case IR_FSS: {
            int b = ra(m, fn, in->a, SC0), s = frs(m, fn, in->b, FSC1, 0);
            mc64_fcvt(m, 2, 3, FSC0, s);
            mc64_str_fp(m, 2, FSC0, b, 0);
            break;
        }
        case IR_FLH: {
            int b = ra(m, fn, in->a, SC0), d = frd(fn, in->dst, FSC0);
            mc64_ldr_fp(m, 1, FSC0, b, 0);
            mc64_fcvt(m, 3, 1, d, FSC0);
            fwd(m, fn, in->dst, d, 0);
            break;
        }
        case IR_FSH: {
            int b = ra(m, fn, in->a, SC0), s = frs(m, fn, in->b, FSC1, 0);
            mc64_fcvt(m, 1, 3, FSC0, s);
            mc64_str_fp(m, 1, FSC0, b, 0);
            break;
        }
        case IR_FLDL: {
            int f32 = in->imm == FWIDTH_F32;
            int d = frd(fn, in->dst, FSC0);
            mc64_ldr_fp(m, f32 ? 2 : 3, d, MC64_FP, slot_offset(fn, in->slot));
            fwd(m, fn, in->dst, d, f32);
            break;
        }
        case IR_FSTL: {
            int f32 = in->imm == FWIDTH_F32;
            int s = frs(m, fn, in->a, FSC0, f32);
            mc64_str_fp(m, f32 ? 2 : 3, s, MC64_FP, slot_offset(fn, in->slot));
            break;
        }
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
            int f32 = in->imm == FWIDTH_F32, sz = f32 ? 2 : 3;
            int sa = frs(m, fn, in->a, FSC0, f32);
            int sb = frs(m, fn, in->b, FSC1, f32);
            int sd = frd(fn, in->dst, FSC0);
            switch (in->op) {
            case IR_FADD: mc64_fadd(m, sz, sd, sa, sb); break;
            case IR_FSUB: mc64_fsub(m, sz, sd, sa, sb); break;
            case IR_FMUL: mc64_fmul(m, sz, sd, sa, sb); break;
            case IR_FDIV: mc64_fdiv(m, sz, sd, sa, sb); break;
            }
            fwd(m, fn, in->dst, sd, f32);
            break;
        }
        case IR_FNEG: case IR_FABS: {
            int f32 = in->imm == FWIDTH_F32, sz = f32 ? 2 : 3;
            int sa = frs(m, fn, in->a, FSC0, f32);
            int sd = frd(fn, in->dst, FSC0);
            if (in->op == IR_FNEG) mc64_fneg(m, sz, sd, sa); else mc64_fabs(m, sz, sd, sa);
            fwd(m, fn, in->dst, sd, f32);
            break;
        }
        case IR_FSQRT: {
            int f32 = in->imm == FWIDTH_F32, sz = f32 ? 2 : 3;
            int sa = frs(m, fn, in->a, FSC0, f32);
            int sd = frd(fn, in->dst, FSC0);
            mc64_fsqrt(m, sz, sd, sa);
            fwd(m, fn, in->dst, sd, f32);
            break;
        }
        case IR_FCMPEQ: case IR_FCMPLT: case IR_FCMPLE: {
            int f32 = in->imm == FWIDTH_F32, sz = f32 ? 2 : 3;
            int sa = frs(m, fn, in->a, FSC0, f32);
            int sb = frs(m, fn, in->b, FSC1, f32);
            int sd = rd(fn, in->dst, SC0);
            int cond = in->op == IR_FCMPEQ ? MC64_EQ
                     : in->op == IR_FCMPLT ? MC64_MI : MC64_LS;
            mc64_fcmp(m, sz, sa, sb);
            mc64_cset(m, 0, sd, cond);
            wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_ITOF: {
            int f32 = in->imm == FWIDTH_F32;
            int sa = rs(m, fn, in->a, SC0);
            int sd = frd(fn, in->dst, FSC0);
            mc64_scvtf(m, f32 ? 2 : 3, sd, sa);
            fwd(m, fn, in->dst, sd, f32);
            break;
        }
        case IR_FTOI: {
            int f32 = in->imm == FWIDTH_F32;
            int sa = frs(m, fn, in->a, FSC0, f32);
            int sd = rd(fn, in->dst, SC0);
            mc64_fcvtzs(m, f32 ? 2 : 3, sd, sa);
            wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_FROUND: {
            int f32 = in->imm == FWIDTH_F32;
            int sa = frs(m, fn, in->a, FSC0, f32);
            int sd = rd(fn, in->dst, SC0);
            mc64_fcvtns(m, f32 ? 2 : 3, sd, sa);
            wd32(m, fn, in->dst, sd);
            break;
        }
        case IR_F32TOF64: {
            int sa = frs(m, fn, in->a, FSC0, 1);
            int sd = frd(fn, in->dst, FSC0);
            mc64_fcvt(m, 3, 2, sd, sa);
            fwd(m, fn, in->dst, sd, 0);
            break;
        }
        case IR_F64TOF32: {
            int sa = frs(m, fn, in->a, FSC0, 0);
            int sd = frd(fn, in->dst, FSC0);
            mc64_fcvt(m, 2, 3, sd, sa);
            fwd(m, fn, in->dst, sd, 1);
            break;
        }
        case IR_ASM:
            if (in->asm_info) {
                emit_asm_ext(m, fn, in);
                break;
            }
            mc64_asm(m, in->sym);
            break;
        case IR_RET:
            epilogue(m, fn);
            break;
        case IR_RETV: {
            int sa = rs(m, fn, in->a, SC0);
            if (sa != 0)
                mc64_mov(m, 0, 0, sa);
            epilogue(m, fn);
            break;
        }
        case IR_RETV2: {
            /* failable return: value in x0, error in x1.  Materialise each through a
               distinct scratch so the second load cannot clobber the first; neither
               scratch is x0/x1, so the two moves are independent. */
            int sa = rs(m, fn, in->a, SC0);
            int sb = rs(m, fn, in->b, SC1);
            if (sa != 0)
                mc64_mov(m, 0, 0, sa);
            if (sb != 1)
                mc64_mov(m, 0, 1, sb);
            epilogue(m, fn);
            break;
        }
        case IR_RETV64: {
            int sa = rsx(m, fn, in->a, SC0);
            if (sa != 0)
                mc64_mov(m, 1, 0, sa);
            epilogue(m, fn);
            break;
        }
        case IR_FRETV: {
            int sa = frs(m, fn, in->a, FSC0, 0);
            if (sa != 0)
                mc64_fmov(m, 3, 0, sa);
            /* a single result is narrowed to f32 in d0 only when its temp holds
               f64 format (Kobold); a temp already in f32 format (C) returns as-is */
            if (in->imm == FWIDTH_F32 && !src_is_f32(in->a))
                mc64_fcvt(m, 2, 3, 0, 0);
            epilogue(m, fn);
            break;
        }
        case IR_RETV_AGG: {
            int j, ii = 0, ff = 0;
            int stride = (fn->ret_cls[0] == 2) ? 4 : 8;
            int ab = ra(m, fn, in->a, SC0);
            for (j = 0; j < fn->ret_neb; j++) {
                if (fn->ret_cls[j] == 2)      mc64_ldr_fp(m, 2, ff++, ab, stride * j);
                else if (fn->ret_cls[j] == 1) mc64_ldr_fp(m, 3, ff++, ab, stride * j);
                else                          mc64_ldrx(m, ii++, ab, stride * j);
            }
            epilogue(m, fn);
            break;
        }
        case IR_VA_START: {
            /* fill the AAPCS64 va_list at [in->a]:
                 +0 __stack, +8 __gr_top, +16 __vr_top, +24 __gr_offs, +28 __vr_offs */
            int ng = 0, nf = 0, ns = 0, k, j, fr, ap;
            for (k = 0; k < fn->nparams; k++) {
                struct aapcs_ploc p;
                aapcs_param(fn, k, &p);
                if (p.cls[0] == -3)
                    continue;
                if (!p.is_reg) { ns += p.hwords; continue; }
                for (j = 0; j < p.neb; j++) {
                    if (p.cls[j] == 1 || p.cls[j] == 2) nf++; else ng++;
                }
            }
            fr = frame_reserve(fn);
            ap = ra(m, fn, in->a, SC1);          /* va_list address in x10 or reg */
            mc64_addimm(m, SC0, MC64_FP, 16 + 8 * ns);   /* __stack */
            mc64_strx(m, SC0, ap, 0);
            mc64_addimm(m, SC0, MC64_FP, -fr + 64);       /* __gr_top */
            mc64_strx(m, SC0, ap, 8);
            mc64_addimm(m, SC0, MC64_FP, -fr + 64 + 128); /* __vr_top */
            mc64_strx(m, SC0, ap, 16);
            mc64_movimm(m, 0, SC0, (uint64_t)(uint32_t)(-((8 - ng) * 8)));
            mc64_strw(m, SC0, ap, 24);            /* __gr_offs */
            mc64_movimm(m, 0, SC0, (uint64_t)(uint32_t)(-((8 - nf) * 16)));
            mc64_strw(m, SC0, ap, 28);            /* __vr_offs */
            break;
        }
        default:
            snprintf(err, errlen, "unsupported opcode %d in '%s'", in->op, fn->name);
            rc = -1;
            break;
        }
    }

    free(lbl);
    free(is_float);
    g_f32 = NULL;
    free(f32fmt);
    return rc;
}
