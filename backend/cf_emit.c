/* cf_emit.c : ColdFire / m68k back-end, emits GAS-syntax assembly */
/*
 * Instruction selection is direct: one IR op -> a short burst of m68k
 * instructions.  Integer temps live in d2..d7, float temps in fp2..fp7
 * (after regalloc); spilled temps reload through scratch registers
 * (d0/d1 for int, fp0/fp1 for float).  Address temps go through a0/a1.
 *
 * SysV m68k calling convention subset:
 *   - Args pushed right-to-left on the stack, 32 bits each; caller pops.
 *   - Return value in d0.
 *   - Callee-save: d2..d7, a2..a5, fp (a6), fp2..fp7.
 *   - Scratch:     d0, d1, a0, a1, fp0, fp1.
 *
 * Frame layout:
 *
 *      +8 + 4*i (%fp)   param i
 *      +4       (%fp)   return address
 *      +0       (%fp)   saved fp
 *      -locals_size     bottom of locals
 *                       integer spill slots (4 bytes each)
 *                       float spill slots (8 bytes each)
 *                       saved d2..d7 (24 bytes, always)
 *                       saved fp2..fp7 (48 bytes, if function uses floats)
 *                %sp -> bottom of save area
 */

#include "ir.h"

#include <stdio.h>
#include <string.h>

static const char *dregs[] = {
    "%d0", "%d1", "%d2", "%d3", "%d4", "%d5", "%d6", "%d7",
};

static const char *fpregs[] = {
    "%fp0", "%fp1", "%fp2", "%fp3", "%fp4", "%fp5", "%fp6", "%fp7",
};

/* A single-precision argument's stack footprint under the toolkit convention.
   The two front ends that pass single-float arguments size the matching
   parameter slot differently, and since each compiles as its own tool the
   stride is a build-time choice.  The C compiler (CC_M68K) gives a single-float
   parameter an 8-byte doubleword home (cc/lower.c, its cross-target o32 rule),
   so its caller pushes 8.  The Kobold AOT compiler sizes a single parameter slot
   at one 4-byte word (it sets param_fw and a 4-byte slot), so its caller pushes
   4.  Pushing the wrong stride lands the next argument mid-slot. */
#if defined(CC_M68K)
#define CF_SINGLE_ARG_BYTES 8
#else
#define CF_SINGLE_ARG_BYTES 4
#endif

/* per-temp "defined by a 64-bit op" map, built per function (emit_function).
   The i32/i64 accessors below read it to bridge a mixed-width operand: an i64
   temp used in an i32 context is narrowed to its low word, and an i32 temp used
   as an i64 is widened with a sign-extended high word.  The KPM IR mixes the two
   freely (e.g. the coroutine seed store ST64s a plain i32), which the one-register
   i64 backends get for free; ColdFire's register-pair model must bridge it. */
static int *temp_is_i64;

/* forward declarations: the i32/i64 accessors below reference helpers defined
   further down (the register-pair math and the constant-count shift). */
static int i64_hi_reg(int pair);
static int i64_lo_reg(int pair);
static void emit_shift_imm(FILE *out, const char *mnemonic,
                           const char *reg, int n);

/****************************************************************
 * Frame layout helpers
 ****************************************************************/

static int
locals_size(struct ir_func *fn)
{
    int i, s;

    s = 0;
    for (i = fn->nparams; i < fn->nslots; i++) {
        int sz = (fn->slot_size[i] + 3) & ~3;
        s += sz;
    }
    return s;
}

static int
slot_offset(struct ir_func *fn, int slot)
{
    int i, off;

    if (slot < fn->nparams) {
        off = 8;
        for (i = 0; i < slot; i++) {
            int sz = (fn->slot_size[i] + 3) & ~3;
            off += sz;
        }
        return off;
    }
    off = 0;
    for (i = fn->nparams; i <= slot; i++) {
        int sz = (fn->slot_size[i] + 3) & ~3;
        off += sz;
    }
    return -off;
}

static int
spill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - (fn->temp_spill[temp] + 4);
}

static int
fspill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - fn->nspills * 4 - (fn->temp_spill[temp] + 8);
}

static int
i64spill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - fn->nspills * 4
           - fn->nfspills * 8 - (fn->temp_spill[temp] + 8);
}

static int
func_uses_fpregs(struct ir_func *fn)
{
    struct ir_insn *i;

    for (i = fn->head; i; i = i->next) {
        if (ir_op_is_float_def(i->op))
            return 1;
    }
    return 0;
}

/****************************************************************
 * Temp -> register materialisation
 ****************************************************************/

static const char *
rs(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (temp_is_i64 && temp_is_i64[t]) {
        /* An i64 temp used in an i32 context is narrowed to its low word.  The
           pair's low register, or (big-endian) the high address of the i64
           spill cell, which i64_rs loads from off - 4. */
        if (r >= 0)
            return dregs[i64_lo_reg(r)];
        fprintf(out, "\tmove.l %d(%%fp), %s\n",
            i64spill_byte_offset(fn, t) - 4, dregs[scratch]);
        return dregs[scratch];
    }
    if (r >= 0)
        return dregs[r];
    fprintf(out, "\tmove.l %d(%%fp), %s\n",
        spill_byte_offset(fn, t), dregs[scratch]);
    return dregs[scratch];
}

static const char *
rd(struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return dregs[r];
    return dregs[scratch];
}

static void
wd(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tmove.l %s, %d(%%fp)\n",
        reg, spill_byte_offset(fn, t));
}

static const char *
frs(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return fpregs[r];
    fprintf(out, "\tfmove.d %d(%%fp), %s\n",
        fspill_byte_offset(fn, t), fpregs[scratch]);
    return fpregs[scratch];
}

static const char *
frd(struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return fpregs[r];
    return fpregs[scratch];
}

static void
fwd(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tfmove.d %s, %d(%%fp)\n",
        reg, fspill_byte_offset(fn, t));
}

/****************************************************************
 * I64 register-pair helpers
 *
 * Pair index P maps to physical registers:
 *   hi = d(6 - 2*P), lo = d(7 - 2*P)
 * Pair 0 = (d6,d7), Pair 1 = (d4,d5), Pair 2 = (d2,d3)
 *
 * Scratch for spill reload: d0 (hi), d1 (lo)
 ****************************************************************/

static int
i64_hi_reg(int pair)
{
    return 6 - 2 * pair;
}

static int
i64_lo_reg(int pair)
{
    return 7 - 2 * pair;
}

static void
i64_rs(FILE *out, struct ir_func *fn, int t, int *hi, int *lo)
{
    int p = fn->temp_reg[t];

    if (!(temp_is_i64 && temp_is_i64[t])) {
        /* An i32 temp used as an i64 (e.g. the coroutine seed store): its value
           is the low word and the high word is its sign extension.  Materialise
           the low word in d1 and the sign fill in d0. */
        const char *sv = rs(out, fn, t, 1);
        if (strcmp(sv, "%d1") != 0)
            fprintf(out, "\tmove.l %s, %%d1\n", sv);
        fprintf(out, "\tmove.l %%d1, %%d0\n");
        emit_shift_imm(out, "asr.l", "%d0", 31);
        *hi = 0;
        *lo = 1;
        return;
    }
    if (p >= 0) {
        *hi = i64_hi_reg(p);
        *lo = i64_lo_reg(p);
    } else {
        int off = i64spill_byte_offset(fn, t);
        fprintf(out, "\tmove.l %d(%%fp), %%d0\n", off);
        fprintf(out, "\tmove.l %d(%%fp), %%d1\n", off - 4);
        *hi = 0;
        *lo = 1;
    }
}

/* Read one 32-bit word of i64 temp t into a register: the high word when hi is
   nonzero, the low word otherwise.  A pair-allocated temp returns its own
   register (no load); a spilled temp loads the word into `scratch` (big-endian
   spill: hi at off, lo at off - 4).  An i32 temp widened to i64 gives its value
   as the low word and its sign extension (in `scratch`) as the high word.  Used
   by the i64 comparisons, which read both operands' high words, then both low
   words, so loading a word at a time keeps the second operand's load from
   clobbering the first's scratch (the bug a plain two-operand i64_rs has when
   both operands spill into the shared d0/d1 pair). */
static const char *
i64_word(FILE *out, struct ir_func *fn, int t, int hi, int scratch)
{
    int p = fn->temp_reg[t];

    if (temp_is_i64 && temp_is_i64[t]) {
        if (p >= 0)
            return dregs[hi ? i64_hi_reg(p) : i64_lo_reg(p)];
        {
            int off = i64spill_byte_offset(fn, t);
            fprintf(out, "\tmove.l %d(%%fp), %s\n",
                hi ? off : off - 4, dregs[scratch]);
            return dregs[scratch];
        }
    }
    /* i32 widened to i64 */
    if (!hi) {
        if (p >= 0)
            return dregs[p];
        fprintf(out, "\tmove.l %d(%%fp), %s\n",
            spill_byte_offset(fn, t), dregs[scratch]);
        return dregs[scratch];
    }
    if (p >= 0)
        fprintf(out, "\tmove.l %s, %s\n", dregs[p], dregs[scratch]);
    else
        fprintf(out, "\tmove.l %d(%%fp), %s\n",
            spill_byte_offset(fn, t), dregs[scratch]);
    emit_shift_imm(out, "asr.l", dregs[scratch], 31);
    return dregs[scratch];
}

/* Load operand a's and b's high words (hi=1) or low words (hi=0) into
   non-overlapping registers (a through scratch d0, b through scratch d1), for
   the i64 comparisons. */
static void
i64_cmp_words(FILE *out, struct ir_func *fn, int ta, int tb, int hi,
              const char **ra, const char **rb)
{
    *ra = i64_word(out, fn, ta, hi, 0);
    *rb = i64_word(out, fn, tb, hi, 1);
}

static void
i64_rd(struct ir_func *fn, int t, int *hi, int *lo)
{
    int p = fn->temp_reg[t];

    if (p >= 0) {
        *hi = i64_hi_reg(p);
        *lo = i64_lo_reg(p);
    } else {
        *hi = 0;
        *lo = 1;
    }
}

static void
i64_wd(FILE *out, struct ir_func *fn, int t, int hi, int lo)
{
    int off;

    if (fn->temp_reg[t] >= 0)
        return;
    off = i64spill_byte_offset(fn, t);
    fprintf(out, "\tmove.l %s, %d(%%fp)\n", dregs[hi], off);
    fprintf(out, "\tmove.l %s, %d(%%fp)\n", dregs[lo], off - 4);
}

