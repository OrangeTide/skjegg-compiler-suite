/* mips_emit.c : MIPS I (R2000/R3000, little-endian, o32) back-end.
 *               Emits GAS-syntax assembly.
 */
/*
 * Covers the integer core, control flow, calls/tail-calls, alloca, the
 * delimited-continuation machinery, IEEE 754 double and single float, and
 * 64-bit integers (register pairs).
 *
 * We lean on the assembler's default `.set reorder`: for -march=mips1 the
 * assembler fills the load delay slot, the branch delay slot, the FP
 * condition-to-branch hazard, and the mult/div -> mflo/mfhi latency
 * (inserting a nop or scheduling a safe instruction), and expands the
 * macros we emit (li, la, move, mul, div, beqz, l.d, s.d, ...).  This is
 * correct on the R2000, which has no interlocks.  The cost is that
 * hand-written boot code, ISRs, and exception handlers that need
 * `.set noreorder` are not expressible here yet; that waits on a real
 * scheduler and an inline-asm path.
 *
 * Stack-based calling convention (matches the ColdFire/RISC-V pattern):
 *   - Args pushed on the stack; caller pops.  An int is 4 bytes; a float
 *     or a 64-bit int is 8 bytes, 8-aligned in the outgoing block.
 *   - Return value in $v0 (an i64 in $v0:$v1, a float in $f0).
 *   - Callee-save: $s0..$s7, even $f20..$f30, $fp, $ra.
 *   - Scratch: $t0..$t9, $v0, $v1, $a0..$a3, $f0..$f18.
 *   - $at is left to the assembler (li/la/branch macros clobber it).
 *
 * The two exceptions to the stack convention are the softfloat half
 * helpers (__skj_extendhfsf / __skj_truncsfhf), which are gcc-built o32
 * functions: they take their argument in $a0 and return in $v0.
 *
 * Frame layout (after prologue):
 *
 *      $fp + 8 + 4*i    param i (pushed by caller before jal)
 *      $fp + 4          saved $ra
 *      $fp + 0          saved old $fp
 *      $fp - locals     bottom of locals
 *      below locals     float spills (8 each), int spills (4), i64 spills (8)
 *      $sp + ...        saved $s0..$s7 then used even $f20..$f30
 */

#define _POSIX_C_SOURCE 200809L

#include "ir.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define NSAVED 8

/* Set from the SKJ_MIPS_NOREORDER environment variable in target_emit: when
   on, the emitter's output is post-processed by the delay-slot scheduler
   (see the bottom of the file) and the file is `.set noreorder`. */
static int g_noreorder;

/*
 * Integer register name table.  Indices 0-1 are scratch ($t0, $t1).
 * Indices 2-9 are allocatable callee-save ($s0-$s7), matching FIRST_REG=2,
 * NUM_REGS=8 in regalloc_mips.c.
 */
static const char *regs[] = {
    "$t0", "$t1",
    "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
};

/*
 * Float register name table (o32, -mfp32).  Indices 0-1 are scratch
 * ($f0, $f2).  Indices 2-7 are allocatable callee-save, the even
 * $f20..$f30 (a double is an even/odd pair named by the even register;
 * a single lives in the low register of the pair), matching FP_FIRST_REG=2,
 * FP_NUM_REGS=6 in regalloc_mips.c.
 */
static const char *fregs[] = {
    "$f0", "$f2",
    "$f20", "$f22", "$f24", "$f26", "$f28", "$f30",
};

/* allocatable float index range in fregs[] ($f20..$f30) */
#define FP_ALLOC_FIRST 2
#define FP_ALLOC_LAST  7

#ifdef CC_PSABI
/*
 * The o32 platform ABI.  Emitted for skj-exc-mips so a gcc-built guest
 * runtime (libexc and its host binding, o32 hard-float) can be linked and
 * called in both directions.  The rules used here:
 *
 *   - Arguments fill a conceptual "argument structure" one word at a time,
 *     the first four words ($a0-$a3) passed in registers with reserved home
 *     space, the rest on the stack.  The caller always reserves at least the
 *     16-byte home.
 *   - A two-word argument (long long, or a double) is 8-aligned in that
 *     structure, so it starts on an even word.  With that alignment it never
 *     splits across the register/stack boundary (unlike RISC-V's ILP32).
 *   - Results come back in $v0, or $v0:$v1 for two words, or $f0 for a float.
 *
 * The gcc-built runtime declares no float or double parameter, so the o32
 * rule that routes a leading FP argument through $f12/$f14 never fires at the
 * boundary.  A double argument here is therefore only ever skj-to-skj, and it
 * rides the integer argument structure (moved through its home slot), which
 * both sides agree on.
 *
 * The parameter side needs nothing new: slot_offset already lays parameters
 * out at $fp+8 upward, 8-aligned for 8-byte slots, which is exactly the
 * argument structure.  A register parameter's home is the caller's reserved
 * word, at $fp+slot_offset, so the prologue spills $a0-$a3 there and the rest
 * of the lowering reads every parameter from its slot as before.
 */
static const char *argreg[4] = { "$a0", "$a1", "$a2", "$a3" };
#endif

/****************************************************************
 * Frame layout helpers
 ****************************************************************/

static int
slot_bytes(struct ir_func *fn, int i)
{
    return (fn->slot_size[i] + 3) & ~3;
}

static int
locals_size(struct ir_func *fn)
{
    int i, x;

    x = 0;
    for (i = fn->nparams; i < fn->nslots; i++) {
        int sz = slot_bytes(fn, i);
        if (sz % 8 == 0)
            x = (x + 7) & ~7;
        x += sz;
    }
    return (x + 7) & ~7;   /* keep the spill areas below 8-aligned */
}

static int
slot_offset(struct ir_func *fn, int slot)
{
    int i, off;

    if (slot < fn->nparams) {
        /* params sit above the frame from $fp+8 upward */
        off = 8;
        for (i = 0; ; i++) {
            int sz = slot_bytes(fn, i);
            if (sz % 8 == 0)
                off = (off + 7) & ~7;
            if (i == slot)
                return off;
            off += sz;
        }
    }
    /* locals grow downward from $fp */
    off = 0;
    for (i = fn->nparams; i <= slot; i++) {
        int sz = slot_bytes(fn, i);
        if (sz % 8 == 0)
            off = (off + 7) & ~7;
        off += sz;
    }
    return -off;
}

static int
frame_size(struct ir_func *fn)
{
    return locals_size(fn) + fn->nfspills * 8
           + fn->nspills * 4
           + fn->ni64spills * 8;
}

#ifndef MIPS_SOFTFLOAT
/* float spills sit directly below the (8-aligned) locals, so a spilled
   double reload (l.d) stays 8-aligned regardless of the int spill count.
   Soft-float has no float register class, so no float spills and no reload. */
static int
fspill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - (fn->temp_spill[temp] + 8);
}
#endif

static int
spill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - fn->nfspills * 8
           - (fn->temp_spill[temp] + 4);
}

/* i64 spills are read/written as lw/sw pairs, so 4-byte alignment suffices */
static int
i64spill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - fn->nfspills * 8
           - fn->nspills * 4
           - (fn->temp_spill[temp] + 8);
}

/****************************************************************
 * Float temp -> register materialisation
 *
 * f32 selects the reload/store macro (l.s/s.s vs l.d/s.d); the register
 * name is width-independent.  A spilled F32 lives in the low 4 bytes of
 * its 8-byte slot.
 ****************************************************************/

#ifndef MIPS_SOFTFLOAT
static const char *
frs_w(FILE *out, struct ir_func *fn, int t, int scratch, int f32)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return fregs[r];
    fprintf(out, "\t%s %s, %d($fp)\n", f32 ? "l.s" : "l.d",
        fregs[scratch], fspill_byte_offset(fn, t));
    return fregs[scratch];
}

static const char *
frd_w(struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    return r >= 0 ? fregs[r] : fregs[scratch];
}

static void
fwd_f(FILE *out, struct ir_func *fn, int t, const char *reg, int f32)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\t%s %s, %d($fp)\n", f32 ? "s.s" : "s.d",
        reg, fspill_byte_offset(fn, t));
}
#endif /* MIPS_SOFTFLOAT */

/****************************************************************
 * Temp -> register materialisation
 ****************************************************************/

static const char *
rs(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return regs[r];
    fprintf(out, "\tlw %s, %d($fp)\n",
        regs[scratch], spill_byte_offset(fn, t));
    return regs[scratch];
}

static const char *
rd(struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return regs[r];
    return regs[scratch];
}

static void
wd(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tsw %s, %d($fp)\n",
        reg, spill_byte_offset(fn, t));
}

/****************************************************************
 * I64 register-pair helpers
 *
 * Pair layout (allocated from top of $s0..$s7):
 *   pair 0: lo=$s6 (idx 8), hi=$s7 (idx 9)
 *   pair 1: lo=$s4 (idx 6), hi=$s5 (idx 7)
 *   pair 2: lo=$s2 (idx 4), hi=$s3 (idx 5)
 *   pair 3: lo=$s0 (idx 2), hi=$s1 (idx 3)
 ****************************************************************/

static int i64_lo_idx(int pair) { return 8 - 2 * pair; }
static int i64_hi_idx(int pair) { return 9 - 2 * pair; }

static const char *
i64_rs_lo(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int pair = fn->temp_reg[t];
    if (pair >= 0)
        return regs[i64_lo_idx(pair)];
    fprintf(out, "\tlw %s, %d($fp)\n",
            regs[scratch], i64spill_byte_offset(fn, t));
    return regs[scratch];
}

static const char *
i64_rs_hi(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int pair = fn->temp_reg[t];
    if (pair >= 0)
        return regs[i64_hi_idx(pair)];
    fprintf(out, "\tlw %s, %d($fp)\n",
            regs[scratch], i64spill_byte_offset(fn, t) + 4);
    return regs[scratch];
}

static const char *
i64_rd_lo(struct ir_func *fn, int t, int scratch)
{
    int pair = fn->temp_reg[t];
    if (pair >= 0)
        return regs[i64_lo_idx(pair)];
    return regs[scratch];
}

