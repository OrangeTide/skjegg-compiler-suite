/* rv_emit.c : RISC-V (RV32IM) back-end, emits GAS-syntax assembly */
/*
 * Stack-based calling convention (matches ColdFire backend pattern):
 *   - Args pushed right-to-left on the stack, 32 bits each; caller pops.
 *   - Return value in a0.
 *   - Callee-save: s0..s11, ra.
 *   - Scratch: t0..t6, a0..a7.
 *
 * Frame layout (after prologue, matches ColdFire convention):
 *
 *      s0 + 8 + 4*i    param i (pushed by caller before jal)
 *      s0 + 4           saved ra
 *      s0 + 0           saved old s0
 *      s0 - locals_size bottom of locals
 *      below locals     spill slots
 *      sp + 0..43       saved s1..s11 (11 words)
 */

#include "ir.h"

#include <stdio.h>
#include <string.h>

#define NSAVED 11

/*
 * Register name table.  Indices 0-1 are scratch (t0, t1).
 * Indices 2-12 are allocatable callee-save (s1-s11), matching
 * FIRST_REG=2, NUM_REGS=11 in regalloc_rv.c.
 */
static const char *regs[] = {
    "t0", "t1",
    "s1", "s2", "s3", "s4", "s5", "s6",
    "s7", "s8", "s9", "s10", "s11",
};

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

    if (slot < fn->nparams)
        return 8 + 4 * slot;
    off = 0;
    for (i = fn->nparams; i <= slot; i++) {
        int sz = (fn->slot_size[i] + 3) & ~3;
        off += sz;
    }
    return -off;
}

static int
frame_size(struct ir_func *fn)
{
    return locals_size(fn) + fn->nspills * 4 + fn->ni64spills * 8;
}

static int
spill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - (fn->temp_spill[temp] + 4);
}

static int
i64spill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - fn->nspills * 4
           - (fn->temp_spill[temp] + 8);
}

/****************************************************************
 * Temp -> register materialisation
 ****************************************************************/