/* If temp t is defined by a load-immediate-constant, return 1 and its value;
   otherwise 0.  Temps are function-unique and written once, so the single
   defining instruction is unambiguous.  Used to inline a constant-count i64
   shift (the shift helpers in soft64.c shift a 64-bit value by a constant, so
   lowering that to a helper call would make each helper call itself). */
static int
i64_const_shift_count(struct ir_func *fn, int t, int *val)
{
    struct ir_insn *i;

    for (i = fn->head; i; i = i->next)
        if (i->dst == t && i->op == IR_LIC) {
            *val = (int)i->imm;
            return 1;
        }
    return 0;
}

/* Emit a constant-count shift on one data register, in chunks of at most 8
   (ColdFire shift-immediate encodes a count of 1..8). */
static void
emit_shift_imm(FILE *out, const char *mnemonic, const char *reg, int n)
{
    while (n > 0) {
        int step = n > 8 ? 8 : n;
        fprintf(out, "\t%s #%d, %s\n", mnemonic, step, reg);
        n -= step;
    }
}

/* Inline a 64-bit shift by a compile-time constant count.  The result is
   computed in the d0:d1 scratch pair (hi:lo), so it may overlap a spilled
   source, then moved to the destination.  For a sub-word count the cross-word
   term needs one more scratch; d2 is borrowed over the stack after the source
   has been copied out. */
static void
emit_i64_shift_const(FILE *out, struct ir_func *fn, struct ir_insn *i,
                     int shi, int slo, int dhi, int dlo, int c)
{
    const char *rsh = (i->op == IR_SHRS64) ? "asr.l" : "lsr.l";

    c &= 63;
    fprintf(out, "\tmove.l %s, %%d0\n", dregs[shi]);
    fprintf(out, "\tmove.l %s, %%d1\n", dregs[slo]);

    if (i->op == IR_SHL64) {
        if (c == 0) {
            /* identity: result already in d0:d1 */
        } else if (c >= 32) {
            fprintf(out, "\tmove.l %%d1, %%d0\n");     /* hi = lo */
            emit_shift_imm(out, "lsl.l", "%d0", c - 32);
            fprintf(out, "\tmoveq #0, %%d1\n");         /* lo = 0 */
        } else {
            fprintf(out, "\tmove.l %%d2, -(%%sp)\n");
            fprintf(out, "\tmove.l %%d1, %%d2\n");       /* d2 = lo */
            emit_shift_imm(out, "lsr.l", "%d2", 32 - c);
            emit_shift_imm(out, "lsl.l", "%d0", c);      /* hi <<= c */
            fprintf(out, "\tor.l %%d2, %%d0\n");          /* hi |= lo>>(32-c) */
            emit_shift_imm(out, "lsl.l", "%d1", c);      /* lo <<= c */
            fprintf(out, "\tmove.l (%%sp)+, %%d2\n");
        }
    } else { /* IR_SHRU64 / IR_SHRS64 */
        if (c == 0) {
            /* identity */
        } else if (c >= 32) {
            fprintf(out, "\tmove.l %%d0, %%d1\n");       /* lo = hi */
            emit_shift_imm(out, rsh, "%d1", c - 32);
            if (i->op == IR_SHRS64)
                emit_shift_imm(out, "asr.l", "%d0", 31); /* hi = sign fill */
            else
                fprintf(out, "\tmoveq #0, %%d0\n");       /* hi = 0 */
        } else {
            fprintf(out, "\tmove.l %%d2, -(%%sp)\n");
            fprintf(out, "\tmove.l %%d0, %%d2\n");       /* d2 = hi */
            emit_shift_imm(out, "lsl.l", "%d2", 32 - c);
            emit_shift_imm(out, "lsr.l", "%d1", c);      /* lo >>= c (logical) */
            fprintf(out, "\tor.l %%d2, %%d1\n");          /* lo |= hi<<(32-c) */
            emit_shift_imm(out, rsh, "%d0", c);          /* hi >>= c */
            fprintf(out, "\tmove.l (%%sp)+, %%d2\n");
        }
    }

    if (dhi != 0)
        fprintf(out, "\tmove.l %%d0, %s\n", dregs[dhi]);
    if (dlo != 1)
        fprintf(out, "\tmove.l %%d1, %s\n", dregs[dlo]);
    i64_wd(out, fn, i->dst, dhi, dlo);
}

/****************************************************************
 * Binary op helper
 ****************************************************************/

static void
emit_binop(FILE *out, struct ir_func *fn, struct ir_insn *i,
           const char *mnem)
{
    const char *sa, *sb, *sd;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    sd = rd(fn, i->dst, 0);
    if (strcmp(sb, sd) == 0 && strcmp(sa, sd) != 0) {
        fprintf(out, "\tmove.l %s, %%d1\n", sb);
        sb = dregs[1];
    }
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmove.l %s, %s\n", sa, sd);
    fprintf(out, "\t%s %s, %s\n", mnem, sb, sd);
    wd(out, fn, i->dst, sd);
}

/* file-scope: the per-function label namespace and a counter for the unique
   local labels the label-less overflow trap needs (declared up here because
   emit_overflow below references them). */
static int label_prefix;
static int uses_floats;
static int ovf_label_ctr;

static void
emit_unop(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *mnem)
{
    const char *sa, *sd;

    sa = rs(out, fn, i->a, 0);
    sd = rd(fn, i->dst, 0);
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmove.l %s, %s\n", sa, sd);
    fprintf(out, "\t%s %s\n", mnem, sd);
    wd(out, fn, i->dst, sd);
}

/* Overflow-checked signed 32-bit arithmetic (IR_ADDO/SUBO/MULO).  add.l,
 * sub.l, and muls.l each set the V flag on signed overflow (the emulator sets
 * it for muls.l too; see emu/coldfire.c), so a bvs after the operation takes
 * the trap.  i->label >= 0 is an explicit per-site trap target (Excelsior's
 * cold __exc_trap trampoline): branch there on overflow.  Otherwise (Kobold,
 * i->label == -1) call the runtime's kp_trap_overflow.  The branch is emitted
 * before wd(), since a move to a spill slot would clear V. */
static void
emit_overflow(FILE *out, struct ir_func *fn, struct ir_insn *i)
{
    const char *mnem = i->op == IR_ADDO ? "add.l" :
                       i->op == IR_SUBO ? "sub.l" : "muls.l";
    const char *sa, *sb, *sd;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    sd = rd(fn, i->dst, 0);
    if (strcmp(sb, sd) == 0 && strcmp(sa, sd) != 0) {
        fprintf(out, "\tmove.l %s, %%d1\n", sb);
        sb = dregs[1];
    }
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmove.l %s, %s\n", sa, sd);
    fprintf(out, "\t%s %s, %s\n", mnem, sb, sd);
    if (i->label >= 0) {
        fprintf(out, "\tbvs .L%d_%d\n", label_prefix, i->label);
    } else {
        int k = ovf_label_ctr++;
        fprintf(out, "\tbvc .Lovfok%d\n", k);
        fprintf(out, "\tjsr kp_trap_overflow\n");
        fprintf(out, ".Lovfok%d:\n", k);
    }
    wd(out, fn, i->dst, sd);
}

static int divz_label_ctr;

/* Under Kobold's guarded trap policy (target_aot_trap_guarded) a zero divisor
 * must raise the clean DIV_ZERO trap (exit 70), not the m68k hardware
 * divide-by-zero exception (which reaches the shell as SIGFPE).  Test the
 * divisor and call kp_trap_div_zero when it is zero.  cc and Excelsior leave
 * the policy clear and keep the bare divide (Excelsior checks a fallible
 * divisor in its own lowering, cc's divide by zero is undefined). */
static void
emit_divzero_guard(FILE *out, const char *divisor)
{
    int k;
    if (!target_aot_trap_guarded)
        return;
    k = divz_label_ctr++;
    fprintf(out, "\ttst.l %s\n", divisor);
    fprintf(out, "\tbne .Ldivok%d\n", k);
    fprintf(out, "\tjsr kp_trap_div_zero\n");
    fprintf(out, ".Ldivok%d:\n", k);
}

/* Under the guarded policy a signed INT_MIN / -1 divide (and its modulo) must
 * trap OVERFLOW: the true quotient 2^31 is not a representable signed 32-bit
 * value.  divs.l's own overflow behavior is not usable here, qemu-m68k raises
 * SIGFPE on the divide itself, so the pair is detected before the divide and
 * kp_trap_overflow is called; the divide runs only when there is no overflow.
 * The constant goes in %a0 (address scratch, never a live data temp) and is
 * compared with cmp.l %a0,Dn, the register form both skj-as and GNU as encode
 * alike (the immediate CMPI form they encode differently).  Signed only;
 * divu.l cannot overflow this way. */
static void
emit_divovf_guard(FILE *out, const char *dividend, const char *divisor,
                  int is_signed)
{
    int k;
    if (!target_aot_trap_guarded || !is_signed)
        return;
    k = divz_label_ctr++;
    fprintf(out, "\tmovea.l #-1, %%a0\n");
    fprintf(out, "\tcmp.l %%a0, %s\n", divisor);
    fprintf(out, "\tbne .Lovfok%d\n", k);          /* divisor != -1: no overflow */
    fprintf(out, "\tmovea.l #-2147483648, %%a0\n");
    fprintf(out, "\tcmp.l %%a0, %s\n", dividend);
    fprintf(out, "\tbne .Lovfok%d\n", k);          /* dividend != INT_MIN: none */
    fprintf(out, "\tjsr kp_trap_overflow\n");
    fprintf(out, ".Lovfok%d:\n", k);
}

/* 32-bit divide quotient (IR_DIVS/IR_DIVU), emit_binop plus the divisor guard.
 * The guard tests sb after any remap to %d1, before sd is overwritten. */
static void
emit_div(FILE *out, struct ir_func *fn, struct ir_insn *i, const char *mnem,
         int is_signed)
{
    const char *sa, *sb, *sd;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    sd = rd(fn, i->dst, 0);
    if (strcmp(sb, sd) == 0 && strcmp(sa, sd) != 0) {
        fprintf(out, "\tmove.l %s, %%d1\n", sb);
        sb = dregs[1];
    }
    emit_divzero_guard(out, sb);
    emit_divovf_guard(out, sa, sb, is_signed);   /* before sa is moved into sd */
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmove.l %s, %s\n", sa, sd);
    fprintf(out, "\t%s %s, %s\n", mnem, sb, sd);
    wd(out, fn, i->dst, sd);
}