static const char *
i64_rd_hi(struct ir_func *fn, int t, int scratch)
{
    int pair = fn->temp_reg[t];
    if (pair >= 0)
        return regs[i64_hi_idx(pair)];
    return regs[scratch];
}

static void
i64_wd_lo(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tsw %s, %d($fp)\n",
            reg, i64spill_byte_offset(fn, t));
}

static void
i64_wd_hi(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tsw %s, %d($fp)\n",
            reg, i64spill_byte_offset(fn, t) + 4);
}

#ifdef MIPS_SOFTFLOAT
/****************************************************************
 * Soft-float helper-call plumbing (PlayStation R3051, no coprocessor 1)
 *
 * Every float value is an integer bit pattern: a double lives in an i64
 * (register pair or 8-byte slot), a single in an int (one register or a
 * 4-byte slot).  Each float operation is a call into runtime/softfloat.c,
 * made with the o32 register convention the half-float helpers already use
 * (arguments in $a0.., a 16-byte argument home, results in $v0/$v0:$v1).
 * The helpers are gcc-built and preserve $s0..$s7, so a call is safe in the
 * middle of instruction selection: the allocated temps, which live in the
 * $s file or in slots, are untouched, and only the scratch registers move.
 ****************************************************************/

static const char *const sf_areg[4] = { "$a0", "$a1", "$a2", "$a3" };

static void
sf_call(FILE *out, const char *name)
{
    fprintf(out, "\taddiu $sp, $sp, -16\n");    /* o32 argument home */
    fprintf(out, "\tjal %s\n", name);
    fprintf(out, "\taddiu $sp, $sp, 16\n");
}

/* place a double operand (an i64 temp) into $a{lo}:$a{lo+1} */
static void
sf_arg_d(FILE *out, struct ir_func *fn, int t, int lo)
{
    const char *l = i64_rs_lo(out, fn, t, 0);
    fprintf(out, "\tmove %s, %s\n", sf_areg[lo], l);
    const char *h = i64_rs_hi(out, fn, t, 0);
    fprintf(out, "\tmove %s, %s\n", sf_areg[lo + 1], h);
}

/* place a single/int operand into $a{k} */
static void
sf_arg_w(FILE *out, struct ir_func *fn, int t, int k)
{
    const char *r = rs(out, fn, t, 0);
    fprintf(out, "\tmove %s, %s\n", sf_areg[k], r);
}

/* write $v0:$v1 into a double (i64) destination */
static void
sf_ret_d(FILE *out, struct ir_func *fn, int dst)
{
    const char *dlo = i64_rd_lo(fn, dst, 0);
    const char *dhi = i64_rd_hi(fn, dst, 1);

    if (strcmp(dlo, "$v0") != 0)
        fprintf(out, "\tmove %s, $v0\n", dlo);
    if (strcmp(dhi, "$v1") != 0)
        fprintf(out, "\tmove %s, $v1\n", dhi);
    i64_wd_lo(out, fn, dst, dlo);
    i64_wd_hi(out, fn, dst, dhi);
}

/* write $v0 into a single/int destination */
static void
sf_ret_w(FILE *out, struct ir_func *fn, int dst)
{
    const char *sd = rd(fn, dst, 0);

    if (strcmp(sd, "$v0") != 0)
        fprintf(out, "\tmove %s, $v0\n", sd);
    wd(out, fn, dst, sd);
}

/* a soft-float binary op: pick the double or single helper by width */
static void
sf_fbinop(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *dname, const char *fname)
{
    if (i->imm == FWIDTH_F32) {
        sf_arg_w(out, fn, i->a, 0);
        sf_arg_w(out, fn, i->b, 1);
        sf_call(out, fname);
        sf_ret_w(out, fn, i->dst);
    } else {
        sf_arg_d(out, fn, i->a, 0);
        sf_arg_d(out, fn, i->b, 2);
        sf_call(out, dname);
        sf_ret_d(out, fn, i->dst);
    }
}
#endif /* MIPS_SOFTFLOAT */

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
    wd(out, fn, i->dst, sd);
}

static void
emit_unop(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *mnem)
{
    const char *sa, *sd;

    sa = rs(out, fn, i->a, 0);
    sd = rd(fn, i->dst, 0);
    fprintf(out, "\t%s %s, %s\n", mnem, sd, sa);
    wd(out, fn, i->dst, sd);
}

static void
emit_cmp(FILE *out, struct ir_func *fn, struct ir_insn *i, int op)
{
    const char *sa, *sb, *sd;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    sd = rd(fn, i->dst, 0);

    switch (op) {
    case IR_CMPEQ:
        fprintf(out, "\txor %s, %s, %s\n", sd, sa, sb);
        fprintf(out, "\tsltiu %s, %s, 1\n", sd, sd);
        break;
    case IR_CMPNE:
        fprintf(out, "\txor %s, %s, %s\n", sd, sa, sb);
        fprintf(out, "\tsltu %s, $zero, %s\n", sd, sd);
        break;
    case IR_CMPLTS:
        fprintf(out, "\tslt %s, %s, %s\n", sd, sa, sb);
        break;
    case IR_CMPLES:                                  /* a<=b == !(b<a) */
        fprintf(out, "\tslt %s, %s, %s\n", sd, sb, sa);
        fprintf(out, "\txori %s, %s, 1\n", sd, sd);
        break;
    case IR_CMPGTS:                                  /* a>b == b<a */
        fprintf(out, "\tslt %s, %s, %s\n", sd, sb, sa);
        break;
    case IR_CMPGES:                                  /* a>=b == !(a<b) */
        fprintf(out, "\tslt %s, %s, %s\n", sd, sa, sb);
        fprintf(out, "\txori %s, %s, 1\n", sd, sd);
        break;
    case IR_CMPLTU:
        fprintf(out, "\tsltu %s, %s, %s\n", sd, sa, sb);
        break;
    case IR_CMPLEU:
        fprintf(out, "\tsltu %s, %s, %s\n", sd, sb, sa);
        fprintf(out, "\txori %s, %s, 1\n", sd, sd);
        break;
    case IR_CMPGTU:
        fprintf(out, "\tsltu %s, %s, %s\n", sd, sb, sa);
        break;
    case IR_CMPGEU:
        fprintf(out, "\tsltu %s, %s, %s\n", sd, sa, sb);
        fprintf(out, "\txori %s, %s, 1\n", sd, sd);
        break;
    }

    wd(out, fn, i->dst, sd);
}

#ifndef MIPS_SOFTFLOAT
static void
emit_fbinop(FILE *out, struct ir_func *fn, struct ir_insn *i,
            const char *base)
{
    int f32 = i->imm == FWIDTH_F32;
    const char *sfx = f32 ? "s" : "d";
    const char *sa, *sb, *sd;

    sa = frs_w(out, fn, i->a, 0, f32);
    sb = frs_w(out, fn, i->b, 1, f32);
    sd = frd_w(fn, i->dst, 0);
    fprintf(out, "\t%s.%s %s, %s, %s\n", base, sfx, sd, sa, sb);
    fwd_f(out, fn, i->dst, sd, f32);
}

static void
emit_funop(FILE *out, struct ir_func *fn, struct ir_insn *i,
           const char *base)
{
    int f32 = i->imm == FWIDTH_F32;
    const char *sfx = f32 ? "s" : "d";
    const char *sa, *sd;

    sa = frs_w(out, fn, i->a, 0, f32);
    sd = frd_w(fn, i->dst, 0);
    fprintf(out, "\t%s.%s %s, %s\n", base, sfx, sd, sa);
    fwd_f(out, fn, i->dst, sd, f32);
}
#endif

/****************************************************************
 * Per-instruction emission
 ****************************************************************/

#define RET_INT   0
#define RET_I64   1
#define RET_FLOAT 2

static int arg_temps[16];
static int arg_is_i64[16];
static int arg_is_float[16];
static int arg_fw[16];       /* per-arg float width: 1 = F32, 0 = F64 */
static int narg;
static int label_prefix;
#ifndef MIPS_SOFTFLOAT
static int fcmp_serial;
#endif
static int i64cmp_serial;

static void
emit_load(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *mnem)
{
    const char *sa, *sd;

    sa = rs(out, fn, i->a, 0);
    sd = rd(fn, i->dst, 0);
    fprintf(out, "\t%s %s, 0(%s)\n", mnem, sd, sa);
    wd(out, fn, i->dst, sd);
}

static void
emit_store(FILE *out, struct ir_func *fn, struct ir_insn *i,
           const char *mnem)
{
    const char *sa, *sb;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    fprintf(out, "\t%s %s, 0(%s)\n", mnem, sb, sa);
}

/*
 * Callee-saved float registers actually used by this function.  Only these
 * are saved/restored, so a pure-integer function pays nothing for the float
 * class.  used_fregs[] holds their fregs[] indices (2..7), in order.
 */
static int used_fregs[8];
static int n_used_fregs;

static void
compute_used_fregs(struct ir_func *fn)
{
#ifdef MIPS_SOFTFLOAT
    /* Soft-float uses no coprocessor-1 register, so there is never one to
       save.  Every float value is carried in the integer file. */
    (void)fn;
    n_used_fregs = 0;
#else
    struct ir_insn *i;
    int seen[FP_ALLOC_LAST + 1];
    int r;

    for (r = 0; r <= FP_ALLOC_LAST; r++)
        seen[r] = 0;
    n_used_fregs = 0;
    for (i = fn->head; i; i = i->next) {
        if (ir_op_is_float_def(i->op) && i->dst >= 0 &&
            i->dst < fn->ntemps) {
            r = fn->temp_reg[i->dst];
            if (r >= FP_ALLOC_FIRST && r <= FP_ALLOC_LAST)
                seen[r] = 1;
        }
    }
    for (r = FP_ALLOC_FIRST; r <= FP_ALLOC_LAST; r++)
        if (seen[r])
            used_fregs[n_used_fregs++] = r;
#endif
}