static const char *
rs(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return regs[r];
    fprintf(out, "\tlw %s, %d(s0)\n",
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
    fprintf(out, "\tsw %s, %d(s0)\n",
        reg, spill_byte_offset(fn, t));
}

/****************************************************************
 * I64 register-pair helpers
 *
 * Pair layout (allocated from top of s1..s11):
 *   pair 0: lo=s10 (idx 11), hi=s11 (idx 12)
 *   pair 1: lo=s8  (idx 9),  hi=s9  (idx 10)
 *   pair 2: lo=s6  (idx 7),  hi=s7  (idx 8)
 *   pair 3: lo=s4  (idx 5),  hi=s5  (idx 6)
 ****************************************************************/

static int i64_lo_idx(int pair) { return 11 - 2 * pair; }
static int i64_hi_idx(int pair) { return 12 - 2 * pair; }

static const char *
i64_rs_lo(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int pair = fn->temp_reg[t];
    if (pair >= 0)
        return regs[i64_lo_idx(pair)];
    fprintf(out, "\tlw %s, %d(s0)\n",
            regs[scratch], i64spill_byte_offset(fn, t));
    return regs[scratch];
}

static const char *
i64_rs_hi(FILE *out, struct ir_func *fn, int t, int scratch)
{
    int pair = fn->temp_reg[t];
    if (pair >= 0)
        return regs[i64_hi_idx(pair)];
    fprintf(out, "\tlw %s, %d(s0)\n",
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
    fprintf(out, "\tsw %s, %d(s0)\n",
            reg, i64spill_byte_offset(fn, t));
}

static void
i64_wd_hi(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tsw %s, %d(s0)\n",
            reg, i64spill_byte_offset(fn, t) + 4);
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
emit_cmp(FILE *out, struct ir_func *fn, struct ir_insn *i,
         int op)
{
    const char *sa, *sb, *sd;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    sd = rd(fn, i->dst, 0);

    switch (op) {
    case IR_CMPEQ:
        fprintf(out, "\txor %s, %s, %s\n", sd, sa, sb);
        fprintf(out, "\tseqz %s, %s\n", sd, sd);
        break;
    case IR_CMPNE:
        fprintf(out, "\txor %s, %s, %s\n", sd, sa, sb);
        fprintf(out, "\tsnez %s, %s\n", sd, sd);
        break;
    case IR_CMPLTS:
        fprintf(out, "\tslt %s, %s, %s\n", sd, sa, sb);
        break;
    case IR_CMPLES:
        fprintf(out, "\tslt %s, %s, %s\n", sd, sb, sa);
        fprintf(out, "\txori %s, %s, 1\n", sd, sd);
        break;
    case IR_CMPGTS:
        fprintf(out, "\tslt %s, %s, %s\n", sd, sb, sa);
        break;
    case IR_CMPGES:
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

/****************************************************************
 * Per-instruction emission
 ****************************************************************/

static int arg_temps[16];
static int arg_is_i64[16];
static int narg;
static int label_prefix;
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

static void
emit_prologue(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int total = 8 + frame + NSAVED * 4;
    int k;

    fprintf(out, "\taddi sp, sp, -%d\n", total);
    fprintf(out, "\tsw ra, %d(sp)\n", NSAVED * 4 + frame + 4);
    fprintf(out, "\tsw s0, %d(sp)\n", NSAVED * 4 + frame);
    fprintf(out, "\taddi s0, sp, %d\n", NSAVED * 4 + frame);
    for (k = 0; k < NSAVED; k++)
        fprintf(out, "\tsw %s, %d(sp)\n", regs[k + 2], k * 4);
}

static void
emit_epilogue_no_ret(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int saved_base = -(NSAVED * 4 + frame);
    int k;

    for (k = 0; k < NSAVED; k++)
        fprintf(out, "\tlw %s, %d(s0)\n",
            regs[k + 2], saved_base + k * 4);
    fprintf(out, "\tlw ra, 4(s0)\n");
    fprintf(out, "\tlw t0, 0(s0)\n");
    fprintf(out, "\taddi sp, s0, 8\n");
    fprintf(out, "\tmv s0, t0\n");
}

static void
emit_epilogue(FILE *out, struct ir_func *fn)
{
    emit_epilogue_no_ret(out, fn);
    fprintf(out, "\tret\n");
}

static void
emit_call_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                int indirect, int ret_i64)
{
    int k, push_bytes, off;

    push_bytes = 0;
    for (k = 0; k < narg; k++)
        push_bytes += arg_is_i64[k] ? 8 : 4;

    if (push_bytes > 0)
        fprintf(out, "\taddi sp, sp, -%d\n", push_bytes);
    off = 0;
    for (k = 0; k < narg; k++) {
        if (arg_is_i64[k]) {
            const char *lo = i64_rs_lo(out, fn, arg_temps[k], 0);
            fprintf(out, "\tsw %s, %d(sp)\n", lo, off);
            const char *hi = i64_rs_hi(out, fn, arg_temps[k], 0);
            fprintf(out, "\tsw %s, %d(sp)\n", hi, off + 4);
            off += 8;
        } else {
            const char *sa = rs(out, fn, arg_temps[k], 0);
            fprintf(out, "\tsw %s, %d(sp)\n", sa, off);
            off += 4;
        }
    }
    if (indirect) {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tjalr ra, %s, 0\n", sa);
    } else {
        fprintf(out, "\tjal ra, %s\n", i->sym);
    }
    if (push_bytes > 0)
        fprintf(out, "\taddi sp, sp, %d\n", push_bytes);
    narg = 0;

    if (i->dst >= 0) {
        if (ret_i64) {
            const char *dlo = i64_rd_lo(fn, i->dst, 0);
            const char *dhi = i64_rd_hi(fn, i->dst, 1);
            if (strcmp(dlo, "a0") != 0)
                fprintf(out, "\tmv %s, a0\n", dlo);
            if (strcmp(dhi, "a1") != 0)
                fprintf(out, "\tmv %s, a1\n", dhi);
            i64_wd_lo(out, fn, i->dst, dlo);
            i64_wd_hi(out, fn, i->dst, dhi);
        } else {
            const char *sd = rd(fn, i->dst, 0);
            if (strcmp(sd, "a0") != 0)
                fprintf(out, "\tmv %s, a0\n", sd);
            wd(out, fn, i->dst, sd);
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
        fprintf(out, "\tsw %s, %d(s0)\n", sa, 8 + 4 * k);
    }
    if (indirect) {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tmv t2, %s\n", sa);
    }
    narg = 0;
    emit_epilogue_no_ret(out, fn);
    if (indirect)
        fprintf(out, "\tjr t2\n");
    else
        fprintf(out, "\tj %s\n", i->sym);
}

static void
emit_insn(FILE *out, struct ir_func *fn, struct ir_insn *i)
{
    switch (i->op) {
    case IR_NOP:
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
        fprintf(out, "\taddi %s, s0, %d\n", sd, off);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_MOV: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmv %s, %s\n", sd, sa);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ADD:  emit_binop(out, fn, i, "add");  break;
    case IR_SUB:  emit_binop(out, fn, i, "sub");  break;
    case IR_MUL:  emit_binop(out, fn, i, "mul");  break;
    case IR_AND:  emit_binop(out, fn, i, "and");  break;
    case IR_OR:   emit_binop(out, fn, i, "or");   break;
    case IR_XOR:  emit_binop(out, fn, i, "xor");  break;
    case IR_SHL:  emit_binop(out, fn, i, "sll");  break;
    case IR_SHRS: emit_binop(out, fn, i, "sra");  break;
    case IR_SHRU: emit_binop(out, fn, i, "srl");  break;
    case IR_DIVS: emit_binop(out, fn, i, "div");  break;
    case IR_DIVU: emit_binop(out, fn, i, "divu"); break;
    case IR_MODS: emit_binop(out, fn, i, "rem");  break;
    case IR_MODU: emit_binop(out, fn, i, "remu"); break;

    case IR_NEG: emit_unop(out, fn, i, "neg"); break;
    case IR_NOT: emit_unop(out, fn, i, "not"); break;

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
        fprintf(out, "\tlw %s, %d(s0)\n", sd, off);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_STL: {
        const char *sa = rs(out, fn, i->a, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tsw %s, %d(s0)\n", sa, off);
        break;
    }

    case IR_ALLOCA: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "t0") != 0)
            fprintf(out, "\tmv t0, %s\n", sa);
        fprintf(out, "\taddi t0, t0, 3\n");
        fprintf(out, "\tandi t0, t0, -4\n");
        fprintf(out, "\tsub sp, sp, t0\n");
        fprintf(out, "\tmv %s, sp\n", sd);
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
            die("rv_emit: too many args");
        arg_is_i64[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_ARG64:
        if (narg >= 16)
            die("rv_emit: too many args");
        arg_is_i64[narg] = 1;
        arg_temps[narg++] = i->a;
        break;
    case IR_CALL:
        emit_call_flush(out, fn, i, 0, 0);
        break;
    case IR_CALLI:
        emit_call_flush(out, fn, i, 1, 0);
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
        if (strcmp(sa, "a0") != 0)
            fprintf(out, "\tmv a0, %s\n", sa);
        emit_epilogue(out, fn);
        break;
    }

    case IR_MARK: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);

        fprintf(out, "\tsw s0, %d(s0)\n", off);
        fprintf(out, "\tsw sp, %d(s0)\n", off + 4);
        fprintf(out, "\tla t0, .Lmark%d_%d\n",
            label_prefix, i->label);
        fprintf(out, "\tsw t0, %d(s0)\n", off + 8);
        fprintf(out, "\taddi t0, s0, %d\n", off);
        fprintf(out, "\tla t1, __cont_mark_sp\n");
        fprintf(out, "\tsw t0, 0(t1)\n");
        fprintf(out, "\tli a0, 0\n");
        fprintf(out, ".Lmark%d_%d:\n", label_prefix, i->label);
        if (strcmp(sd, "a0") != 0)
            fprintf(out, "\tmv %s, a0\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CAPTURE: {
        const char *sd = rd(fn, i->dst, 0);
        int k;

        for (k = 0; k < NSAVED; k++)
            fprintf(out, "\taddi sp, sp, -4\n\tsw %s, 0(sp)\n",
                regs[k + 2]);
        fprintf(out, "\tjal ra, __cont_capture\n");
        for (k = NSAVED - 1; k >= 0; k--)
            fprintf(out, "\tlw %s, 0(sp)\n\taddi sp, sp, 4\n",
                regs[k + 2]);
        if (strcmp(sd, "a0") != 0)
            fprintf(out, "\tmv %s, a0\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_RESUME: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);

        fprintf(out, "\taddi sp, sp, -8\n");
        fprintf(out, "\tsw %s, 0(sp)\n", sa);
        fprintf(out, "\tsw %s, 4(sp)\n", sb);
        fprintf(out, "\tjal ra, __cont_resume\n");
        break;
    }

    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
    case IR_FNEG: case IR_FABS:
    case IR_FCMPEQ: case IR_FCMPLT: case IR_FCMPLE:
    case IR_ITOF: case IR_FTOI:
    case IR_FLS: case IR_FLD:
    case IR_FSS: case IR_FSD:
    case IR_FLDL: case IR_FSTL:
    case IR_FRETV: case IR_FCALL: case IR_FCALLI:
    case IR_FARG:
        die("rv_emit: floating point not yet supported on RV32IM");
        break;

    /* ---- I64 opcodes ---- */

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
        fprintf(out, "\tadd %s, %s, %s\n", dlo, alo, blo);
        fprintf(out, "\tsltu t0, %s, %s\n", dlo, blo);
        i64_wd_lo(out, fn, i->dst, dlo);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        fprintf(out, "\tadd %s, %s, t0\n", dhi, ahi);
        const char *bhi = i64_rs_hi(out, fn, i->b, 0);
        fprintf(out, "\tadd %s, %s, %s\n", dhi, dhi, bhi);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_SUB64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        fprintf(out, "\tsltu t0, %s, %s\n", alo, blo);
        fprintf(out, "\tsub %s, %s, %s\n", dlo, alo, blo);
        i64_wd_lo(out, fn, i->dst, dlo);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        fprintf(out, "\tsub %s, %s, t0\n", dhi, ahi);
        const char *bhi = i64_rs_hi(out, fn, i->b, 0);
        fprintf(out, "\tsub %s, %s, %s\n", dhi, dhi, bhi);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_MUL64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        fprintf(out, "\taddi sp, sp, -8\n");
        fprintf(out, "\tsw %s, 0(sp)\n", alo);
        fprintf(out, "\tsw %s, 4(sp)\n",
            i64_rs_hi(out, fn, i->a, 0));
        fprintf(out, "\taddi sp, sp, -8\n");
        fprintf(out, "\tsw %s, 0(sp)\n", blo);
        fprintf(out, "\tsw %s, 4(sp)\n",
            i64_rs_hi(out, fn, i->b, 1));
        fprintf(out, "\tjal ra, __muldi3\n");
        fprintf(out, "\taddi sp, sp, 16\n");
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, "a0") != 0)
            fprintf(out, "\tmv %s, a0\n", dlo);
        if (strcmp(dhi, "a1") != 0)
            fprintf(out, "\tmv %s, a1\n", dhi);
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
        fprintf(out, "\tneg %s, %s\n", dlo, alo);
        fprintf(out, "\tsnez t0, %s\n", alo);
        fprintf(out, "\tneg %s, %s\n", dhi, ahi);
        fprintf(out, "\tsub %s, %s, t0\n", dhi, dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_SHL64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        const char *sb = rs(out, fn, i->b, 0);
        fprintf(out, "\taddi sp, sp, -8\n");
        fprintf(out, "\tsw %s, 0(sp)\n", alo);
        fprintf(out, "\tsw %s, 4(sp)\n", ahi);
        fprintf(out, "\taddi sp, sp, -4\n");
        fprintf(out, "\tsw %s, 0(sp)\n", sb);
        fprintf(out, "\tjal ra, __ashldi3\n");
        fprintf(out, "\taddi sp, sp, 12\n");
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, "a0") != 0)
            fprintf(out, "\tmv %s, a0\n", dlo);
        if (strcmp(dhi, "a1") != 0)
            fprintf(out, "\tmv %s, a1\n", dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_SHRS64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        const char *sb = rs(out, fn, i->b, 0);
        fprintf(out, "\taddi sp, sp, -8\n");
        fprintf(out, "\tsw %s, 0(sp)\n", alo);
        fprintf(out, "\tsw %s, 4(sp)\n", ahi);
        fprintf(out, "\taddi sp, sp, -4\n");
        fprintf(out, "\tsw %s, 0(sp)\n", sb);
        fprintf(out, "\tjal ra, __ashrdi3\n");
        fprintf(out, "\taddi sp, sp, 12\n");
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, "a0") != 0)
            fprintf(out, "\tmv %s, a0\n", dlo);
        if (strcmp(dhi, "a1") != 0)
            fprintf(out, "\tmv %s, a1\n", dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_SHRU64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        const char *sb = rs(out, fn, i->b, 0);
        fprintf(out, "\taddi sp, sp, -8\n");
        fprintf(out, "\tsw %s, 0(sp)\n", alo);
        fprintf(out, "\tsw %s, 4(sp)\n", ahi);
        fprintf(out, "\taddi sp, sp, -4\n");
        fprintf(out, "\tsw %s, 0(sp)\n", sb);
        fprintf(out, "\tjal ra, __lshrdi3\n");
        fprintf(out, "\taddi sp, sp, 12\n");
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, "a0") != 0)
            fprintf(out, "\tmv %s, a0\n", dlo);
        if (strcmp(dhi, "a1") != 0)
            fprintf(out, "\tmv %s, a1\n", dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_CMP64EQ: case IR_CMP64NE: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *blo = i64_rs_lo(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor t0, %s, %s\n", alo, blo);
        const char *ahi = i64_rs_hi(out, fn, i->a, 0);
        const char *bhi = i64_rs_hi(out, fn, i->b, 1);
        fprintf(out, "\txor t1, %s, %s\n", ahi, bhi);
        fprintf(out, "\tor t0, t0, t1\n");
        if (i->op == IR_CMP64EQ)
            fprintf(out, "\tseqz %s, t0\n", sd);
        else
            fprintf(out, "\tsnez %s, t0\n", sd);
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
        fprintf(out, "\tlw %s, %d(s0)\n", dlo, off);
        fprintf(out, "\tlw %s, %d(s0)\n", dhi, off + 4);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_STL64: {
        int off = slot_offset(fn, i->slot);
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        fprintf(out, "\tsw %s, %d(s0)\n", alo, off);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        fprintf(out, "\tsw %s, %d(s0)\n", ahi, off + 4);
        break;
    }

    case IR_SEXT64: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, sa) != 0)
            fprintf(out, "\tmv %s, %s\n", dlo, sa);
        fprintf(out, "\tsrai %s, %s, 31\n", dhi, dlo);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_ZEXT64: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *dlo = i64_rd_lo(fn, i->dst, 0);
        const char *dhi = i64_rd_hi(fn, i->dst, 1);
        if (strcmp(dlo, sa) != 0)
            fprintf(out, "\tmv %s, %s\n", dlo, sa);
        fprintf(out, "\tli %s, 0\n", dhi);
        i64_wd_lo(out, fn, i->dst, dlo);
        i64_wd_hi(out, fn, i->dst, dhi);
        break;
    }

    case IR_TRUNC64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sd, alo) != 0)
            fprintf(out, "\tmv %s, %s\n", sd, alo);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CALL64:
        emit_call_flush(out, fn, i, 0, 1);
        break;
    case IR_CALLI64:
        emit_call_flush(out, fn, i, 1, 1);
        break;

    case IR_RETV64: {
        const char *alo = i64_rs_lo(out, fn, i->a, 0);
        const char *ahi = i64_rs_hi(out, fn, i->a, 1);
        if (strcmp(alo, "a0") != 0)
            fprintf(out, "\tmv a0, %s\n", alo);
        if (strcmp(ahi, "a1") != 0)
            fprintf(out, "\tmv a1, %s\n", ahi);
        emit_epilogue(out, fn);
        break;
    }

    case IR_FUNC:
    case IR_ENDF:
        break;

    default:
        die("rv_emit: unhandled op %d", i->op);
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
        default:     elsz = 4; break;
        }

        fprintf(out, "\t.align 2\n");
        if (!g->is_local)
            fprintf(out, "\t.globl %s\n", g->name);
        fprintf(out, "%s:\n", g->name);
        if (g->init_string) {
            emit_string_bytes(out, g->init_string,
                      g->init_strlen);
        } else if (g->init_count > 0) {
            int k;
            for (k = 0; k < g->init_count; k++) {
                if (g->init_syms && g->init_syms[k])
                    fprintf(out, "\t.word %s\n",
                        g->init_syms[k]);
                else if (g->base_type == IR_I64) {
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
 * Entry point
 ****************************************************************/

void
target_emit(FILE *out, struct ir_program *prog)
{
    struct ir_func *fn;

    fputs("# tinc: generated RISC-V (RV32IM) assembly\n", out);
    for (fn = prog->funcs; fn; fn = fn->next)
        emit_function(out, fn);
    emit_globals(out, prog);
}