static void
emit_cmp(FILE *out, struct ir_func *fn, struct ir_insn *i,
         const char *scc)
{
    const char *sa, *sb, *sd;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    fprintf(out, "\tcmp.l %s, %s\n", sb, sa);
    sd = rd(fn, i->dst, 0);
    fprintf(out, "\t%s %s\n", scc, sd);
    /* scc writes 0x00/0xFF to the low byte; and.l #1 turns that into a
     * clean 0/1 and clears the upper bits, replacing the 68k-only
     * neg.b + extb.l pair (ColdFire has no byte-size neg) */
    fprintf(out, "\tand.l #1, %s\n", sd);
    wd(out, fn, i->dst, sd);
}

static void
emit_fbinop(FILE *out, struct ir_func *fn, struct ir_insn *i,
            const char *mnem)
{
    const char *sa, *sb, *sd;

    sa = frs(out, fn, i->a, 0);
    sb = frs(out, fn, i->b, 1);
    sd = frd(fn, i->dst, 0);
    if (strcmp(sb, sd) == 0 && strcmp(sa, sd) != 0) {
        fprintf(out, "\tfmove.x %s, %%fp1\n", sb);
        sb = fpregs[1];
    }
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tfmove.x %s, %s\n", sa, sd);
    fprintf(out, "\t%s %s, %s\n", mnem, sb, sd);
    fwd(out, fn, i->dst, sd);
}

static void
emit_funop(FILE *out, struct ir_func *fn, struct ir_insn *i,
           const char *mnem)
{
    const char *sa, *sd;

    sa = frs(out, fn, i->a, 0);
    sd = frd(fn, i->dst, 0);
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tfmove.x %s, %s\n", sa, sd);
    fprintf(out, "\t%s %s\n", mnem, sd);
    fwd(out, fn, i->dst, sd);
}

/****************************************************************
 * Per-instruction emission
 ****************************************************************/

/* Maximum arguments in one call. Mirrors the C front end's cap (lower.c:
   "too many arguments"); the two must agree, or a call the front end accepts
   would be rejected here. */
#define CF_MAX_ARGS 32

static int arg_temps[CF_MAX_ARGS];
static int arg_is_float[CF_MAX_ARGS];
static int arg_is_f32[CF_MAX_ARGS];
static int arg_is_i64[CF_MAX_ARGS];
static int narg;
static int fcmp_serial;
static int i64cmp_serial;

static void
emit_load(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *sz, int sign_extend, int clear_first)
{
    const char *sa, *sd;

    sa = rs(out, fn, i->a, 0);
    sd = rd(fn, i->dst, 0);
    fprintf(out, "\tmovea.l %s, %%a0\n", sa);
    if (clear_first)
        fprintf(out, "\tmoveq #0, %s\n", sd);
    fprintf(out, "\tmove.%s (%%a0), %s\n", sz, sd);
    if (sign_extend) {
        if (sz[0] == 'b')
            fprintf(out, "\text.w %s\n", sd);
        fprintf(out, "\text.l %s\n", sd);
    }
    wd(out, fn, i->dst, sd);
}

static void
emit_store(FILE *out, struct ir_func *fn, struct ir_insn *i,
           const char *sz)
{
    const char *sa, *sb;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    fprintf(out, "\tmovea.l %s, %%a0\n", sa);
    fprintf(out, "\tmove.%s %s, (%%a0)\n", sz, sb);
}

static int
frame_size(struct ir_func *fn)
{
    return locals_size(fn) + fn->nspills * 4
           + fn->nfspills * 8 + fn->ni64spills * 8;
}

static void
emit_prologue(FILE *out, struct ir_func *fn)
{
    fprintf(out, "\tlink.w %%fp, #%d\n", -frame_size(fn));
    /* real ColdFire movem has no predecrement mode: reserve then store
     * through (%sp), the same final layout the 68k -(%sp) form produced */
    fprintf(out, "\tlea -24(%%sp), %%sp\n");
    fprintf(out, "\tmovem.l %%d2-%%d7, (%%sp)\n");
    if (uses_floats) {
        fprintf(out, "\tlea -48(%%sp), %%sp\n");
        fprintf(out, "\tfmove.d %%fp2, (%%sp)\n");
        fprintf(out, "\tfmove.d %%fp3, 8(%%sp)\n");
        fprintf(out, "\tfmove.d %%fp4, 16(%%sp)\n");
        fprintf(out, "\tfmove.d %%fp5, 24(%%sp)\n");
        fprintf(out, "\tfmove.d %%fp6, 32(%%sp)\n");
        fprintf(out, "\tfmove.d %%fp7, 40(%%sp)\n");
    }
}

static void
emit_epilogue_no_rts(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);

    if (uses_floats) {
        fprintf(out, "\tlea %d(%%fp), %%sp\n", -(frame + 72));
        fprintf(out, "\tfmove.d (%%sp), %%fp2\n");
        fprintf(out, "\tfmove.d 8(%%sp), %%fp3\n");
        fprintf(out, "\tfmove.d 16(%%sp), %%fp4\n");
        fprintf(out, "\tfmove.d 24(%%sp), %%fp5\n");
        fprintf(out, "\tfmove.d 32(%%sp), %%fp6\n");
        fprintf(out, "\tfmove.d 40(%%sp), %%fp7\n");
        fprintf(out, "\tlea 48(%%sp), %%sp\n");
    } else {
        fprintf(out, "\tlea %d(%%fp), %%sp\n", -(frame + 24));
    }
    /* ColdFire movem: load through (%sp); unlk resets %sp from %fp, so
     * the 24-byte pop the old (%sp)+ form did is not needed */
    fprintf(out, "\tmovem.l (%%sp), %%d2-%%d7\n");
    fprintf(out, "\tunlk %%fp\n");
}

static void
emit_epilogue(FILE *out, struct ir_func *fn)
{
    emit_epilogue_no_rts(out, fn);
    fprintf(out, "\trts\n");
}

static void
emit_call_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                int indirect)
{
    int k;
    int arg_bytes = 0;
    int float_ret = (i->op == IR_FCALL || i->op == IR_FCALLI);

    for (k = narg - 1; k >= 0; k--) {
        if (arg_is_float[k]) {
            const char *sa = frs(out, fn, arg_temps[k], 0);
            if (arg_is_f32[k]) {
                /* A single argument's stack footprint (CF_SINGLE_ARG_BYTES)
                 * must match the callee's single-parameter slot, or the next
                 * argument lands mid-slot.  The single sits at the slot base. */
                fprintf(out, "\tlea -%d(%%sp), %%sp\n", CF_SINGLE_ARG_BYTES);
                fprintf(out, "\tfmove.s %s, (%%sp)\n", sa);
                arg_bytes += CF_SINGLE_ARG_BYTES;
            } else {
                fprintf(out, "\tfmove.d %s, -(%%sp)\n", sa);
                arg_bytes += 8;
            }
        } else if (arg_is_i64[k]) {
            int hi, lo;
            i64_rs(out, fn, arg_temps[k], &hi, &lo);
            fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[lo]);
            fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[hi]);
            arg_bytes += 8;
        } else {
            const char *sa = rs(out, fn, arg_temps[k], 0);
            fprintf(out, "\tmove.l %s, -(%%sp)\n", sa);
            arg_bytes += 4;
        }
    }
    if (indirect) {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tmovea.l %s, %%a0\n", sa);
        fprintf(out, "\tjsr (%%a0)\n");
    } else {
        fprintf(out, "\tjsr %s\n", i->sym);
    }
    if (arg_bytes > 0)
        fprintf(out, "\tlea %d(%%sp), %%sp\n", arg_bytes);
    narg = 0;

    /* A failable call (CALL2/CALLI2) returns the error signal in the second
       return register (d1); store it to the error slot before the value in d0
       is moved to dst, since a spilled dst's fix-up scratch is d1. */
    if (i->op == IR_CALL2 || i->op == IR_CALLI2)
        fprintf(out, "\tmove.l %%d1, %d(%%fp)\n", slot_offset(fn, i->slot));

    if (i->dst >= 0) {
        if (float_ret) {
            const char *sd = frd(fn, i->dst, 0);
            if (strcmp(sd, "%fp0") != 0)
                fprintf(out, "\tfmove.x %%fp0, %s\n", sd);
            fwd(out, fn, i->dst, sd);
        } else if (i->op == IR_CALL64 || i->op == IR_CALLI64) {
            int dhi, dlo;
            i64_rd(fn, i->dst, &dhi, &dlo);
            if (dhi != 0)
                fprintf(out, "\tmove.l %%d0, %s\n", dregs[dhi]);
            if (dlo != 1)
                fprintf(out, "\tmove.l %%d1, %s\n", dregs[dlo]);
            i64_wd(out, fn, i->dst, dhi, dlo);
        } else {
            const char *sd = rd(fn, i->dst, 0);
            if (strcmp(sd, "%d0") != 0)
                fprintf(out, "\tmove.l %%d0, %s\n", sd);
            wd(out, fn, i->dst, sd);
        }
    }
}

static void
emit_tailcall_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                    int indirect)
{
    int k;

    {
        int off = 8;
        for (k = 0; k < narg; k++) {
            if (arg_is_float[k]) {
                const char *sa = frs(out, fn, arg_temps[k], 0);
                fprintf(out, "\tfmove.%c %s, %d(%%fp)\n",
                    arg_is_f32[k] ? 's' : 'd', sa, off);
                off += arg_is_f32[k] ? CF_SINGLE_ARG_BYTES : 8;
            } else {
                const char *sa = rs(out, fn, arg_temps[k], 0);
                fprintf(out, "\tmove.l %s, %d(%%fp)\n", sa, off);
                off += 4;
            }
        }
    }
    if (indirect) {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tmovea.l %s, %%a1\n", sa);
    }
    narg = 0;
    emit_epilogue_no_rts(out, fn);
    if (indirect)
        fprintf(out, "\tjmp (%%a1)\n");
    else
        fprintf(out, "\tjmp %s\n", i->sym);
}

/* Is physical register `name` named in the asm's clobber list? */
static int
asm_reg_clobbered(struct ir_asm_info *ai, const char *name)
{
    int k;
    for (k = 0; k < ai->nclobbers; k++)
        if (strcmp(ai->clobbers[k], name) == 0)
            return 1;
    return 0;
}