static void
emit_prologue(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int fsaved = n_used_fregs * 8;
    int base = NSAVED * 4 + fsaved + frame;
    /* round the whole frame to 16 so $sp stays 16-aligned across calls; the
       padding is dead space at the bottom, leaving every $fp-relative offset
       (and the epilogue) unchanged. */
    int total = (8 + base + 15) & ~15;
    int pad = total - (8 + base);
    int k;

    fprintf(out, "\taddiu $sp, $sp, -%d\n", total);
    fprintf(out, "\tsw $ra, %d($sp)\n", total - 4);
    fprintf(out, "\tsw $fp, %d($sp)\n", total - 8);
    fprintf(out, "\taddiu $fp, $sp, %d\n", total - 8);
    for (k = 0; k < NSAVED; k++)
        fprintf(out, "\tsw %s, %d($sp)\n", regs[k + 2], pad + k * 4);
    for (k = 0; k < n_used_fregs; k++)
        fprintf(out, "\ts.d %s, %d($sp)\n",
            fregs[used_fregs[k]], pad + NSAVED * 4 + k * 8);

#ifdef CC_PSABI
    /* Spill the register parameters into their argument-structure homes (the
       words the caller reserved at $fp+8 upward), so every parameter is read
       from its slot afterwards and the rest of the lowering does not know the
       difference. */
    for (k = 0; k < fn->nparams; k++) {
        int off = slot_offset(fn, k);
        int word = (off - 8) / 4;
        int words = slot_bytes(fn, k) >= 8 ? 2 : 1;
        if (word >= 4)
            continue;
        fprintf(out, "\tsw %s, %d($fp)\n", argreg[word], off);
        if (words == 2 && word + 1 < 4)
            fprintf(out, "\tsw %s, %d($fp)\n", argreg[word + 1], off + 4);
    }
#endif
}

static void
emit_epilogue_no_ret(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int fsaved = n_used_fregs * 8;
    int int_base = -(NSAVED * 4 + fsaved + frame);
    int fp_base = -(fsaved + frame);
    int k;

    for (k = 0; k < NSAVED; k++)
        fprintf(out, "\tlw %s, %d($fp)\n",
            regs[k + 2], int_base + k * 4);
    for (k = 0; k < n_used_fregs; k++)
        fprintf(out, "\tl.d %s, %d($fp)\n",
            fregs[used_fregs[k]], fp_base + k * 8);
    fprintf(out, "\tlw $ra, 4($fp)\n");
    fprintf(out, "\tlw $t0, 0($fp)\n");
    fprintf(out, "\taddiu $sp, $fp, 8\n");
    fprintf(out, "\tmove $fp, $t0\n");
}

static void
emit_epilogue(FILE *out, struct ir_func *fn)
{
    emit_epilogue_no_ret(out, fn);
    fprintf(out, "\tjr $ra\n");
}

#ifdef CC_PSABI
/*
 * The o32 call.  Arguments fill the argument structure (see the ABI note
 * above); the first four words go in $a0-$a3, the rest on the stack, a
 * two-word argument 8-aligned.  A float rides the integer structure through
 * its home slot, which keeps the register and stack paths one store.
 */
static void
emit_call_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                int indirect, int retkind)
{
    struct { int words, ireg, boff, fpreg; } loc[16];
    int k, wi, block, rawbytes;
    int leading_fp = 1, fpidx = 0;

    wi = 0;
    for (k = 0; k < narg; k++) {
        int w = (arg_is_i64[k] || (arg_is_float[k] && !arg_fw[k])) ? 2 : 1;
        if (w == 2)
            wi = (wi + 1) & ~1;
        loc[k].words = w;
        loc[k].boff = 4 * wi;
        loc[k].ireg = wi < 4 ? wi : -1;
        /* o32 hard-float: the leading run of FP arguments (at most two, before
           any integer argument) is ALSO passed in $f12/$f14, which is where a
           gcc-built callee reads a double.  The integer-register/home copy
           below stays, so a skj callee (which reads its float parameters from
           the home slot) sees the same value.  Both cost only a spare move. */
        if (leading_fp && arg_is_float[k] && fpidx < 2)
            loc[k].fpreg = 12 + 2 * fpidx++;
        else {
            loc[k].fpreg = -1;
            leading_fp = 0;
        }
        wi += w;
    }
    rawbytes = 4 * wi;
    block = rawbytes < 16 ? 16 : rawbytes;   /* always reserve the home area */
    block = (block + 15) & ~15;

    fprintf(out, "\taddiu $sp, $sp, -%d\n", block);

    for (k = 0; k < narg; k++) {
        int boff = loc[k].boff, ir = loc[k].ireg;

        if (arg_is_float[k]) {
            int f32 = arg_fw[k];
            const char *sa = frs_w(out, fn, arg_temps[k], 0, f32);
            /* store to the home slot; if it lands in the register region,
               also load it into the a-register(s) */
            fprintf(out, "\t%s %s, %d($sp)\n", f32 ? "s.s" : "s.d", sa, boff);
            if (ir >= 0) {
                fprintf(out, "\tlw %s, %d($sp)\n", argreg[ir], boff);
                if (!f32)
                    fprintf(out, "\tlw %s, %d($sp)\n", argreg[ir + 1], boff + 4);
            }
            /* leading FP arg: also place it in $f12/$f14 for a gcc callee */
            if (loc[k].fpreg >= 0)
                fprintf(out, "\tmov.%s $f%d, %s\n", f32 ? "s" : "d",
                    loc[k].fpreg, sa);
        } else if (arg_is_i64[k]) {
            const char *lo = i64_rs_lo(out, fn, arg_temps[k], 0);
            const char *hi = i64_rs_hi(out, fn, arg_temps[k], 1);
            if (ir >= 0) {
                fprintf(out, "\tmove %s, %s\n", argreg[ir], lo);
                fprintf(out, "\tmove %s, %s\n", argreg[ir + 1], hi);
            } else {
                fprintf(out, "\tsw %s, %d($sp)\n", lo, boff);
                fprintf(out, "\tsw %s, %d($sp)\n", hi, boff + 4);
            }
        } else {
            const char *sa = rs(out, fn, arg_temps[k], 0);
            if (ir >= 0)
                fprintf(out, "\tmove %s, %s\n", argreg[ir], sa);
            else
                fprintf(out, "\tsw %s, %d($sp)\n", sa, boff);
        }
    }

    if (indirect) {
        const char *sa = rs(out, fn, i->a, 0);
        if (strcmp(sa, "$t9") != 0)
            fprintf(out, "\tmove $t9, %s\n", sa);
        fprintf(out, "\tjalr $t9\n");
    } else {
        fprintf(out, "\tjal %s\n", i->sym);
    }
    fprintf(out, "\taddiu $sp, $sp, %d\n", block);
    narg = 0;

    if (i->dst >= 0) {
        if (retkind == RET_I64) {
            const char *dlo = i64_rd_lo(fn, i->dst, 0);
            const char *dhi = i64_rd_hi(fn, i->dst, 1);
            if (strcmp(dlo, "$v0") != 0)
                fprintf(out, "\tmove %s, $v0\n", dlo);
            if (strcmp(dhi, "$v1") != 0)
                fprintf(out, "\tmove %s, $v1\n", dhi);
            i64_wd_lo(out, fn, i->dst, dlo);
            i64_wd_hi(out, fn, i->dst, dhi);
        } else if (retkind == RET_FLOAT) {
            int f32 = i->imm == FWIDTH_F32;
            const char *sd = frd_w(fn, i->dst, 0);
            if (strcmp(sd, "$f0") != 0)
                fprintf(out, "\tmov.%s %s, $f0\n", f32 ? "s" : "d", sd);
            fwd_f(out, fn, i->dst, sd, f32);
        } else {
            const char *sd = rd(fn, i->dst, 0);
            if (strcmp(sd, "$v0") != 0)
                fprintf(out, "\tmove %s, $v0\n", sd);
            wd(out, fn, i->dst, sd);
        }
    }
}

/*
 * A tail call under the o32 ABI.  A real tail call would place the arguments
 * over the frame being torn down, which the register/home layout makes
 * fiddly.  The fall-back is an ordinary call and a return: the result is left
 * in $v0/$f0 by the call and the epilogue does not disturb it.  Excelsior,
 * the only front end that targets this ABI, does not rely on tail-call
 * elimination, so the extra frame is harmless.
 */
static void
emit_tailcall_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                    int indirect)
{
    struct ir_insn call = *i;

    call.dst = -1;
    emit_call_flush(out, fn, &call, indirect, RET_INT);
    emit_epilogue(out, fn);
}
#else
static void
emit_call_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                int indirect, int retkind)
{
    int k, push_bytes, off;

    /* An 8-byte arg (a float, or an i64) is 8-aligned in the outgoing block,
       mirroring slot_offset's param layout so the callee reads each param
       where the caller wrote it.  The whole block rounds to 16 so the callee
       is entered with a 16-aligned $sp. */
    off = 0;
    for (k = 0; k < narg; k++) {
        int eight = arg_is_float[k] || arg_is_i64[k];
        if (eight)
            off = (off + 7) & ~7;
        off += eight ? 8 : 4;
    }
    push_bytes = (off + 15) & ~15;

    if (push_bytes > 0)
        fprintf(out, "\taddiu $sp, $sp, -%d\n", push_bytes);
    off = 0;
    for (k = 0; k < narg; k++) {
        if (arg_is_float[k]) {
            int f32 = arg_fw[k];
            off = (off + 7) & ~7;
#ifdef MIPS_SOFTFLOAT
            /* A float rides the argument block in the same 8-byte, 8-aligned
               footprint the callee's param layout expects.  A double is two
               words from its i64; a single is one word from its int, at the
               low end (the callee reads only that word). */
            if (f32) {
                const char *sa = rs(out, fn, arg_temps[k], 0);
                fprintf(out, "\tsw %s, %d($sp)\n", sa, off);
            } else {
                const char *lo = i64_rs_lo(out, fn, arg_temps[k], 0);
                fprintf(out, "\tsw %s, %d($sp)\n", lo, off);
                const char *hi = i64_rs_hi(out, fn, arg_temps[k], 0);
                fprintf(out, "\tsw %s, %d($sp)\n", hi, off + 4);
            }
#else
            const char *sa = frs_w(out, fn, arg_temps[k], 0, f32);
            fprintf(out, "\t%s %s, %d($sp)\n", f32 ? "s.s" : "s.d",
                sa, off);
#endif
            off += 8;
        } else if (arg_is_i64[k]) {
            const char *lo, *hi;
            off = (off + 7) & ~7;
            lo = i64_rs_lo(out, fn, arg_temps[k], 0);
            fprintf(out, "\tsw %s, %d($sp)\n", lo, off);
            hi = i64_rs_hi(out, fn, arg_temps[k], 0);
            fprintf(out, "\tsw %s, %d($sp)\n", hi, off + 4);
            off += 8;
        } else {
            const char *sa = rs(out, fn, arg_temps[k], 0);
            fprintf(out, "\tsw %s, %d($sp)\n", sa, off);
            off += 4;
        }
    }
    if (indirect) {
        const char *sa = rs(out, fn, i->a, 0);
        if (strcmp(sa, "$t9") != 0)
            fprintf(out, "\tmove $t9, %s\n", sa);
        fprintf(out, "\tjalr $t9\n");
    } else {
        fprintf(out, "\tjal %s\n", i->sym);
    }
    if (push_bytes > 0)
        fprintf(out, "\taddiu $sp, $sp, %d\n", push_bytes);
    narg = 0;

    if (i->dst >= 0) {
        if (retkind == RET_I64) {
            const char *dlo = i64_rd_lo(fn, i->dst, 0);
            const char *dhi = i64_rd_hi(fn, i->dst, 1);
            if (strcmp(dlo, "$v0") != 0)
                fprintf(out, "\tmove %s, $v0\n", dlo);
            if (strcmp(dhi, "$v1") != 0)
                fprintf(out, "\tmove %s, $v1\n", dhi);
            i64_wd_lo(out, fn, i->dst, dlo);
            i64_wd_hi(out, fn, i->dst, dhi);
        } else if (retkind == RET_FLOAT) {
#ifdef MIPS_SOFTFLOAT
            /* a double comes back in $v0:$v1 (i64), a single in $v0 (int) */
            if (i->imm == FWIDTH_F32)
                sf_ret_w(out, fn, i->dst);
            else
                sf_ret_d(out, fn, i->dst);
#else
            int f32 = i->imm == FWIDTH_F32;
            const char *sd = frd_w(fn, i->dst, 0);
            if (strcmp(sd, "$f0") != 0)
                fprintf(out, "\tmov.%s %s, $f0\n", f32 ? "s" : "d", sd);
            fwd_f(out, fn, i->dst, sd, f32);
#endif
        } else {
            const char *sd = rd(fn, i->dst, 0);
            if (strcmp(sd, "$v0") != 0)
                fprintf(out, "\tmove %s, $v0\n", sd);
            wd(out, fn, i->dst, sd);
        }
    }
}

static void
emit_tailcall_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                    int indirect)
{
    int k;

    /* Overwrite the incoming parameter area (which the successor reads at
       its own $fp+8) with the new arguments, then tear the frame down and
       jump.  $t9 is caller-saved, so it survives the epilogue.  Integer
       arguments only (the tail-call callers are the Scheme front end). */
    for (k = 0; k < narg; k++) {
        const char *sa = rs(out, fn, arg_temps[k], 0);
        fprintf(out, "\tsw %s, %d($fp)\n", sa, 8 + 4 * k);
    }
    if (indirect) {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tmove $t9, %s\n", sa);
    }
    narg = 0;
    emit_epilogue_no_ret(out, fn);
    if (indirect)
        fprintf(out, "\tjr $t9\n");
    else
        fprintf(out, "\tj %s\n", i->sym);
}
#endif /* CC_PSABI */