/* The dregs[] index of data register `name`, or -1.  Indices 2..7 (%d2..%d7)
   are the allocatable registers; the register allocator reserves any that an
   asm pins or clobbers (cf_asm_reserved_regs below). */
static int
cf_regindex(const char *name)
{
    int t;
    for (t = 0; t < (int)(sizeof dregs / sizeof dregs[0]); t++)
        if (strcmp(dregs[t], name) == 0)
            return t;
    return -1;
}

/* Fill out[] (capacity max) with the allocator indices of the allocatable
   data registers any extended asm in fn pins or clobbers, deduplicated. */
int
cf_asm_reserved_regs(struct ir_func *fn, int *out, int max)
{
    struct ir_insn *i;
    int n = 0, k, j;

    for (i = fn->head; i; i = i->next) {
        struct ir_asm_info *ai = i->asm_info;
        if (i->op != IR_ASM || !ai)
            continue;
        for (k = 0; k < ai->noutputs + ai->ninputs; k++) {
            int idx = ai->ops[k].pin_reg ? cf_regindex(ai->ops[k].pin_reg) : -1;
            if (idx < 2)                /* not allocatable (or not a d-reg) */
                continue;
            for (j = 0; j < n && out[j] != idx; j++)
                ;
            if (j == n && n < max)
                out[n++] = idx;
        }
        for (k = 0; k < ai->nclobbers; k++) {
            int idx = cf_regindex(ai->clobbers[k]);
            if (idx < 2)
                continue;
            for (j = 0; j < n && out[j] != idx; j++)
                ;
            if (j == n && n < max)
                out[n++] = idx;
        }
    }
    return n;
}

/* Is physical register `name` the pin target of any operand in this asm? */
static int
asm_reg_pinned(struct ir_asm_info *ai, const char *name)
{
    int k, n = ai->noutputs + ai->ninputs;
    for (k = 0; k < n; k++)
        if (ai->ops[k].pin_reg && strcmp(ai->ops[k].pin_reg, name) == 0)
            return 1;
    return 0;
}

/* Extended inline asm (cc's asm(template : outputs : inputs : clobbers)).
 * A generic "r" operand goes in a data register: its allocated %d2..%d7, or a
 * %d0/%d1 scratch materialised from the spill slot.  A pinned operand (a
 * "{%reg}" constraint or a GNU local register variable) goes in that physical
 * register, with its value moved in from the operand temp's home and, for an
 * output, moved back out.  The template's %N are substituted.  Register names
 * are the assembler's own spelling, e.g. %d0. */
/* The register class a constraint asks for (the leading =/+/&/% skipped):
   'a' is an address register, 'd'/'r' a data register (the default). */
static int
cf_constraint_class(const char *c)
{
    while (*c == '=' || *c == '+' || *c == '&' || *c == '%')
        c++;
    return *c == 'a' ? 'a' : 'd';
}

static void
emit_asm_ext(FILE *out, struct ir_func *fn, struct ir_insn *i)
{
    struct ir_asm_info *ai = i->asm_info;
    int n = ai->noutputs + ai->ninputs;
    const char *pool[] = { "%d0", "%d1" };          /* data scratch */
    const char *apool[] = { "%a0", "%a1" };         /* address scratch */
    int npool = (int)(sizeof pool / sizeof pool[0]);
    int nap = (int)(sizeof apool / sizeof apool[0]);
    const char *opreg[32];
    int forced[32];     /* 1 = a pin or an address-class operand: move in/out */
    int next = 0, anext = 0, k;
    const char *s;

    if (n > 32)
        die("cf: too many asm operands");

    /* 1. name each operand's register.  A pin goes to the named register; an
       "a"-class operand to an address scratch register; otherwise a data
       register (its allocated %d2..%d7, or a %d0/%d1 scratch assigned next). */
    for (k = 0; k < n; k++) {
        struct ir_asm_op *op = &ai->ops[k];

        if (op->pin_reg) {
            int j;
            /* an allocatable pin is fine: the allocator reserved it (see
               cf_asm_reserved_regs), so no temp lives there across the asm */
            for (j = 0; j < k; j++)
                if (ai->ops[j].pin_reg &&
                    strcmp(ai->ops[j].pin_reg, op->pin_reg) == 0)
                    die("cf: two asm operands pinned to %s", op->pin_reg);
            opreg[k] = op->pin_reg;
            forced[k] = 1;
        } else if (cf_constraint_class(op->constraint) == 'a') {
            if (anext >= nap)
                die("cf: too many address-class asm operands");
            opreg[k] = apool[anext++];
            forced[k] = 1;
        } else if (fn->temp_reg[op->temp] >= 0) {
            opreg[k] = dregs[fn->temp_reg[op->temp]];
            forced[k] = 0;
        } else {
            opreg[k] = NULL;    /* a data pool reg, assigned next */
            forced[k] = 0;
        }
    }
    /* 2. hand the spilled generic operands a scratch register each */
    for (k = 0; k < n; k++) {
        if (opreg[k])
            continue;
        while (next < npool && (asm_reg_clobbered(ai, pool[next]) ||
                                asm_reg_pinned(ai, pool[next])))
            next++;
        if (next >= npool)
            die("cf: too many spilled asm operands");
        opreg[k] = pool[next++];
    }

    /* 3. materialise input values into their operand registers */
    for (k = 0; k < n; k++) {
        struct ir_asm_op *op = &ai->ops[k];
        int home = fn->temp_reg[op->temp];

        if (op->is_output && !op->is_inout)
            continue;
        if (forced[k]) {
            if (home >= 0)
                fprintf(out, "\tmove.l %s, %s\n", dregs[home], opreg[k]);
            else
                fprintf(out, "\tmove.l %d(%%fp), %s\n",
                    spill_byte_offset(fn, op->temp), opreg[k]);
        } else if (home < 0) {
            fprintf(out, "\tmove.l %d(%%fp), %s\n",
                spill_byte_offset(fn, op->temp), opreg[k]);
        }
    }

    /* 4. the template, %N substituted */
    for (s = i->sym; *s; s++) {
        if (*s != '%') {
            fputc(*s, out);
            continue;
        }
        s++;
        if (*s == '%') {
            fputc('%', out);
        } else if (*s >= '0' && *s <= '9') {
            int idx = 0;
            while (*s >= '0' && *s <= '9')
                idx = idx * 10 + (*s++ - '0');
            s--;
            if (idx >= n)
                die("cf: asm operand %%%d out of range", idx);
            fputs(opreg[idx], out);
        } else {
            fputc('%', out);
            fputc(*s, out);
        }
    }
    fputc('\n', out);

    /* 5. store outputs back from their operand registers */
    for (k = 0; k < ai->noutputs; k++) {
        struct ir_asm_op *op = &ai->ops[k];
        int home = fn->temp_reg[op->temp];

        if (forced[k]) {
            if (home >= 0)
                fprintf(out, "\tmove.l %s, %s\n", opreg[k], dregs[home]);
            else
                fprintf(out, "\tmove.l %s, %d(%%fp)\n",
                    opreg[k], spill_byte_offset(fn, op->temp));
        } else {
            wd(out, fn, op->temp, opreg[k]);
        }
    }
}