static void
emit_insn(FILE *out, struct ir_func *fn, struct ir_insn *i)
{
    switch (i->op) {
    case IR_NOP:
        break;

    case IR_ASM:
        /* basic inline asm: the string is emitted verbatim.  Under
           .set noreorder the scheduler must not fill or reorder around the
           user's own delay slots, so bracket the text with sentinels that
           sched_emit copies through untouched (see sched_emit). */
        if (g_noreorder)
            fprintf(out, "#skj-asm-begin\n%s\n#skj-asm-end\n", i->sym);
        else
            fprintf(out, "%s\n", i->sym);
        break;

    case IR_LIC: {
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tli %s, %ld\n", sd, i->imm);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_LEA: {
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tla %s, %s\n", sd, i->sym);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ADL: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\taddiu %s, $fp, %d\n", sd, off);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_MOV: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmove %s, %s\n", sd, sa);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ADD:  emit_binop(out, fn, i, "addu"); break;
    case IR_SUB:  emit_binop(out, fn, i, "subu"); break;
    case IR_MUL:  emit_binop(out, fn, i, "mul");  break;   /* macro: mult+mflo */
    case IR_AND:  emit_binop(out, fn, i, "and");  break;
    case IR_OR:   emit_binop(out, fn, i, "or");   break;
    case IR_XOR:  emit_binop(out, fn, i, "xor");  break;
    case IR_SHL:  emit_binop(out, fn, i, "sllv"); break;
    case IR_SHRS: emit_binop(out, fn, i, "srav"); break;
    case IR_SHRU: emit_binop(out, fn, i, "srlv"); break;
    case IR_DIVS: emit_binop(out, fn, i, "div");  break;   /* macro: div+mflo */
    case IR_DIVU: emit_binop(out, fn, i, "divu"); break;
    case IR_MODS: emit_binop(out, fn, i, "rem");  break;   /* macro: div+mfhi */
    case IR_MODU: emit_binop(out, fn, i, "remu"); break;

    case IR_NEG: emit_unop(out, fn, i, "negu"); break;
    case IR_NOT: emit_unop(out, fn, i, "not");  break;

    case IR_LB:  emit_load(out, fn, i, "lbu"); break;
    case IR_LBS: emit_load(out, fn, i, "lb");  break;
    case IR_LH:  emit_load(out, fn, i, "lhu"); break;
    case IR_LHS: emit_load(out, fn, i, "lh");  break;
    case IR_LW:  emit_load(out, fn, i, "lw");  break;

    case IR_SB: emit_store(out, fn, i, "sb"); break;
    case IR_SH: emit_store(out, fn, i, "sh"); break;
    case IR_SW: emit_store(out, fn, i, "sw"); break;

    case IR_LDL: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tlw %s, %d($fp)\n", sd, off);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_STL: {
        const char *sa = rs(out, fn, i->a, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tsw %s, %d($fp)\n", sa, off);
        break;
    }

    case IR_ALLOCA: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "$t0") != 0)
            fprintf(out, "\tmove $t0, %s\n", sa);
        fprintf(out, "\taddiu $t0, $t0, 3\n");
        /* andi zero-extends its immediate, so mask through a register */
        fprintf(out, "\tli $t1, -4\n");
        fprintf(out, "\tand $t0, $t0, $t1\n");
        fprintf(out, "\tsubu $sp, $sp, $t0\n");
        fprintf(out, "\tmove %s, $sp\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMPEQ:
    case IR_CMPNE:
    case IR_CMPLTS: case IR_CMPLES: case IR_CMPGTS: case IR_CMPGES:
    case IR_CMPLTU: case IR_CMPLEU: case IR_CMPGTU: case IR_CMPGEU:
        emit_cmp(out, fn, i, i->op);
        break;

    case IR_JMP:
        fprintf(out, "\tj .L%d_%d\n", label_prefix, i->label);
        break;
    case IR_BZ: {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tbeqz %s, .L%d_%d\n",
            sa, label_prefix, i->label);
        break;
    }
    case IR_BNZ: {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tbnez %s, .L%d_%d\n",
            sa, label_prefix, i->label);
        break;
    }
    case IR_LABEL:
        fprintf(out, ".L%d_%d:\n", label_prefix, i->label);
        break;

    case IR_ARG:
        if (narg >= 16)
            die("mips_emit: too many args");
        arg_is_i64[narg] = 0;
        arg_is_float[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_ARG64:
        if (narg >= 16)
            die("mips_emit: too many args");
        arg_is_i64[narg] = 1;
        arg_is_float[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_FARG:
        if (narg >= 16)
            die("mips_emit: too many args");
        arg_is_i64[narg] = 0;
        arg_is_float[narg] = 1;
        arg_fw[narg] = i->imm == FWIDTH_F32;
        arg_temps[narg++] = i->a;
        break;
    case IR_CALL:
        emit_call_flush(out, fn, i, 0, RET_INT);
        break;
    case IR_CALLI:
        emit_call_flush(out, fn, i, 1, RET_INT);
        break;
    case IR_CALL64:
        emit_call_flush(out, fn, i, 0, RET_I64);
        break;
    case IR_CALLI64:
        emit_call_flush(out, fn, i, 1, RET_I64);
        break;
    case IR_FCALL:
        emit_call_flush(out, fn, i, 0, RET_FLOAT);
        break;
    case IR_FCALLI:
        emit_call_flush(out, fn, i, 1, RET_FLOAT);
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
        if (strcmp(sa, "$v0") != 0)
            fprintf(out, "\tmove $v0, %s\n", sa);
        emit_epilogue(out, fn);
        break;
    }
    case IR_RETV64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        if (strcmp(alo, "$v0") != 0)
            fprintf(out, "\tmove $v0, %s\n", alo);
        if (strcmp(ahi, "$v1") != 0)
            fprintf(out, "\tmove $v1, %s\n", ahi);
        emit_epilogue(out, fn);
        break;
    }
    case IR_FRETV: {
#ifdef MIPS_SOFTFLOAT
        if (i->imm == FWIDTH_F32) {                  /* single -> $v0 */
            const char *sa = rs(out, fn, i->a, 0);
            if (strcmp(sa, "$v0") != 0)
                fprintf(out, "\tmove $v0, %s\n", sa);
        } else {                                     /* double -> $v0:$v1 */
            const char *lo = i64_rs_lo(out, fn, i->a, 0);
            if (strcmp(lo, "$v0") != 0)
                fprintf(out, "\tmove $v0, %s\n", lo);
            const char *hi = i64_rs_hi(out, fn, i->a, 1);
            if (strcmp(hi, "$v1") != 0)
                fprintf(out, "\tmove $v1, %s\n", hi);
        }
        emit_epilogue(out, fn);
#else
        int f32 = i->imm == FWIDTH_F32;
        const char *sa = frs_w(out, fn, i->a, 0, f32);
        if (strcmp(sa, "$f0") != 0)
            fprintf(out, "\tmov.%s $f0, %s\n", f32 ? "s" : "d", sa);
        emit_epilogue(out, fn);
#endif
        break;
    }

    case IR_MARK: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);

        fprintf(out, "\tsw $fp, %d($fp)\n", off);
        fprintf(out, "\tsw $sp, %d($fp)\n", off + 4);
        fprintf(out, "\tla $t0, .Lmark%d_%d\n",
            label_prefix, i->label);
        fprintf(out, "\tsw $t0, %d($fp)\n", off + 8);
        fprintf(out, "\taddiu $t0, $fp, %d\n", off);
        fprintf(out, "\tla $t1, __cont_mark_sp\n");
        fprintf(out, "\tsw $t0, 0($t1)\n");
        fprintf(out, "\tli $v0, 0\n");
        fprintf(out, ".Lmark%d_%d:\n", label_prefix, i->label);
        if (strcmp(sd, "$v0") != 0)
            fprintf(out, "\tmove %s, $v0\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CAPTURE: {
        const char *sd = rd(fn, i->dst, 0);
        int k;

        for (k = 0; k < NSAVED; k++)
            fprintf(out, "\taddiu $sp, $sp, -4\n\tsw %s, 0($sp)\n",
                regs[k + 2]);
        fprintf(out, "\tjal __cont_capture\n");
        for (k = NSAVED - 1; k >= 0; k--)
            fprintf(out, "\tlw %s, 0($sp)\n\taddiu $sp, $sp, 4\n",
                regs[k + 2]);
        if (strcmp(sd, "$v0") != 0)
            fprintf(out, "\tmove %s, $v0\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_RESUME: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);

        fprintf(out, "\taddiu $sp, $sp, -8\n");
        fprintf(out, "\tsw %s, 0($sp)\n", sa);
        fprintf(out, "\tsw %s, 4($sp)\n", sb);
        fprintf(out, "\tjal __cont_resume\n");
        break;
    }

#ifdef MIPS_SOFTFLOAT
    /* ---- Floating point (soft-float: every op is a softfloat.c call) ---- */

    case IR_FADD: sf_fbinop(out, fn, i, "__skj_dadd", "__skj_fadd"); break;
    case IR_FSUB: sf_fbinop(out, fn, i, "__skj_dsub", "__skj_fsub"); break;
    case IR_FMUL: sf_fbinop(out, fn, i, "__skj_dmul", "__skj_fmul"); break;
    case IR_FDIV: sf_fbinop(out, fn, i, "__skj_ddiv", "__skj_fdiv"); break;

    case IR_FNEG: case IR_FABS: {
        /* Sign-bit flip (neg) or clear (abs) on the bit pattern: no call.  A
           single touches bit 31 of its word; a double touches bit 31 of its
           high word and copies the low word unchanged.  $t2 holds the mask. */
        int isneg = i->op == IR_FNEG;
        if (i->imm == FWIDTH_F32) {
            const char *sa = rs(out, fn, i->a, 0);
            const char *sd = rd(fn, i->dst, 1);
            if (isneg) {
                fprintf(out, "\tlui $t2, 0x8000\n");        /* 0x80000000 */
                fprintf(out, "\txor %s, %s, $t2\n", sd, sa);
            } else {
                fprintf(out, "\tli $t2, 0x7fffffff\n");
                fprintf(out, "\tand %s, %s, $t2\n", sd, sa);
            }
            wd(out, fn, i->dst, sd);
        } else {
            const char *alo = i64_rs_lo(out, fn, i->a, 0);
            const char *dlo = i64_rd_lo(fn, i->dst, 0);
            if (strcmp(dlo, alo) != 0)
                fprintf(out, "\tmove %s, %s\n", dlo, alo);
            i64_wd_lo(out, fn, i->dst, dlo);
            const char *ahi = i64_rs_hi(out, fn, i->a, 1);
            const char *dhi = i64_rd_hi(fn, i->dst, 1);
            if (isneg) {
                fprintf(out, "\tlui $t2, 0x8000\n");
                fprintf(out, "\txor %s, %s, $t2\n", dhi, ahi);
            } else {
                fprintf(out, "\tli $t2, 0x7fffffff\n");
                fprintf(out, "\tand %s, %s, $t2\n", dhi, ahi);
            }
            i64_wd_hi(out, fn, i->dst, dhi);
        }
        break;
    }

    case IR_FCMPEQ: case IR_FCMPLT: case IR_FCMPLE: {
        int f32 = i->imm == FWIDTH_F32;
        const char *dn = i->op == IR_FCMPEQ ? "__skj_dcmpeq"
                       : i->op == IR_FCMPLT ? "__skj_dcmplt" : "__skj_dcmple";
        const char *fnm = i->op == IR_FCMPEQ ? "__skj_fcmpeq"
                       : i->op == IR_FCMPLT ? "__skj_fcmplt" : "__skj_fcmple";
        if (f32) {
            sf_arg_w(out, fn, i->a, 0);
            sf_arg_w(out, fn, i->b, 1);
            sf_call(out, fnm);
        } else {
            sf_arg_d(out, fn, i->a, 0);
            sf_arg_d(out, fn, i->b, 2);
            sf_call(out, dn);
        }
        sf_ret_w(out, fn, i->dst);                   /* 0/1 in $v0 -> int dst */
        break;
    }

    case IR_ITOF: {
        sf_arg_w(out, fn, i->a, 0);                  /* int in $a0 */
        if (i->imm == FWIDTH_F32) {
            sf_call(out, "__skj_si2f");
            sf_ret_w(out, fn, i->dst);
        } else {
            sf_call(out, "__skj_si2d");
            sf_ret_d(out, fn, i->dst);
        }
        break;
    }

    case IR_FTOI: {
        if (i->imm == FWIDTH_F32) {
            sf_arg_w(out, fn, i->a, 0);
            sf_call(out, "__skj_f2si");
        } else {
            sf_arg_d(out, fn, i->a, 0);
            sf_call(out, "__skj_d2si");
        }
        sf_ret_w(out, fn, i->dst);                   /* int result in $v0 */
        break;
    }

    case IR_F32TOF64: {                              /* single -> double */
        sf_arg_w(out, fn, i->a, 0);
        sf_call(out, "__skj_extendsfdf2");
        sf_ret_d(out, fn, i->dst);
        break;
    }

    case IR_F64TOF32: {                              /* double -> single */
        sf_arg_d(out, fn, i->a, 0);
        sf_call(out, "__skj_truncdfsf2");
        sf_ret_w(out, fn, i->dst);
        break;
    }

    case IR_FLDL: {                                  /* load double/single local */
        int off = slot_offset(fn, i->slot);
        if (i->imm == FWIDTH_F32) {
            const char *sd = rd(fn, i->dst, 0);
            fprintf(out, "\tlw %s, %d($fp)\n", sd, off);
            wd(out, fn, i->dst, sd);
        } else {
            const char *dlo = i64_rd_lo(fn, i->dst, 0);
            fprintf(out, "\tlw %s, %d($fp)\n", dlo, off);
            i64_wd_lo(out, fn, i->dst, dlo);
            const char *dhi = i64_rd_hi(fn, i->dst, 1);
            fprintf(out, "\tlw %s, %d($fp)\n", dhi, off + 4);
            i64_wd_hi(out, fn, i->dst, dhi);
        }
        break;
    }

    case IR_FSTL: {                                  /* store double/single local */
        int off = slot_offset(fn, i->slot);
        if (i->imm == FWIDTH_F32) {
            const char *sa = rs(out, fn, i->a, 0);
            fprintf(out, "\tsw %s, %d($fp)\n", sa, off);
        } else {
            const char *lo = i64_rs_lo(out, fn, i->a, 0);
            fprintf(out, "\tsw %s, %d($fp)\n", lo, off);
            const char *hi = i64_rs_hi(out, fn, i->a, 1);
            fprintf(out, "\tsw %s, %d($fp)\n", hi, off + 4);
        }
        break;
    }

    case IR_FLD: {                                   /* load through address */
        const char *sa = rs(out, fn, i->a, 0);       /* address in $t0/$sX */
        if (i->imm == FWIDTH_F32) {
            const char *sd = rd(fn, i->dst, 1);
            fprintf(out, "\tlw %s, 0(%s)\n", sd, sa);
            wd(out, fn, i->dst, sd);
        } else {
            const char *dlo = i64_rd_lo(fn, i->dst, 1);
            fprintf(out, "\tlw %s, 0(%s)\n", dlo, sa);
            i64_wd_lo(out, fn, i->dst, dlo);
            const char *dhi = i64_rd_hi(fn, i->dst, 1);
            fprintf(out, "\tlw %s, 4(%s)\n", dhi, sa);
            i64_wd_hi(out, fn, i->dst, dhi);
        }
        break;
    }

    case IR_FSD: {                                   /* store through address */
        const char *sa = rs(out, fn, i->a, 0);       /* address */
        if (i->imm == FWIDTH_F32) {
            const char *sb = rs(out, fn, i->b, 1);
            fprintf(out, "\tsw %s, 0(%s)\n", sb, sa);
        } else {
            const char *lo = i64_rs_lo(out, fn, i->b, 1);
            fprintf(out, "\tsw %s, 0(%s)\n", lo, sa);
            const char *hi = i64_rs_hi(out, fn, i->b, 1);
            fprintf(out, "\tsw %s, 4(%s)\n", hi, sa);
        }
        break;
    }

    case IR_FLS: {                                   /* load single -> double */
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tlw $a0, 0(%s)\n", sa);       /* single bits into $a0 */
        sf_call(out, "__skj_extendsfdf2");
        sf_ret_d(out, fn, i->dst);
        break;
    }

    case IR_FSS: {                                   /* double -> single, store */
        sf_arg_d(out, fn, i->b, 0);                  /* double source in $a0:$a1 */
        sf_call(out, "__skj_truncdfsf2");            /* $v0 = single bits */
        {
            const char *sa = rs(out, fn, i->a, 0);   /* address, reload after call */
            fprintf(out, "\tsw $v0, 0(%s)\n", sa);
        }
        break;
    }

    case IR_FLH: {                                   /* load half -> double */
        const char *sa = rs(out, fn, i->a, 0);       /* address */
        fprintf(out, "\tlhu $a0, 0(%s)\n", sa);       /* zero-extended half */
        sf_call(out, "__skj_extendhfsf");             /* $v0 = single bits */
        fprintf(out, "\tmove $a0, $v0\n");
        sf_call(out, "__skj_extendsfdf2");            /* $v0:$v1 = double bits */
        sf_ret_d(out, fn, i->dst);
        break;
    }

    case IR_FSH: {                                   /* double -> half, store */
        sf_arg_d(out, fn, i->b, 0);                  /* double source in $a0:$a1 */
        sf_call(out, "__skj_truncdfsf2");            /* $v0 = single bits */
        fprintf(out, "\tmove $a0, $v0\n");
        sf_call(out, "__skj_truncsfhf");             /* $v0 = half bits */
        {
            const char *sa = rs(out, fn, i->a, 0);   /* address, reload after call */
            fprintf(out, "\tsh $v0, 0(%s)\n", sa);
        }
        break;
    }
#else
    /* ---- Floating point (IEEE 754, hardware coprocessor 1) ---- */

    case IR_FADD: emit_fbinop(out, fn, i, "add"); break;
    case IR_FSUB: emit_fbinop(out, fn, i, "sub"); break;
    case IR_FMUL: emit_fbinop(out, fn, i, "mul"); break;
    case IR_FDIV: emit_fbinop(out, fn, i, "div"); break;
    case IR_FNEG: emit_funop(out, fn, i, "neg"); break;
    case IR_FABS: emit_funop(out, fn, i, "abs"); break;

    case IR_FCMPEQ: case IR_FCMPLT: case IR_FCMPLE: {
        /* MIPS I has no compare-to-register: c.cond.d sets the FP condition
           flag, then a branch materialises the 0/1 bool.  A NaN operand
           makes c.eq/c.lt/c.le false (unordered), matching the other
           backends' "0 on NaN". */
        int f32 = i->imm == FWIDTH_F32;
        const char *sfx = f32 ? "s" : "d";
        const char *fa = frs_w(out, fn, i->a, 0, f32);
        const char *fb = frs_w(out, fn, i->b, 1, f32);
        const char *sd = rd(fn, i->dst, 0);
        const char *op = i->op == IR_FCMPEQ ? "eq"
                       : i->op == IR_FCMPLT ? "lt" : "le";
        int ser = ++fcmp_serial;
        fprintf(out, "\tc.%s.%s %s, %s\n", op, sfx, fa, fb);
        fprintf(out, "\tli %s, 0\n", sd);
        fprintf(out, "\tbc1f .Lfc%d_%d\n", label_prefix, ser);
        fprintf(out, "\tli %s, 1\n", sd);
        fprintf(out, ".Lfc%d_%d:\n", label_prefix, ser);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ITOF: {
        int f32 = i->imm == FWIDTH_F32;
        const char *sa = rs(out, fn, i->a, 0);       /* int */
        const char *sd = frd_w(fn, i->dst, 0);
        fprintf(out, "\tmtc1 %s, $f0\n", sa);         /* int bits -> FPR */
        fprintf(out, "\tcvt.%s.w %s, $f0\n", f32 ? "s" : "d", sd);
        fwd_f(out, fn, i->dst, sd, f32);
        break;
    }

    case IR_FTOI: {
        /* MIPS I has no trunc.w, so set the FCSR rounding mode to
           round-toward-zero for the cvt.w, then restore it. */
        int f32 = i->imm == FWIDTH_F32;
        const char *sa = frs_w(out, fn, i->a, 0, f32);
        const char *sd = rd(fn, i->dst, 0);          /* int */
        fprintf(out, "\tcfc1 $t3, $31\n");            /* save FCSR */
        fprintf(out, "\tli $t4, -4\n");
        fprintf(out, "\tand $t5, $t3, $t4\n");        /* clear RM bits */
        fprintf(out, "\tori $t5, $t5, 1\n");          /* RM = 01 (toward zero) */
        fprintf(out, "\tctc1 $t5, $31\n");
        fprintf(out, "\tcvt.w.%s $f2, %s\n", f32 ? "s" : "d", sa);
        fprintf(out, "\tctc1 $t3, $31\n");            /* restore FCSR */
        fprintf(out, "\tmfc1 %s, $f2\n", sd);         /* int result -> GPR */
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_F32TOF64: {                              /* single -> double */
        const char *sa = frs_w(out, fn, i->a, 0, 1);
        const char *sd = frd_w(fn, i->dst, 0);
        fprintf(out, "\tcvt.d.s %s, %s\n", sd, sa);
        fwd_f(out, fn, i->dst, sd, 0);
        break;
    }

    case IR_F64TOF32: {                              /* double -> single */
        const char *sa = frs_w(out, fn, i->a, 0, 0);
        const char *sd = frd_w(fn, i->dst, 0);
        fprintf(out, "\tcvt.s.d %s, %s\n", sd, sa);
        fwd_f(out, fn, i->dst, sd, 1);
        break;
    }

    case IR_FLDL: {
        int f32 = i->imm == FWIDTH_F32;
        const char *sd = frd_w(fn, i->dst, 0);
        fprintf(out, "\t%s %s, %d($fp)\n", f32 ? "l.s" : "l.d",
            sd, slot_offset(fn, i->slot));
        fwd_f(out, fn, i->dst, sd, f32);
        break;
    }

    case IR_FSTL: {
        int f32 = i->imm == FWIDTH_F32;
        const char *sa = frs_w(out, fn, i->a, 0, f32);
        fprintf(out, "\t%s %s, %d($fp)\n", f32 ? "s.s" : "s.d",
            sa, slot_offset(fn, i->slot));
        break;
    }

    case IR_FLD: {
        int f32 = i->imm == FWIDTH_F32;
        const char *sa = rs(out, fn, i->a, 0);       /* address */
        const char *sd = frd_w(fn, i->dst, 0);
        fprintf(out, "\t%s %s, 0(%s)\n", f32 ? "l.s" : "l.d", sd, sa);
        fwd_f(out, fn, i->dst, sd, f32);
        break;
    }

    case IR_FSD: {
        int f32 = i->imm == FWIDTH_F32;
        const char *sa = rs(out, fn, i->a, 0);       /* address */
        const char *sb = frs_w(out, fn, i->b, 0, f32);
        fprintf(out, "\t%s %s, 0(%s)\n", f32 ? "s.s" : "s.d", sb, sa);
        break;
    }

    case IR_FLS: {                                   /* load single -> double */
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = frd_w(fn, i->dst, 0);
        fprintf(out, "\tlwc1 $f0, 0(%s)\n", sa);
        fprintf(out, "\tcvt.d.s %s, $f0\n", sd);
        fwd_f(out, fn, i->dst, sd, 0);
        break;
    }

    case IR_FSS: {                                   /* double -> single, store */
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = frs_w(out, fn, i->b, 0, 0);
        fprintf(out, "\tcvt.s.d $f0, %s\n", sb);
        fprintf(out, "\tswc1 $f0, 0(%s)\n", sa);
        break;
    }

    case IR_FLH: {                                   /* load half -> double */
        /* No hardware half on MIPS I: the integer-only softfloat helper
           extends the binary16 to a binary32 bit pattern.  It is a gcc-built
           o32 function (arg in $a0, result in $v0) and touches no float or
           callee-saved allocatable register, so it is safe mid-selection. */
        const char *sa = rs(out, fn, i->a, 0);       /* address */
        const char *sd = frd_w(fn, i->dst, 0);
        fprintf(out, "\tlhu $a0, 0(%s)\n", sa);       /* zero-extended half */
        fprintf(out, "\taddiu $sp, $sp, -16\n");      /* o32 arg space */
        fprintf(out, "\tjal __skj_extendhfsf\n");     /* $v0 = single bits */
        fprintf(out, "\taddiu $sp, $sp, 16\n");
        fprintf(out, "\tmtc1 $v0, $f0\n");            /* single bits -> FPR */
        fprintf(out, "\tcvt.d.s %s, $f0\n", sd);
        fwd_f(out, fn, i->dst, sd, 0);
        break;
    }

    case IR_FSH: {                                   /* double -> half, store */
        const char *sb = frs_w(out, fn, i->b, 0, 0); /* double source */
        fprintf(out, "\tcvt.s.d $f0, %s\n", sb);      /* round to single */
        fprintf(out, "\tmfc1 $a0, $f0\n");            /* single bits -> $a0 */
        fprintf(out, "\taddiu $sp, $sp, -16\n");
        fprintf(out, "\tjal __skj_truncsfhf\n");      /* $v0 = half bits */
        fprintf(out, "\taddiu $sp, $sp, 16\n");
        {
            const char *sa = rs(out, fn, i->a, 0);    /* reload addr after call */
            fprintf(out, "\tsh $v0, 0(%s)\n", sa);    /* store low 16 bits */
        }
        break;
    }
#endif /* MIPS_SOFTFLOAT */

    /* ---- I64 opcodes (register pairs) ---- */

    case IR_LIC64: {
        uint64_t val = (uint64_t)i->imm;
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        fprintf(out, "\tli %s, %d\n", dlo, (int)(uint32_t)val);
        fprintf(out, "\tli %s, %d\n", dhi,
            (int)(uint32_t)(val >> 32));
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_ADD64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        fprintf(out, "\taddu %s, %s, %s\n", dlo, alo, blo);
        fprintf(out, "\tsltu $t2, %s, %s\n", dlo, blo);  /* carry out */
        i64_wd_lo(out, fn, i->dst, dlo);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        fprintf(out, "\taddu %s, %s, $t2\n", dhi, ahi);
        const char *bhi = i64_rs_hi(out, fn, i->b, 0);
        fprintf(out, "\taddu %s, %s, %s\n", dhi, dhi, bhi);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_SUB64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        fprintf(out, "\tsltu $t2, %s, %s\n", alo, blo);  /* borrow */
        fprintf(out, "\tsubu %s, %s, %s\n", dlo, alo, blo);
        i64_wd_lo(out, fn, i->dst, dlo);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        fprintf(out, "\tsubu %s, %s, $t2\n", dhi, ahi);
        const char *bhi = i64_rs_hi(out, fn, i->b, 0);
        fprintf(out, "\tsubu %s, %s, %s\n", dhi, dhi, bhi);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_MUL64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        fprintf(out, "\taddiu $sp, $sp, -8\n");
        fprintf(out, "\tsw %s, 0($sp)\n", alo);
        fprintf(out, "\tsw %s, 4($sp)\n",
            i64_rs_hi(out, fn, i->a, 0));
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        fprintf(out, "\taddiu $sp, $sp, -8\n");
        fprintf(out, "\tsw %s, 0($sp)\n", blo);
        fprintf(out, "\tsw %s, 4($sp)\n",
            i64_rs_hi(out, fn, i->b, 1));
        fprintf(out, "\tjal __muldi3\n");
        fprintf(out, "\taddiu $sp, $sp, 16\n");
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, "$v0") != 0)
            fprintf(out, "\tmove %s, $v0\n", dlo);
        if (strcmp(dhi, "$v1") != 0)
            fprintf(out, "\tmove %s, $v1\n", dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_AND64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        fprintf(out, "\tand %s, %s, %s\n", dlo, alo, blo);
        i64_wd_lo(out, fn, i->dst, dlo);
        const char *ahi = i64_rs_hi(out, fn, i->a, 0);
        const char *bhi = i64_rs_hi(out, fn, i->b, 1);
        const char *dhi = i64_rd_hi(fn, i->dst, 0);
        fprintf(out, "\tand %s, %s, %s\n", dhi, ahi, bhi);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_OR64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        fprintf(out, "\tor %s, %s, %s\n", dlo, alo, blo);
        i64_wd_lo(out, fn, i->dst, dlo);
        const char *ahi = i64_rs_hi(out, fn, i->a, 0);
        const char *bhi = i64_rs_hi(out, fn, i->b, 1);
        const char *dhi = i64_rd_hi(fn, i->dst, 0);
        fprintf(out, "\tor %s, %s, %s\n", dhi, ahi, bhi);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_XOR64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s, %s\n", dlo, alo, blo);
        i64_wd_lo(out, fn, i->dst, dlo);
        const char *ahi = i64_rs_hi(out, fn, i->a, 0);
        const char *bhi = i64_rs_hi(out, fn, i->b, 1);
        const char *dhi = i64_rd_hi(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s, %s\n", dhi, ahi, bhi);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_NEG64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        fprintf(out, "\tnegu %s, %s\n", dlo, alo);
        fprintf(out, "\tsltu $t2, $zero, %s\n", alo);  /* borrow if lo != 0 */
        fprintf(out, "\tnegu %s, %s\n", dhi, ahi);
        fprintf(out, "\tsubu %s, %s, $t2\n", dhi, dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_SHL64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        fprintf(out, "\taddiu $sp, $sp, -8\n");
        fprintf(out, "\tsw %s, 4($sp)\n", ahi);
        fprintf(out, "\tsw %s, 0($sp)\n", alo);
        const char *sb = rs(out, fn, i->b, 0);
        fprintf(out, "\taddiu $sp, $sp, -4\n");
        fprintf(out, "\tsw %s, 0($sp)\n", sb);
        fprintf(out, "\tjal __ashldi3\n");
        fprintf(out, "\taddiu $sp, $sp, 12\n");
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, "$v0") != 0)
            fprintf(out, "\tmove %s, $v0\n", dlo);
        if (strcmp(dhi, "$v1") != 0)
            fprintf(out, "\tmove %s, $v1\n", dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_SHRS64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        fprintf(out, "\taddiu $sp, $sp, -8\n");
        fprintf(out, "\tsw %s, 4($sp)\n", ahi);
        fprintf(out, "\tsw %s, 0($sp)\n", alo);
        const char *sb = rs(out, fn, i->b, 0);
        fprintf(out, "\taddiu $sp, $sp, -4\n");
        fprintf(out, "\tsw %s, 0($sp)\n", sb);
        fprintf(out, "\tjal __ashrdi3\n");
        fprintf(out, "\taddiu $sp, $sp, 12\n");
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, "$v0") != 0)
            fprintf(out, "\tmove %s, $v0\n", dlo);
        if (strcmp(dhi, "$v1") != 0)
            fprintf(out, "\tmove %s, $v1\n", dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_SHRU64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        fprintf(out, "\taddiu $sp, $sp, -8\n");
        fprintf(out, "\tsw %s, 4($sp)\n", ahi);
        fprintf(out, "\tsw %s, 0($sp)\n", alo);
        const char *sb = rs(out, fn, i->b, 0);
        fprintf(out, "\taddiu $sp, $sp, -4\n");
        fprintf(out, "\tsw %s, 0($sp)\n", sb);
        fprintf(out, "\tjal __lshrdi3\n");
        fprintf(out, "\taddiu $sp, $sp, 12\n");
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, "$v0") != 0)
            fprintf(out, "\tmove %s, $v0\n", dlo);
        if (strcmp(dhi, "$v1") != 0)
            fprintf(out, "\tmove %s, $v1\n", dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_CMP64EQ: case IR_CMP64NE: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor $t2, %s, %s\n", alo, blo);
        const char *ahi = i64_rs_hi(out, fn, i->a, 0);
        const char *bhi = i64_rs_hi(out, fn, i->b, 1);
        fprintf(out, "\txor $t3, %s, %s\n", ahi, bhi);
        fprintf(out, "\tor $t2, $t2, $t3\n");
        if (i->op == IR_CMP64EQ)
            fprintf(out, "\tsltiu %s, $t2, 1\n", sd);
        else
            fprintf(out, "\tsltu %s, $zero, $t2\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64LTS: case IR_CMP64LES:
    case IR_CMP64GTS: case IR_CMP64GES:
    case IR_CMP64LTU: case IR_CMP64LEU:
    case IR_CMP64GTU: case IR_CMP64GEU: {
        int ser = ++i64cmp_serial;
        const char *ahi = i64_rs_hi(out, fn, i->a, 0);
        const char *bhi = i64_rs_hi(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        const char *hi_op, *lo_op;
        int swap_ab = 0;

        switch (i->op) {
        case IR_CMP64LTS: hi_op = "slt"; lo_op = "sltu"; break;
        case IR_CMP64GTS: hi_op = "slt"; lo_op = "sltu"; swap_ab = 1; break;
        case IR_CMP64LTU: hi_op = "sltu"; lo_op = "sltu"; break;
        case IR_CMP64GTU: hi_op = "sltu"; lo_op = "sltu"; swap_ab = 1; break;
        case IR_CMP64LES: hi_op = "slt"; lo_op = "sltu"; swap_ab = 1; break;
        case IR_CMP64GES: hi_op = "slt"; lo_op = "sltu"; break;
        case IR_CMP64LEU: hi_op = "sltu"; lo_op = "sltu"; swap_ab = 1; break;
        case IR_CMP64GEU: hi_op = "sltu"; lo_op = "sltu"; break;
        default: hi_op = lo_op = "sltu"; break;
        }

        if (swap_ab) {
            const char *tmp;
            tmp = ahi; ahi = bhi; bhi = tmp;
        }

        fprintf(out, "\t%s %s, %s, %s\n", hi_op, sd, ahi, bhi);
        fprintf(out, "\tbne %s, %s, .Lc%d_done\n", ahi, bhi, ser);

        const char *alo = i64_rs_lo(out, fn,
            swap_ab ? i->b : i->a, 0);
        const char *blo = i64_rs_lo(out, fn,
            swap_ab ? i->a : i->b, 1);
        fprintf(out, "\t%s %s, %s, %s\n", lo_op, sd, alo, blo);
        fprintf(out, ".Lc%d_done:\n", ser);

        switch (i->op) {
        case IR_CMP64LES: case IR_CMP64GES:
        case IR_CMP64LEU: case IR_CMP64GEU:
            fprintf(out, "\txori %s, %s, 1\n", sd, sd);
            break;
        default:
            break;
        }

        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_LD64: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        fprintf(out, "\tlw %s, 0(%s)\n", dlo, sa);
        fprintf(out, "\tlw %s, 4(%s)\n", dhi, sa);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_ST64: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        fprintf(out, "\tsw %s, 0(%s)\n", blo, sa);
        const char *bhi = i64_rs_hi(out, fn, i->b, 1);
        fprintf(out, "\tsw %s, 4(%s)\n", bhi, sa);
        break;
    }

    case IR_LDL64: {
        int off = slot_offset(fn, i->slot);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        fprintf(out, "\tlw %s, %d($fp)\n", dlo, off);
        fprintf(out, "\tlw %s, %d($fp)\n", dhi, off + 4);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_STL64: {
        int off = slot_offset(fn, i->slot);
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        fprintf(out, "\tsw %s, %d($fp)\n", alo, off);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        fprintf(out, "\tsw %s, %d($fp)\n", ahi, off + 4);
        break;
    }

    case IR_SEXT64: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, sa) != 0)
            fprintf(out, "\tmove %s, %s\n", dlo, sa);
        fprintf(out, "\tsra %s, %s, 31\n", dhi, dlo);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_ZEXT64: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, sa) != 0)
            fprintf(out, "\tmove %s, %s\n", dlo, sa);
        fprintf(out, "\tli %s, 0\n", dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_TRUNC64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sd, alo) != 0)
            fprintf(out, "\tmove %s, %s\n", sd, alo);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_FUNC:
    case IR_ENDF:
        break;

    default:
        die("mips_emit: unhandled op %d", i->op);
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
    compute_used_fregs(fn);

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
   sized value (8-byte low word first, little-endian), pad to the full size */
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
            fprintf(out, "\t.word %s\n", it->sym);
        else if (it->size == 8) {
            uint64_t b = (uint64_t)it->ival;
            fprintf(out, "\t.word 0x%08x\n", (unsigned)(b & 0xFFFFFFFF));
            fprintf(out, "\t.word 0x%08x\n", (unsigned)(b >> 32));
        } else if (it->size == 2)
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
        default:     elsz = 4; break;   /* incl. IR_F32 (4-byte bit pattern) */
        }

        /* 8-align a double global; an aggregate with an 8-byte field or an
           _Alignas request (g->align) may raise it.  GAS .align is a
           power-of-two exponent (3 = 8 bytes, 2 = 4 bytes). */
        {
            int alb = g->base_type == IR_F64 ? 8 : 4, e = 0;
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
                    fprintf(out, "\t.word %s\n",
                        g->init_syms[k]);
                else if (g->base_type == IR_I64 ||
                         g->base_type == IR_F64) {
                    /* little-endian: low word first (F64 holds the raw
                       64-bit IEEE bit pattern in init_ivals) */
                    uint64_t bits = (uint64_t)g->init_ivals[k];
                    fprintf(out, "\t.word 0x%08x\n",
                        (unsigned)(bits & 0xFFFFFFFF));
                    fprintf(out, "\t.word 0x%08x\n",
                        (unsigned)(bits >> 32));
                } else if (elsz == 1)
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
 * The .set noreorder delay-slot scheduler
 *
 * With `.set reorder` (the default) the assembler fills the load and branch
 * delay slots for us.  With `.set noreorder` the backend must fill them
 * itself, which is what systems code (boot, ISRs, exception handlers) and
 * `volatile` inline assembly need.  This pass takes the assembly the emitter
 * produced for one function and rewrites it so every hazard is covered:
 *
 *   - Branch/jump delay slot: every j/jal/jalr/jr/beqz/bnez/bne gets its
 *     delay slot filled, by hoisting the preceding instruction into it when
 *     that is provably safe, otherwise a nop.  bc1f/bc1t always take a nop,
 *     since the instruction before them is the FP compare's settle gap.
 *   - Load delay slot: after a load (or a coprocessor move, mtc1/mfc1/cfc1),
 *     a nop is inserted when the very next instruction reads the loaded
 *     register.  MIPS I has a one-instruction load delay with no interlock.
 *
 * The mult/div -> mflo/mfhi latency and the div-by-zero check stay the
 * assembler's job: it fills those inside the `mul`/`div`/`rem` macros even
 * under `.set noreorder` (they are single logical operations it expands).
 *
 * Correctness here is by construction from the MIPS I hazard rules.  It is
 * not observable under qemu-mipsel, which models the interlocked pipeline
 * and so runs unfilled slots correctly; it matters on real R2000 silicon.
 ****************************************************************/

static const char *const CT_SET[] = {
    "j", "jal", "jalr", "jr", "b", "beq", "bne", "beqz", "bnez",
    "bc1f", "bc1t", NULL,
};
static const char *const HOIST_SET[] = {
    "addu", "subu", "and", "or", "xor", "nor",
    "sllv", "srav", "srlv", "sll", "sra", "srl",
    "slt", "sltu", "slti", "sltiu",
    "addiu", "andi", "ori", "xori", "lui", "move", "negu", "not",
    "sw", "sb", "sh", NULL,
};
static const char *const LOADD_SET[] = {   /* delayed dest is the 1st operand */
    "lw", "lb", "lbu", "lh", "lhu", "lwc1", "l.d", "l.s", "mfc1", "cfc1", NULL,
};

struct linevec {
    char **v;
    int n, cap;
};

static void
lv_push(struct linevec *L, char *s)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 128;
        L->v = realloc(L->v, L->cap * sizeof(char *));
        if (!L->v)
            die("mips_emit: out of memory scheduling");
    }
    L->v[L->n++] = s;
}

/* an instruction line starts with a tab and is not a directive (.foo) */
static int
is_insn_line(const char *l)
{
    return l[0] == '\t' && l[1] && l[1] != '.';
}

/* does the line's mnemonic equal m (whole token)? */
static int
mnem_is(const char *l, const char *m)
{
    size_t k;

    while (*l == '\t' || *l == ' ')
        l++;
    k = strlen(m);
    if (strncmp(l, m, k) != 0)
        return 0;
    return l[k] == '\0' || l[k] == '\t' || l[k] == ' ';
}

static int
mnem_in(const char *l, const char *const *set)
{
    for (; *set; set++)
        if (mnem_is(l, *set))
            return 1;
    return 0;
}

/* copy the n-th (0-based) $reg token of the line into buf; 1 if found */
static int
nth_reg(const char *l, int n, char *buf, int sz)
{
    const char *p = l;
    int idx = 0;

    while ((p = strchr(p, '$')) != NULL) {
        const char *s = p++;
        int len;
        while (*p && isalnum((unsigned char)*p))
            p++;
        if (idx == n) {
            len = (int)(p - s);
            if (len >= sz)
                len = sz - 1;
            memcpy(buf, s, len);
            buf[len] = '\0';
            return 1;
        }
        idx++;
    }
    return 0;
}