static void
emit_insn(FILE *out, struct ir_func *fn, struct ir_insn *i)
{
    switch (i->op) {
    case IR_NOP:
        break;

    case IR_ASM:
        if (i->asm_info) {
            emit_asm_ext(out, fn, i);
            break;
        }
        /* basic inline asm: the string is emitted verbatim */
        fprintf(out, "%s\n", i->sym);
        break;

    case IR_LIC: {
        const char *sd = rd(fn, i->dst, 0);
        /* i->imm is int64_t; IR_LIC holds a 32-bit immediate, so cast to long
           to match %ld.  Passing the 64-bit field to %ld reads only half of it
           on an ILP32 host (the self-host build), which also shifts the %s that
           follows -- the register name then prints as (null). */
        if (i->imm >= -128 && i->imm <= 127)
            fprintf(out, "\tmoveq #%ld, %s\n", (long)i->imm, sd);
        else
            fprintf(out, "\tmove.l #%ld, %s\n", (long)i->imm, sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_LEA: {
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tlea %s, %%a0\n", i->sym);
        fprintf(out, "\tmove.l %%a0, %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_LABADDR: {
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tlea .L%d_%d, %%a0\n", label_prefix, i->label);
        fprintf(out, "\tmove.l %%a0, %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ADL: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tlea %d(%%fp), %%a0\n", off);
        fprintf(out, "\tmove.l %%a0, %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_MOV: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmove.l %s, %s\n", sa, sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ADD:  emit_binop(out, fn, i, "add.l");  break;
    case IR_SUB:  emit_binop(out, fn, i, "sub.l");  break;
    case IR_MUL:  emit_binop(out, fn, i, "muls.l"); break;
    case IR_ADDO:
    case IR_SUBO:
    case IR_MULO: emit_overflow(out, fn, i); break;
    case IR_AND:  emit_binop(out, fn, i, "and.l");  break;
    case IR_OR:   emit_binop(out, fn, i, "or.l");   break;
    case IR_XOR:  emit_binop(out, fn, i, "eor.l");  break;
    case IR_SHL:  emit_binop(out, fn, i, "lsl.l");  break;
    case IR_SHRS: emit_binop(out, fn, i, "asr.l");  break;
    case IR_SHRU: emit_binop(out, fn, i, "lsr.l");  break;

    case IR_DIVS: emit_div(out, fn, i, "divs.l", 1); break;
    case IR_DIVU: emit_div(out, fn, i, "divu.l", 0); break;

    case IR_MODS:
    case IR_MODU: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        const char *divop = (i->op == IR_MODS) ? "divs.l" : "divu.l";
        if (strcmp(sb, "%d1") != 0) {
            fprintf(out, "\tmove.l %s, %%d1\n", sb);
            sb = "%d1";
        }
        emit_divzero_guard(out, sb);
        emit_divovf_guard(out, sa, sb, i->op == IR_MODS);   /* before the divide */
        /* remainder = dividend - (dividend / divisor) * divisor.  The quotient
           and product are built in %d0; the dividend must survive until the
           final subtract, so it is saved and restored into %d1 (the divisor in
           %d1 is dead after the muls).  Restoring into the destination instead
           breaks when the destination is %d0, since that holds the product: the
           subtract would then be sub.l %d0,%d0 and the modulo would always be
           zero.  Computing in %d1 and moving to the destination avoids that. */
        fprintf(out, "\tmove.l %s, -(%%sp)\n", sa);
        fprintf(out, "\tmove.l %s, %%d0\n", sa);
        fprintf(out, "\t%s %s, %%d0\n", divop, sb);
        fprintf(out, "\tmuls.l %s, %%d0\n", sb);
        fprintf(out, "\tmove.l (%%sp)+, %%d1\n");
        fprintf(out, "\tsub.l %%d0, %%d1\n");
        if (strcmp(sd, "%d1") != 0)
            fprintf(out, "\tmove.l %%d1, %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_NEG: emit_unop(out, fn, i, "neg.l"); break;
    case IR_NOT: emit_unop(out, fn, i, "not.l"); break;

    case IR_LB:  emit_load(out, fn, i, "b", 0, 1); break;
    case IR_LBS: emit_load(out, fn, i, "b", 1, 0); break;
    case IR_LH:  emit_load(out, fn, i, "w", 0, 1); break;
    case IR_LHS: emit_load(out, fn, i, "w", 1, 0); break;
    case IR_LW:  emit_load(out, fn, i, "l", 0, 0); break;

    case IR_SB: emit_store(out, fn, i, "b"); break;
    case IR_SH: emit_store(out, fn, i, "w"); break;
    case IR_SW: emit_store(out, fn, i, "l"); break;

    case IR_LDL: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmove.l %d(%%fp), %s\n", off, sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_STL: {
        const char *sa = rs(out, fn, i->a, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmove.l %s, %d(%%fp)\n", sa, off);
        break;
    }

    case IR_ALLOCA: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "%d0") != 0)
            fprintf(out, "\tmove.l %s, %%d0\n", sa);
        fprintf(out, "\taddq.l #3, %%d0\n");
        fprintf(out, "\tand.l #-4, %%d0\n");
        fprintf(out, "\tsuba.l %%d0, %%sp\n");
        fprintf(out, "\tmovea.l %%sp, %%a0\n");
        fprintf(out, "\tmove.l %%a0, %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMPEQ:  emit_cmp(out, fn, i, "seq"); break;
    case IR_CMPNE:  emit_cmp(out, fn, i, "sne"); break;
    case IR_CMPLTS: emit_cmp(out, fn, i, "slt"); break;
    case IR_CMPLES: emit_cmp(out, fn, i, "sle"); break;
    case IR_CMPGTS: emit_cmp(out, fn, i, "sgt"); break;
    case IR_CMPGES: emit_cmp(out, fn, i, "sge"); break;
    case IR_CMPLTU: emit_cmp(out, fn, i, "scs"); break;
    case IR_CMPLEU: emit_cmp(out, fn, i, "sls"); break;
    case IR_CMPGTU: emit_cmp(out, fn, i, "shi"); break;
    case IR_CMPGEU: emit_cmp(out, fn, i, "scc"); break;

    case IR_JMP:
        fprintf(out, "\tbra .L%d_%d\n", label_prefix, i->label);
        break;
    case IR_IJMP: {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tmove.l %s, %%a0\n", sa);
        fprintf(out, "\tjmp (%%a0)\n");
        break;
    }
    case IR_BZ:
    case IR_BNZ: {
        const char *bcc = (i->op == IR_BZ) ? "beq" : "bne";
        if (i->a >= 0 && i->a < fn->ntemps && temp_is_i64[i->a]) {
            /* test the full 64-bit value: OR both words into a scratch.
               move the high word first (a nop when it is already d0), so
               the low word (never d0) is not clobbered before the or. */
            int hi, lo;
            i64_rs(out, fn, i->a, &hi, &lo);
            fprintf(out, "\tmove.l %s, %%d0\n\tor.l %s, %%d0\n\t%s .L%d_%d\n",
                dregs[hi], dregs[lo], bcc, label_prefix, i->label);
        } else {
            const char *sa = rs(out, fn, i->a, 0);
            fprintf(out, "\ttst.l %s\n\t%s .L%d_%d\n",
                sa, bcc, label_prefix, i->label);
        }
        break;
    }
    case IR_LABEL:
        fprintf(out, ".L%d_%d:\n", label_prefix, i->label);
        break;

    case IR_ARG:
        if (narg >= CF_MAX_ARGS)
            die("cf_emit: too many args");
        arg_is_float[narg] = 0;
        arg_is_f32[narg] = 0;
        arg_is_i64[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_FARG:
        if (narg >= CF_MAX_ARGS)
            die("cf_emit: too many args");
        arg_is_float[narg] = 1;
        arg_is_f32[narg] = (i->imm == FWIDTH_F32);
        arg_is_i64[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_CALL:
        emit_call_flush(out, fn, i, 0);
        break;
    case IR_CALLI:
        emit_call_flush(out, fn, i, 1);
        break;
    case IR_FCALL:
        emit_call_flush(out, fn, i, 0);
        break;
    case IR_FCALLI:
        emit_call_flush(out, fn, i, 1);
        break;
    case IR_TAILCALL:
        emit_tailcall_flush(out, fn, i, 0);
        break;
    case IR_TAILCALLI:
        emit_tailcall_flush(out, fn, i, 1);
        break;

    case IR_CALL2:
        emit_call_flush(out, fn, i, 0);
        break;
    case IR_CALLI2:
        emit_call_flush(out, fn, i, 1);
        break;

    case IR_RET:
        emit_epilogue(out, fn);
        break;
    case IR_RETV: {
        const char *sa = rs(out, fn, i->a, 0);
        if (strcmp(sa, "%d0") != 0)
            fprintf(out, "\tmove.l %s, %%d0\n", sa);
        emit_epilogue(out, fn);
        break;
    }
    case IR_RETV2: {
        /* the value in the first return register (d0), the error signal in the
           second (d1), then return (the failable-routine ABI). */
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb;
        if (strcmp(sa, "%d0") != 0)
            fprintf(out, "\tmove.l %s, %%d0\n", sa);
        sb = rs(out, fn, i->b, 1);
        if (strcmp(sb, "%d1") != 0)
            fprintf(out, "\tmove.l %s, %%d1\n", sb);
        emit_epilogue(out, fn);
        break;
    }
    case IR_FRETV: {
        const char *sa = frs(out, fn, i->a, 0);
        if (strcmp(sa, "%fp0") != 0)
            fprintf(out, "\tfmove.x %s, %%fp0\n", sa);
        emit_epilogue(out, fn);
        break;
    }

    case IR_MARK: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmove.l %%fp, %d(%%fp)\n", off);
        fprintf(out, "\tlea 0(%%sp), %%a0\n");
        fprintf(out, "\tmove.l %%a0, %d(%%fp)\n", off + 4);
        fprintf(out, "\tlea .Lmark%d_%d, %%a0\n",
            label_prefix, i->label);
        fprintf(out, "\tmove.l %%a0, %d(%%fp)\n", off + 8);
        fprintf(out, "\tlea %d(%%fp), %%a0\n", off);
        fprintf(out, "\tmove.l %%a0, __cont_mark_sp\n");
        /* stash the continuation-arena high-water mark (first entry only,
         * before the re-entry label) for IR_CONT_UNWIND to restore */
        fprintf(out, "\tmove.l __cont_arena_ptr, %%d0\n");
        fprintf(out, "\tmove.l %%d0, %d(%%fp)\n", off + 12);
        fprintf(out, "\tmoveq #0, %%d0\n");
        fprintf(out, ".Lmark%d_%d:\n", label_prefix, i->label);
        if (strcmp(sd, "%d0") != 0)
            fprintf(out, "\tmove.l %%d0, %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CONT_UNWIND: {
        /* restore the continuation arena to the mark-time high-water mark,
         * reclaiming every buffer captured within the closing reset extent */
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmove.l %d(%%fp), %%d0\n", off + 12);
        fprintf(out, "\tmove.l %%d0, __cont_arena_ptr\n");
        break;
    }

    case IR_CAPTURE: {
        const char *sd = rd(fn, i->dst, 0);
        /* ColdFire-legal movem (control mode); the stack layout the
         * capture/resume protocol sees is unchanged */
        fprintf(out, "\tlea -24(%%sp), %%sp\n");
        fprintf(out, "\tmovem.l %%d2-%%d7, (%%sp)\n");
        fprintf(out, "\tjsr __cont_capture\n");
        fprintf(out, "\tmovem.l (%%sp), %%d2-%%d7\n");
        fprintf(out, "\tlea 24(%%sp), %%sp\n");
        if (strcmp(sd, "%d0") != 0)
            fprintf(out, "\tmove.l %%d0, %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_RESUME: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", sb);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", sa);
        fprintf(out, "\tjsr __cont_resume\n");
        /* __cont_resume does not return here */
        break;
    }

    case IR_FADD: emit_fbinop(out, fn, i, "fadd.x"); break;
    case IR_FSUB: emit_fbinop(out, fn, i, "fsub.x"); break;
    case IR_FMUL: emit_fbinop(out, fn, i, "fmul.x"); break;
    case IR_FDIV: emit_fbinop(out, fn, i, "fdiv.x"); break;
    case IR_FNEG: emit_funop(out, fn, i, "fneg.x"); break;
    case IR_FABS: emit_funop(out, fn, i, "fabs.x"); break;
    case IR_FSQRT: emit_funop(out, fn, i, "fsqrt.x"); break;

    case IR_FCMPEQ:
    case IR_FCMPLT:
    case IR_FCMPLE: {
        const char *sa, *sb, *sd;
        const char *fbcc;
        int id = fcmp_serial++;

        sa = frs(out, fn, i->a, 0);
        sb = frs(out, fn, i->b, 1);
        sd = rd(fn, i->dst, 0);
        if (i->op == IR_FCMPEQ)
            fbcc = "fbeq";
        else if (i->op == IR_FCMPLT)
            fbcc = "fblt";
        else
            fbcc = "fble";
        fprintf(out, "\tfcmp.x %s, %s\n", sb, sa);
        fprintf(out, "\tmoveq #1, %s\n", sd);
        fprintf(out, "\t%s .Lfc%d\n", fbcc, id);
        fprintf(out, "\tmoveq #0, %s\n", sd);
        fprintf(out, ".Lfc%d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ITOF: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst, 0);
        fprintf(out, "\tfmove.l %s, %s\n", sa, sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FTOI: {
        const char *sa = frs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tfintrz.x %s, %%fp0\n", sa);
        fprintf(out, "\tfmove.l %%fp0, %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_FROUND: {
        /* f64 -> i32, round to nearest even: fmove.l to an integer destination
           converts under the FPCR rounding mode, which the runtime leaves at
           the default round-to-nearest-even. */
        const char *sa = frs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tfmove.l %s, %s\n", sa, sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    /* IR_F32 on ColdFire: the FPU computes in wide registers (no single-
     * rounded fsadd/fsmul), so single precision is storage-only, rounded at
     * fmove.s.  single -> double is an exact widening copy; double -> single
     * rounds through a single-precision stack round-trip. */
    case IR_F32TOF64: {
        const char *sa = frs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst, 0);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tfmove.x %s, %s\n", sa, sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_F64TOF32: {
        const char *sa = frs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst, 0);
        fprintf(out, "\tfmove.s %s, -(%%sp)\n", sa);   /* round to single */
        fprintf(out, "\tfmove.s (%%sp)+, %s\n", sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FLS: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst, 0);
        fprintf(out, "\tmovea.l %s, %%a0\n", sa);
        fprintf(out, "\tfmove.s (%%a0), %s\n", sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FLD: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst, 0);
        fprintf(out, "\tmovea.l %s, %%a0\n", sa);
        fprintf(out, "\tfmove.%c (%%a0), %s\n",
            i->imm == FWIDTH_F32 ? 's' : 'd', sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSS: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = frs(out, fn, i->b, 0);
        fprintf(out, "\tmovea.l %s, %%a0\n", sa);
        fprintf(out, "\tfmove.s %s, (%%a0)\n", sb);
        break;
    }

    case IR_FSD: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = frs(out, fn, i->b, 0);
        fprintf(out, "\tmovea.l %s, %%a0\n", sa);
        fprintf(out, "\tfmove.%c %s, (%%a0)\n",
            i->imm == FWIDTH_F32 ? 's' : 'd', sb);
        break;
    }

    case IR_FLH: {
        /* half in memory -> F64 reg, via __skj_extendhfsf then fmove.s.
         * The helper is integer-only (no fp regs), so fp2-fp7 survive. */
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst, 0);
        fprintf(out, "\tmovea.l %s, %%a0\n", sa);
        fprintf(out, "\tmoveq #0, %%d0\n");
        fprintf(out, "\tmove.w (%%a0), %%d0\n");     /* zero-extend the 16 bits */
        fprintf(out, "\tmove.l %%d0, -(%%sp)\n");
        fprintf(out, "\tjsr __skj_extendhfsf\n");     /* d0 = single bits */
        fprintf(out, "\taddq.l #4, %%sp\n");
        fprintf(out, "\tmove.l %%d0, -(%%sp)\n");
        fprintf(out, "\tfmove.s (%%sp), %s\n", sd);   /* widen single -> double */
        fprintf(out, "\taddq.l #4, %%sp\n");
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSH: {
        /* F64 reg -> half in memory, via fmove.s then __skj_truncsfhf. */
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = frs(out, fn, i->b, 0);
        fprintf(out, "\tfmove.s %s, -(%%sp)\n", sb);  /* round double -> single */
        fprintf(out, "\tmove.l (%%sp)+, %%d0\n");
        fprintf(out, "\tmove.l %%d0, -(%%sp)\n");
        fprintf(out, "\tjsr __skj_truncsfhf\n");       /* d0 = half bits */
        fprintf(out, "\taddq.l #4, %%sp\n");
        fprintf(out, "\tmovea.l %s, %%a0\n", sa);
        fprintf(out, "\tmove.w %%d0, (%%a0)\n");
        break;
    }

    case IR_FLDL: {
        const char *sd = frd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tfmove.%c %d(%%fp), %s\n",
            i->imm == FWIDTH_F32 ? 's' : 'd', off, sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSTL: {
        const char *sa = frs(out, fn, i->a, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tfmove.%c %s, %d(%%fp)\n",
            i->imm == FWIDTH_F32 ? 's' : 'd', sa, off);
        break;
    }

    case IR_FUNC:
    case IR_ENDF:
        break;

    /* ---- I64 opcodes ---- */

    case IR_LIC64: {
        int dhi, dlo;
        int64_t val = (int64_t)i->imm;
        uint32_t hi = (uint32_t)((uint64_t)val >> 32);
        uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);

        i64_rd(fn, i->dst, &dhi, &dlo);
        if ((int32_t)hi >= -128 && (int32_t)hi <= 127)
            fprintf(out, "\tmoveq #%d, %s\n", (int32_t)hi, dregs[dhi]);
        else
            fprintf(out, "\tmove.l #0x%x, %s\n", hi, dregs[dhi]);
        if ((int32_t)lo >= -128 && (int32_t)lo <= 127)
            fprintf(out, "\tmoveq #%d, %s\n", (int32_t)lo, dregs[dlo]);
        else
            fprintf(out, "\tmove.l #0x%x, %s\n", lo, dregs[dlo]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_ADD64: {
        int ahi, alo, bhi, blo, dhi, dlo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_rs(out, fn, i->b, &bhi, &blo);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (dlo != alo)
            fprintf(out, "\tmove.l %s, %s\n", dregs[alo], dregs[dlo]);
        if (dhi != ahi)
            fprintf(out, "\tmove.l %s, %s\n", dregs[ahi], dregs[dhi]);
        fprintf(out, "\tadd.l %s, %s\n", dregs[blo], dregs[dlo]);
        fprintf(out, "\taddx.l %s, %s\n", dregs[bhi], dregs[dhi]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_SUB64: {
        int ahi, alo, bhi, blo, dhi, dlo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_rs(out, fn, i->b, &bhi, &blo);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (dlo != alo)
            fprintf(out, "\tmove.l %s, %s\n", dregs[alo], dregs[dlo]);
        if (dhi != ahi)
            fprintf(out, "\tmove.l %s, %s\n", dregs[ahi], dregs[dhi]);
        fprintf(out, "\tsub.l %s, %s\n", dregs[blo], dregs[dlo]);
        fprintf(out, "\tsubx.l %s, %s\n", dregs[bhi], dregs[dhi]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_MUL64: {
        int ahi, alo, bhi, blo, dhi, dlo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_rs(out, fn, i->b, &bhi, &blo);
        i64_rd(fn, i->dst, &dhi, &dlo);
        /*
         * 64-bit multiply (low 64 bits of result):
         *   result_lo = a_lo * b_lo  (mulu.l gives 32-bit result)
         *   result_hi = a_lo * b_hi + a_hi * b_lo + hi(a_lo * b_lo)
         *
         * Use __muldi3 library call for correctness.
         */
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[blo]);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[bhi]);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[alo]);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[ahi]);
        fprintf(out, "\tjsr __muldi3\n");
        fprintf(out, "\tlea 16(%%sp), %%sp\n");
        if (dhi != 0)
            fprintf(out, "\tmove.l %%d0, %s\n", dregs[dhi]);
        if (dlo != 1)
            fprintf(out, "\tmove.l %%d1, %s\n", dregs[dlo]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_DIVS64:
    case IR_DIVU64:
    case IR_MODS64:
    case IR_MODU64: {
        int ahi, alo, bhi, blo, dhi, dlo;
        const char *helper =
            i->op == IR_DIVS64 ? "__divdi3" :
            i->op == IR_DIVU64 ? "__udivdi3" :
            i->op == IR_MODS64 ? "__moddi3" : "__umoddi3";

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_rs(out, fn, i->b, &bhi, &blo);
        i64_rd(fn, i->dst, &dhi, &dlo);
        /*
         * 64-bit divide and modulo through the soft64 helper family
         * (__divdi3 / __udivdi3 / __moddi3 / __umoddi3), which take the same
         * stack ABI as __muldi3 above (a_hi, a_lo, b_hi, b_lo) and return the
         * result in d0:d1.  soft64.c's C helpers are ABI-compatible as emitted
         * since the toolkit stack convention is the m68k C ABI.
         */
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[blo]);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[bhi]);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[alo]);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[ahi]);
        fprintf(out, "\tjsr %s\n", helper);
        fprintf(out, "\tlea 16(%%sp), %%sp\n");
        if (dhi != 0)
            fprintf(out, "\tmove.l %%d0, %s\n", dregs[dhi]);
        if (dlo != 1)
            fprintf(out, "\tmove.l %%d1, %s\n", dregs[dlo]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_AND64: {
        int ahi, alo, bhi, blo, dhi, dlo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_rs(out, fn, i->b, &bhi, &blo);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (dlo != alo)
            fprintf(out, "\tmove.l %s, %s\n", dregs[alo], dregs[dlo]);
        if (dhi != ahi)
            fprintf(out, "\tmove.l %s, %s\n", dregs[ahi], dregs[dhi]);
        fprintf(out, "\tand.l %s, %s\n", dregs[blo], dregs[dlo]);
        fprintf(out, "\tand.l %s, %s\n", dregs[bhi], dregs[dhi]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_OR64: {
        int ahi, alo, bhi, blo, dhi, dlo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_rs(out, fn, i->b, &bhi, &blo);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (dlo != alo)
            fprintf(out, "\tmove.l %s, %s\n", dregs[alo], dregs[dlo]);
        if (dhi != ahi)
            fprintf(out, "\tmove.l %s, %s\n", dregs[ahi], dregs[dhi]);
        fprintf(out, "\tor.l %s, %s\n", dregs[blo], dregs[dlo]);
        fprintf(out, "\tor.l %s, %s\n", dregs[bhi], dregs[dhi]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_XOR64: {
        int ahi, alo, bhi, blo, dhi, dlo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_rs(out, fn, i->b, &bhi, &blo);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (dlo != alo)
            fprintf(out, "\tmove.l %s, %s\n", dregs[alo], dregs[dlo]);
        if (dhi != ahi)
            fprintf(out, "\tmove.l %s, %s\n", dregs[ahi], dregs[dhi]);
        fprintf(out, "\teor.l %s, %s\n", dregs[blo], dregs[dlo]);
        fprintf(out, "\teor.l %s, %s\n", dregs[bhi], dregs[dhi]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_SHL64:
    case IR_SHRS64:
    case IR_SHRU64: {
        int ahi, alo, dhi, dlo;
        const char *sb;
        const char *func;
        int cc;

        /* A constant count is inlined: the shift helpers in soft64.c shift a
           64-bit value by a constant, so routing that back through the helper
           would make each one recurse into itself. */
        if (i64_const_shift_count(fn, i->b, &cc)) {
            i64_rs(out, fn, i->a, &ahi, &alo);
            i64_rd(fn, i->dst, &dhi, &dlo);
            emit_i64_shift_const(out, fn, i, ahi, alo, dhi, dlo, cc);
            break;
        }

        i64_rs(out, fn, i->a, &ahi, &alo);
        sb = rs(out, fn, i->b, 1);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (i->op == IR_SHL64)
            func = "__ashldi3";
        else if (i->op == IR_SHRS64)
            func = "__ashrdi3";
        else
            func = "__lshrdi3";
        fprintf(out, "\tmove.l %s, -(%%sp)\n", sb);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[alo]);
        fprintf(out, "\tmove.l %s, -(%%sp)\n", dregs[ahi]);
        fprintf(out, "\tjsr %s\n", func);
        fprintf(out, "\tlea 12(%%sp), %%sp\n");
        if (dhi != 0)
            fprintf(out, "\tmove.l %%d0, %s\n", dregs[dhi]);
        if (dlo != 1)
            fprintf(out, "\tmove.l %%d1, %s\n", dregs[dlo]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_NEG64: {
        int ahi, alo, dhi, dlo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (dlo != alo)
            fprintf(out, "\tmove.l %s, %s\n", dregs[alo], dregs[dlo]);
        if (dhi != ahi)
            fprintf(out, "\tmove.l %s, %s\n", dregs[ahi], dregs[dhi]);
        fprintf(out, "\tneg.l %s\n", dregs[dlo]);
        fprintf(out, "\tnegx.l %s\n", dregs[dhi]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_CMP64EQ:
    case IR_CMP64NE: {
        const char *ra, *rb, *sd;
        int id = i64cmp_serial++;
        int eq = (i->op == IR_CMP64EQ);

        /* Write the result only after both compares consume the operand
           registers: a spilled operand is loaded into the d0/d1 scratch pair,
           which the result register (sd) can also be, so materialising sd before
           the compares would clobber the operand's word. */
        i64_cmp_words(out, fn, i->a, i->b, 1, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbne .Li64c%d_ne\n", id);
        i64_cmp_words(out, fn, i->a, i->b, 0, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbne .Li64c%d_ne\n", id);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\tmoveq #%d, %s\n", eq ? 1 : 0, sd);
        fprintf(out, "\tbra .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_ne:\n", id);
        fprintf(out, "\tmoveq #%d, %s\n", eq ? 0 : 1, sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64LTS: {
        const char *ra, *rb, *sd;
        int id = i64cmp_serial++;

        i64_cmp_words(out, fn, i->a, i->b, 1, &ra, &rb);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tblt .Li64c%d_t\n", id);
        fprintf(out, "\tbgt .Li64c%d_f\n", id);
        i64_cmp_words(out, fn, i->a, i->b, 0, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbcc .Li64c%d_f\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmoveq #1, %s\n", sd);
        fprintf(out, "\tbra .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_f:\n", id);
        fprintf(out, "\tmoveq #0, %s\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64LES: {
        const char *ra, *rb, *sd;
        int id = i64cmp_serial++;

        i64_cmp_words(out, fn, i->a, i->b, 1, &ra, &rb);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tblt .Li64c%d_t\n", id);
        fprintf(out, "\tbgt .Li64c%d_f\n", id);
        i64_cmp_words(out, fn, i->a, i->b, 0, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbhi .Li64c%d_f\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmoveq #1, %s\n", sd);
        fprintf(out, "\tbra .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_f:\n", id);
        fprintf(out, "\tmoveq #0, %s\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64GTS: {
        const char *ra, *rb, *sd;
        int id = i64cmp_serial++;

        i64_cmp_words(out, fn, i->a, i->b, 1, &ra, &rb);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbgt .Li64c%d_t\n", id);
        fprintf(out, "\tblt .Li64c%d_f\n", id);
        i64_cmp_words(out, fn, i->a, i->b, 0, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbls .Li64c%d_f\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmoveq #1, %s\n", sd);
        fprintf(out, "\tbra .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_f:\n", id);
        fprintf(out, "\tmoveq #0, %s\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64GES: {
        const char *ra, *rb, *sd;
        int id = i64cmp_serial++;

        i64_cmp_words(out, fn, i->a, i->b, 1, &ra, &rb);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbgt .Li64c%d_t\n", id);
        fprintf(out, "\tblt .Li64c%d_f\n", id);
        i64_cmp_words(out, fn, i->a, i->b, 0, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbcs .Li64c%d_f\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmoveq #1, %s\n", sd);
        fprintf(out, "\tbra .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_f:\n", id);
        fprintf(out, "\tmoveq #0, %s\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64LTU: {
        const char *ra, *rb, *sd;
        int id = i64cmp_serial++;

        i64_cmp_words(out, fn, i->a, i->b, 1, &ra, &rb);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbcs .Li64c%d_t\n", id);
        fprintf(out, "\tbhi .Li64c%d_f\n", id);
        i64_cmp_words(out, fn, i->a, i->b, 0, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbcc .Li64c%d_f\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmoveq #1, %s\n", sd);
        fprintf(out, "\tbra .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_f:\n", id);
        fprintf(out, "\tmoveq #0, %s\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64LEU: {
        const char *ra, *rb, *sd;
        int id = i64cmp_serial++;

        i64_cmp_words(out, fn, i->a, i->b, 1, &ra, &rb);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbcs .Li64c%d_t\n", id);
        fprintf(out, "\tbhi .Li64c%d_f\n", id);
        i64_cmp_words(out, fn, i->a, i->b, 0, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbhi .Li64c%d_f\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmoveq #1, %s\n", sd);
        fprintf(out, "\tbra .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_f:\n", id);
        fprintf(out, "\tmoveq #0, %s\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64GTU: {
        const char *ra, *rb, *sd;
        int id = i64cmp_serial++;

        i64_cmp_words(out, fn, i->a, i->b, 1, &ra, &rb);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbhi .Li64c%d_t\n", id);
        fprintf(out, "\tbcs .Li64c%d_f\n", id);
        i64_cmp_words(out, fn, i->a, i->b, 0, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbls .Li64c%d_f\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmoveq #1, %s\n", sd);
        fprintf(out, "\tbra .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_f:\n", id);
        fprintf(out, "\tmoveq #0, %s\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64GEU: {
        const char *ra, *rb, *sd;
        int id = i64cmp_serial++;

        i64_cmp_words(out, fn, i->a, i->b, 1, &ra, &rb);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbhi .Li64c%d_t\n", id);
        fprintf(out, "\tbcs .Li64c%d_f\n", id);
        i64_cmp_words(out, fn, i->a, i->b, 0, &ra, &rb);
        fprintf(out, "\tcmp.l %s, %s\n", rb, ra);
        fprintf(out, "\tbcs .Li64c%d_f\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmoveq #1, %s\n", sd);
        fprintf(out, "\tbra .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_f:\n", id);
        fprintf(out, "\tmoveq #0, %s\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_LD64: {
        int dhi, dlo;
        const char *sa;

        sa = rs(out, fn, i->a, 0);
        i64_rd(fn, i->dst, &dhi, &dlo);
        fprintf(out, "\tmovea.l %s, %%a0\n", sa);
        fprintf(out, "\tmove.l (%%a0), %s\n", dregs[dhi]);
        fprintf(out, "\tmove.l 4(%%a0), %s\n", dregs[dlo]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_ST64: {
        int bhi, blo;
        const char *sa;

        sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tmovea.l %s, %%a0\n", sa);
        i64_rs(out, fn, i->b, &bhi, &blo);
        fprintf(out, "\tmove.l %s, (%%a0)\n", dregs[bhi]);
        fprintf(out, "\tmove.l %s, 4(%%a0)\n", dregs[blo]);
        break;
    }

    case IR_LDL64: {
        int dhi, dlo;
        int off = slot_offset(fn, i->slot);

        i64_rd(fn, i->dst, &dhi, &dlo);
        fprintf(out, "\tmove.l %d(%%fp), %s\n", off, dregs[dhi]);
        fprintf(out, "\tmove.l %d(%%fp), %s\n", off + 4, dregs[dlo]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_STL64: {
        int ahi, alo;
        int off = slot_offset(fn, i->slot);

        i64_rs(out, fn, i->a, &ahi, &alo);
        fprintf(out, "\tmove.l %s, %d(%%fp)\n", dregs[ahi], off);
        fprintf(out, "\tmove.l %s, %d(%%fp)\n", dregs[alo], off + 4);
        break;
    }

    case IR_SEXT64: {
        int dhi, dlo;
        const char *sa;

        sa = rs(out, fn, i->a, 0);
        i64_rd(fn, i->dst, &dhi, &dlo);
        fprintf(out, "\tmove.l %s, %s\n", sa, dregs[dlo]);
        fprintf(out, "\tmove.l %s, %s\n", sa, dregs[dhi]);
        fprintf(out, "\tmoveq #31, %%d1\n");
        fprintf(out, "\tasr.l %%d1, %s\n", dregs[dhi]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_ZEXT64: {
        int dhi, dlo;
        const char *sa;

        sa = rs(out, fn, i->a, 0);
        i64_rd(fn, i->dst, &dhi, &dlo);
        fprintf(out, "\tmove.l %s, %s\n", sa, dregs[dlo]);
        fprintf(out, "\tmoveq #0, %s\n", dregs[dhi]);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_TRUNC64: {
        int ahi, alo;
        const char *sd;

        i64_rs(out, fn, i->a, &ahi, &alo);
        sd = rd(fn, i->dst, 0);
        if (strcmp(dregs[alo], sd) != 0)
            fprintf(out, "\tmove.l %s, %s\n", dregs[alo], sd);
        wd(out, fn, i->dst, sd);
        (void)ahi;
        break;
    }

    case IR_ARG64:
        if (narg >= CF_MAX_ARGS)
            die("cf_emit: too many args");
        arg_is_float[narg] = 0;
        arg_is_i64[narg] = 1;
        arg_temps[narg++] = i->a;
        break;

    case IR_CALL64:
        emit_call_flush(out, fn, i, 0);
        break;
    case IR_CALLI64:
        emit_call_flush(out, fn, i, 1);
        break;

    case IR_RETV64: {
        int ahi, alo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        if (ahi != 0)
            fprintf(out, "\tmove.l %s, %%d0\n", dregs[ahi]);
        if (alo != 1)
            fprintf(out, "\tmove.l %s, %%d1\n", dregs[alo]);
        emit_epilogue(out, fn);
        break;
    }

    case IR_VA_START: {
        /* Initialize the va_list at [temp a].  The toolkit convention pushes
           every argument on the caller's stack (caller pops), so there is no
           register save area: all varargs live contiguously just past the
           named parameters, at %fp + 8 + sum(named param slot bytes).  Fill
           overflow_arg_area (field @8 of the SysV __va_list_tag) with that
           address and zero the unused register-area fields; runtime/va_cf.c
           reads only overflow_arg_area. */
        const char *sa = rs(out, fn, i->a, 0);   /* va_list struct address */
        int nb = 8, k;
        for (k = 0; k < fn->nparams; k++)
            nb += (fn->slot_size[k] + 3) & ~3;
        fprintf(out, "\tmove.l %s, %%a0\n", sa);
        fprintf(out, "\tlea %d(%%fp), %%a1\n", nb);
        fprintf(out, "\tmove.l %%a1, 8(%%a0)\n");   /* overflow_arg_area */
        fprintf(out, "\tclr.l (%%a0)\n");           /* gp_offset    = 0 */
        fprintf(out, "\tclr.l 4(%%a0)\n");          /* fp_offset    = 0 */
        fprintf(out, "\tclr.l 12(%%a0)\n");         /* reg_save_area = 0 */
        break;
    }

    case IR_LOC:
        /* debug-info line marker (-g): a DWARF .loc the assembler turns into a
           .debug_line row at this instruction's address. */
        fprintf(out, "\t.loc 1 %d\n", (int)i->imm);
        break;

    default:
        die("cf_emit: unhandled op %d", i->op);
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
    uses_floats = func_uses_fpregs(fn);

    /* map each temp defined by a 64-bit op, so a truthiness test on an
       i64 operand (IR_BZ/IR_BNZ) tests both words, not just one. */
    temp_is_i64 = arena_zalloc(fn->arena, fn->ntemps * sizeof(int));
    for (i = fn->head; i; i = i->next)
        if (i->dst >= 0 && i->dst < fn->ntemps && ir_op_is_i64_def(i->op))
            temp_is_i64[i->dst] = 1;

    fprintf(out, "\n\t.text\n\t.align 2\n");
    if (!fn->is_local)
        fprintf(out, "\t.globl %s\n", fn->name);
    /* STT_FUNC + a size so a debugger maps an address to this function and a
       backtrace knows its extent; the body stays wholly in .text, so
       .Lfunc_end - name is its byte length. */
    fprintf(out, "\t.type %s, @function\n", fn->name);
    fprintf(out, "%s:\n", fn->name);
    emit_prologue(out, fn);

    for (i = fn->head; i; i = i->next) {
        emit_insn(out, fn, i);
        if (i->op != IR_FUNC && i->op != IR_ENDF &&
            i->op != IR_LABEL)
            last = i;
    }

    if (!last || (last->op != IR_RET && last->op != IR_RETV &&
        last->op != IR_RETV64 && last->op != IR_FRETV &&
        last->op != IR_TAILCALL && last->op != IR_TAILCALLI))
        emit_epilogue(out, fn);

    fprintf(out, ".Lfunc_end%d:\n", label_prefix);
    fprintf(out, "\t.size %s, .Lfunc_end%d-%s\n",
            fn->name, label_prefix, fn->name);
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

/* format a relocation operand "sym", "sym + N", or "sym - N" (the addend is a
   symbol-relative initializer's byte offset, carried in the init value word) */
static const char *
sym_ref(char *buf, size_t n, const char *sym, int64_t addend)
{
    if (addend > 0)
        snprintf(buf, n, "%s + %lld", sym, (long long)addend);
    else if (addend < 0)
        snprintf(buf, n, "%s - %lld", sym, (long long)-addend);
    else
        snprintf(buf, n, "%s", sym);
    return buf;
}

/* emit an aggregate global's byte image: pad to each item's offset, emit the
   sized value (8-byte high word first, big-endian m68k), pad to the full size */
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
        if (it->sym) {
            char ref[160];
            fprintf(out, "\t.long %s\n", sym_ref(ref, sizeof ref, it->sym, it->ival));
        } else if (it->size == 8) {
            uint64_t b = (uint64_t)it->ival;
            fprintf(out, "\t.long 0x%08x\n", (unsigned)(b >> 32));
            fprintf(out, "\t.long 0x%08x\n", (unsigned)(b & 0xFFFFFFFF));
        } else if (it->size == 2)
            fprintf(out, "\t.short 0x%04x\n", (unsigned)(it->ival & 0xFFFF));
        else if (it->size == 1)
            fprintf(out, "\t.byte 0x%02x\n", (unsigned)(it->ival & 0xFF));
        else
            fprintf(out, "\t.long 0x%08x\n", (unsigned)it->ival);
        cur += it->size;
    }
    if (g->arr_size > cur)
        fprintf(out, "\t.space %d\n", g->arr_size - cur);
}

/* true when a global has no initializer at all (zero-filled): it belongs in
   .bss (NOBITS), so its zero bytes cost no file space, rather than in .data. */
static int
global_is_bss(struct ir_global *g)
{
    return !g->inits && !g->init_string && g->init_count == 0;
}

static int
global_elsz(struct ir_global *g)
{
    switch (g->base_type) {
    case IR_I8:  return 1;
    case IR_I16: return 2;
    case IR_F64: return 8;
    case IR_I64: return 8;
    default:     return 4;
    }
}

/* the alignment + optional .globl + label common to both sections */
static void
emit_global_head(FILE *out, struct ir_global *g)
{
    /* m68k aligns to 4; an _Alignas request (g->align) may raise it.  On m68k
       (both m68k-linux-gnu-as and skj-as) the .align operand is a byte count,
       not a power-of-two exponent (RISC-V's is the exponent), so emit the
       alignment in bytes. */
    int alb = 4;
    if (g->align > alb)
        alb = g->align;
    fprintf(out, "\t.align %d\n", alb);
    if (!g->is_local)
        fprintf(out, "\t.globl %s\n", g->name);
    fprintf(out, "%s:\n", g->name);
}

/* emit a global's initialized byte image (the content after its label). */
static void
emit_global_def(FILE *out, struct ir_global *g)
{
    int elsz = global_elsz(g);

    if (g->is_strobj) {
        /* A static Kobold string object: the {refcount, kind, len, cap} header
           in target endianness, then the text and a trailing NUL.  cap == len:
           a static string carries no spare capacity.  Emitting the header words
           as .long keeps them big-endian on ColdFire, where str_core.c reads
           them with native loads (a header baked into the KPM as little-endian
           bytes would be byte-swapped). */
        fprintf(out, "\t.long 0x%08x\n", 0xffffffffu);  /* KP_RC_STATIC */
        fprintf(out, "\t.long 0x%08x\n", 1u);           /* KP_OBJ_STRING */
        fprintf(out, "\t.long %d\n", g->init_strlen);   /* byte length */
        fprintf(out, "\t.long %d\n", g->init_strlen);   /* capacity (== length) */
        if (g->init_strlen > 0)
            emit_string_bytes(out, g->init_string, g->init_strlen);
        fprintf(out, "\t.byte 0\n");                    /* trailing NUL */
    } else if (g->inits) {
        emit_init_image(out, g);
    } else if (g->init_string) {
        emit_string_bytes(out, g->init_string, g->init_strlen);
    } else {
        int k;
        for (k = 0; k < g->init_count; k++) {
            if (g->init_syms && g->init_syms[k]) {
                char ref[160];
                fprintf(out, "\t.long %s\n",
                    sym_ref(ref, sizeof ref, g->init_syms[k],
                            g->init_ivals[k]));
            } else if (g->base_type == IR_F64 ||
                 g->base_type == IR_I64) {
                uint64_t bits = (uint64_t)g->init_ivals[k];
                fprintf(out, "\t.long 0x%08x\n",
                    (unsigned)(bits >> 32));
                fprintf(out, "\t.long 0x%08x\n",
                    (unsigned)(bits & 0xFFFFFFFF));
            } else if (elsz == 1)
                fprintf(out, "\t.byte %" PRId64 "\n",
                    g->init_ivals[k]);
            else if (elsz == 2)
                fprintf(out, "\t.short %" PRId64 "\n",
                    g->init_ivals[k]);
            else
                fprintf(out, "\t.long %" PRId64 "\n",
                    g->init_ivals[k]);
        }
        if (g->arr_size > g->init_count)
            fprintf(out, "\t.space %d\n",
                (g->arr_size - g->init_count) * elsz);
    }
}

static void
emit_globals(FILE *out, struct ir_program *prog)
{
    struct ir_global *g;
    int any_bss = 0;
    int any_ro = 0;

    fputs("\n\t.data\n", out);
    for (g = prog->globals; g; g = g->next) {
        if (global_is_bss(g)) {
            any_bss = 1;
            continue;
        }
        if (g->is_rodata) {
            any_ro = 1;
            continue;
        }
        emit_global_head(out, g);
        emit_global_def(out, g);
    }

    /* initialized, read-only globals (const): same bytes, .rodata section */
    if (any_ro) {
        fputs("\n\t.section .rodata\n", out);
        for (g = prog->globals; g; g = g->next) {
            if (global_is_bss(g) || !g->is_rodata)
                continue;
            emit_global_head(out, g);
            emit_global_def(out, g);
        }
    }

    if (!any_bss)
        return;

    /* Zero-filled globals: .bss (NOBITS) so their bytes cost no file space. */
    fputs("\n\t.bss\n", out);
    for (g = prog->globals; g; g = g->next) {
        int elsz = global_elsz(g);
        int sz;

        if (!global_is_bss(g))
            continue;
        emit_global_head(out, g);
        sz = (g->arr_size > 0) ? g->arr_size * elsz : elsz;
        fprintf(out, "\t.space %d\n", sz);
    }
}

/****************************************************************
 * Entry point
 ****************************************************************/

void
target_emit(FILE *out, struct ir_program *prog)
{
    struct ir_func *fn;

    fputs("| generated ColdFire / m68k assembly\n", out);

    /* Under -g a front end sets src_file; emit the DWARF .file hint once so the
       per-statement .loc directives (the IR_LOC case) attach to it.  skj-as
       turns .file/.loc into the big-endian .debug_line/.debug_info sections. */
    for (fn = prog->funcs; fn; fn = fn->next) {
        if (fn->src_file) {
            fprintf(out, "\t.file 1 \"%s\"\n", fn->src_file);
            break;
        }
    }

    for (fn = prog->funcs; fn; fn = fn->next)
        emit_function(out, fn);
    emit_globals(out, prog);
}