/* does the line mention register reg as a whole token? */
static int
mentions(const char *l, const char *reg)
{
    size_t k = strlen(reg);
    const char *p = l;

    while ((p = strstr(p, reg)) != NULL) {
        if (!isalnum((unsigned char)p[k]))
            return 1;
        p += k;
    }
    return 0;
}

static int
is_delayed_load(const char *l)
{
    return mnem_in(l, LOADD_SET) || mnem_is(l, "mtc1");
}

/* the register a delayed load/move writes (mtc1 rt,fs writes fs, the 2nd) */
static int
load_dest(const char *l, char *buf, int sz)
{
    return nth_reg(l, mnem_is(l, "mtc1") ? 1 : 0, buf, sz);
}

/* Fill branch/jump delay slots (in -> out). */
static void
branch_pass(struct linevec *in, struct linevec *out, char *nopln)
{
    int i;

    for (i = 0; i < in->n; i++) {
        char *li = in->v[i];

        if (is_insn_line(li) && mnem_in(li, CT_SET)) {
            char *slot = NULL;
            int last = out->n - 1;

            if (!mnem_is(li, "bc1f") && !mnem_is(li, "bc1t") &&
                last >= 0 && is_insn_line(out->v[last]) &&
                mnem_in(out->v[last], HOIST_SET) &&
                /* out[last] must not itself be a just-filled delay slot */
                !(last >= 1 && is_insn_line(out->v[last - 1]) &&
                  mnem_in(out->v[last - 1], CT_SET))) {
                char def[24];
                int hasdef = nth_reg(out->v[last], 0, def, sizeof def);
                /* safe only if the branch does not read what the hoisted
                   instruction writes (e.g. jalr $t9 reads its target) */
                if (!hasdef || !mentions(li, def)) {
                    slot = out->v[last];
                    out->n--;
                }
            }
            lv_push(out, li);
            lv_push(out, slot ? slot : nopln);
        } else {
            lv_push(out, li);
        }
    }
}

/* Insert load-delay nops (in -> out). */
static void
load_pass(struct linevec *in, struct linevec *out, char *nopln)
{
    int i;

    for (i = 0; i < in->n; i++) {
        char *li = in->v[i];

        lv_push(out, li);
        if (is_insn_line(li) && is_delayed_load(li)) {
            char dest[24];
            int j = i + 1;

            /* the next executed instruction, past any labels/directives */
            while (j < in->n && !is_insn_line(in->v[j]))
                j++;
            if (load_dest(li, dest, sizeof dest) && j < in->n &&
                mentions(in->v[j], dest))
                lv_push(out, nopln);
        }
    }
}

/* Schedule one function's captured assembly and write it to out. */
static void
sched_emit(FILE *out, char *buf)
{
    struct linevec raw = {0};
    char nopln[] = "\tnop";
    char *p = buf;
    int i;

    /* split into NUL-terminated lines (the trailing newline is dropped and
       re-added on output) */
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        lv_push(&raw, p);
        if (!nl)
            break;
        p = nl + 1;
    }

    /* Walk the lines in segments split at inline-asm boundaries.  A normal
       segment is scheduled (delay slots filled, load hazards nop'd); an asm
       region is copied verbatim, since basic inline asm manages its own
       slots and must not be reordered across.  Scheduling each segment
       alone also stops a hoist from crossing an asm boundary: a branch at a
       segment start sees no preceding instruction. */
    i = 0;
    while (i < raw.n) {
        if (strcmp(raw.v[i], "#skj-asm-begin") == 0) {
            i++;
            while (i < raw.n && strcmp(raw.v[i], "#skj-asm-end") != 0)
                fprintf(out, "%s\n", raw.v[i++]);
            if (i < raw.n)
                i++;        /* skip the end sentinel */
            continue;
        }
        struct linevec seg = {0}, mid = {0}, fin = {0};
        int k;
        while (i < raw.n && strcmp(raw.v[i], "#skj-asm-begin") != 0)
            lv_push(&seg, raw.v[i++]);
        branch_pass(&seg, &mid, nopln);
        load_pass(&mid, &fin, nopln);
        for (k = 0; k < fin.n; k++)
            fprintf(out, "%s\n", fin.v[k]);
        free(seg.v);
        free(mid.v);
        free(fin.v);
    }

    free(raw.v);
}

/****************************************************************
 * Entry point
 ****************************************************************/

void
target_emit(FILE *out, struct ir_program *prog)
{
    struct ir_func *fn;

    g_noreorder = getenv("SKJ_MIPS_NOREORDER") != NULL;

    fputs("# skjegg: generated MIPS I (o32, little-endian) assembly\n", out);
    /* .set reorder: the assembler fills the delay/latency slots.
       .set noreorder: the backend's scheduler (sched_emit) does. */
    fputs(g_noreorder ? "\t.set noreorder\n" : "\t.set reorder\n", out);

    for (fn = prog->funcs; fn; fn = fn->next) {
        if (g_noreorder) {
            char *buf = NULL;
            size_t sz = 0;
            FILE *ms = open_memstream(&buf, &sz);

            if (!ms)
                die("mips_emit: open_memstream failed");
            emit_function(ms, fn);
            fclose(ms);
            sched_emit(out, buf);
            free(buf);
        } else {
            emit_function(out, fn);
        }
    }
    emit_globals(out, prog);
}
