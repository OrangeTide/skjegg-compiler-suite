/* x86_emit.c : i686 (IA-32) back-end, emits NASM-syntax assembly */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
/*
 * Targets flat protected mode (Linux ELF32, DOS DPMI).
 * Requires SSE2 for floating-point (Pentium 4 / Athlon 64 minimum).
 *
 * cdecl calling convention:
 *   - Args pushed right-to-left on the stack, 32 bits each; caller pops.
 *   - Return value in eax (integer) or xmm0 (double).
 *   - Callee-save: ebx, esi, edi, ebp.
 *   - Scratch: eax, ecx, edx, xmm0-xmm7.
 *
 * Frame layout (after prologue):
 *
 *      ebp + 8 + 4*i   param i
 *      ebp + 4          return address
 *      ebp + 0          saved ebp
 *      ebp - locals     bottom of locals
 *                       integer spill slots (4 bytes each)
 *                       float spill slots (8 bytes each)
 *                       i64 spill slots (8 bytes each)
 *      ebp - frame      saved ebx, esi, edi (12 bytes)
 *                  esp   bottom of save area
 *
 * Register allocator mapping (regalloc_x86.c):
 *   Integer: reg 0=ebx, 1=esi, 2=edi
 *   Float:   reg 0=xmm1, 1=xmm2, ..., 6=xmm7
 *   I64:     pair 0 = (esi, edi)
 *   Scratch: eax, ecx, edx (int); xmm0 (float)
 */

#include "ir.h"

#include <stdio.h>
#include <string.h>

static const char *iregs[] = {
    "ebx", "esi", "edi",
};

static const char *scratch[] = {
    "eax", "ecx", "edx",
};

static const char *fpregs[] = {
    "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
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
frame_size(struct ir_func *fn)
{
    return locals_size(fn) + fn->nspills * 4
           + fn->nfspills * 8 + fn->ni64spills * 8;
}

static int
spill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - (fn->temp_spill[temp] + 4);
}

static int
fspill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - fn->nspills * 4
           - (fn->temp_spill[temp] + 8);
}

static int
i64spill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - fn->nspills * 4
           - fn->nfspills * 8 - (fn->temp_spill[temp] + 8);
}

/****************************************************************
 * Temp -> register materialisation
 *
 * Integer scratch registers:
 *   scratch 0 = eax  (first operand reload / dest)
 *   scratch 1 = ecx  (second operand reload)
 *   scratch 2 = edx  (third scratch, used by div/mul)
 ****************************************************************/

static const char *
rs(FILE *out, struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return iregs[r];
    fprintf(out, "\tmov %s, [ebp%+d]\n",
        scratch[scr], spill_byte_offset(fn, t));
    return scratch[scr];
}

static const char *
rd(struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return iregs[r];
    return scratch[scr];
}

static void
wd(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tmov [ebp%+d], %s\n",
        spill_byte_offset(fn, t), reg);
}

static const char *
frs(FILE *out, struct ir_func *fn, int t)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return fpregs[r];
    fprintf(out, "\tmovsd xmm0, [ebp%+d]\n",
        fspill_byte_offset(fn, t));
    return "xmm0";
}

static const char *
frd(struct ir_func *fn, int t)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return fpregs[r];
    return "xmm0";
}

static void
fwd(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tmovsd [ebp%+d], %s\n",
        fspill_byte_offset(fn, t), reg);
}

/****************************************************************
 * I64 register-pair helpers
 *
 * Pair 0 = (esi, edi): hi=esi, lo=edi
 * Scratch for spill reload: eax (hi), edx (lo)
 ****************************************************************/

static void
i64_rs(FILE *out, struct ir_func *fn, int t,
       const char **hi, const char **lo)
{
    int p = fn->temp_reg[t];

    if (p >= 0) {
        *hi = "esi";
        *lo = "edi";
    } else {
        int off = i64spill_byte_offset(fn, t);
        fprintf(out, "\tmov eax, [ebp%+d]\n", off);
        fprintf(out, "\tmov edx, [ebp%+d]\n", off - 4);
        *hi = "eax";
        *lo = "edx";
    }
}

static void
i64_rd(struct ir_func *fn, int t, const char **hi, const char **lo)
{
    int p = fn->temp_reg[t];

    if (p >= 0) {
        *hi = "esi";
        *lo = "edi";
    } else {
        *hi = "eax";
        *lo = "edx";
    }
}

static void
i64_wd(FILE *out, struct ir_func *fn, int t,
       const char *hi, const char *lo)
{
    int off;

    if (fn->temp_reg[t] >= 0)
        return;
    off = i64spill_byte_offset(fn, t);
    fprintf(out, "\tmov [ebp%+d], %s\n", off, hi);
    fprintf(out, "\tmov [ebp%+d], %s\n", off - 4, lo);
}

/*
 * i64_src_b: get the "b" operand of an I64 binary op as a source form
 * that won't conflict with eax:edx. Returns register names or memory
 * operand strings (stored in caller-provided buffers).
 */
static void
i64_src_b(struct ir_func *fn, int t,
          char *hi_buf, char *lo_buf,
          const char **hi, const char **lo)
{
    if (fn->temp_reg[t] >= 0) {
        *hi = "esi";
        *lo = "edi";
    } else {
        int off = i64spill_byte_offset(fn, t);
        sprintf(hi_buf, "[ebp%+d]", off);
        sprintf(lo_buf, "[ebp%+d]", off - 4);
        *hi = hi_buf;
        *lo = lo_buf;
    }
}

/*
 * emit_i64_binop: emit dst = a OP b for 64-bit binary operations.
 * Handles all register/spill combinations correctly by using memory
 * operands for operand b and avoiding double-load into eax:edx.
 *
 * op_carry is the carry variant (e.g. "adc" for "add"), or NULL if
 * no carry is needed (and/or/xor).
 */
static void
emit_i64_binop(FILE *out, struct ir_func *fn, int dst_t, int a_t, int b_t,
               const char *op, const char *op_carry)
{
    int ap = fn->temp_reg[a_t];
    int dp = fn->temp_reg[dst_t];
    const char *bh, *bl;
    char bh_buf[32], bl_buf[32];

    i64_src_b(fn, b_t, bh_buf, bl_buf, &bh, &bl);

    if (dp >= 0) {
        if (ap < 0) {
            int aoff = i64spill_byte_offset(fn, a_t);
            fprintf(out, "\tmov esi, [ebp%+d]\n", aoff);
            fprintf(out, "\tmov edi, [ebp%+d]\n", aoff - 4);
        }
        fprintf(out, "\t%s edi, %s\n", op, bl);
        fprintf(out, "\t%s esi, %s\n", op_carry ? op_carry : op, bh);
    } else {
        if (ap >= 0) {
            fprintf(out, "\tmov eax, esi\n");
            fprintf(out, "\tmov edx, edi\n");
        } else {
            int aoff = i64spill_byte_offset(fn, a_t);
            fprintf(out, "\tmov eax, [ebp%+d]\n", aoff);
            fprintf(out, "\tmov edx, [ebp%+d]\n", aoff - 4);
        }
        fprintf(out, "\t%s edx, %s\n", op, bl);
        fprintf(out, "\t%s eax, %s\n", op_carry ? op_carry : op, bh);
        {
            int doff = i64spill_byte_offset(fn, dst_t);
            fprintf(out, "\tmov [ebp%+d], eax\n", doff);
            fprintf(out, "\tmov [ebp%+d], edx\n", doff - 4);
        }
    }
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
    if (strcmp(sb, sd) == 0 && strcmp(sa, sd) != 0) {
        fprintf(out, "\tmov ecx, %s\n", sb);
        sb = "ecx";
    }
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmov %s, %s\n", sd, sa);
    fprintf(out, "\t%s %s, %s\n", mnem, sd, sb);
    wd(out, fn, i->dst, sd);
}

static void
emit_unop(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *mnem)
{
    const char *sa, *sd;

    sa = rs(out, fn, i->a, 0);
    sd = rd(fn, i->dst, 0);
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmov %s, %s\n", sd, sa);
    fprintf(out, "\t%s %s\n", mnem, sd);
    wd(out, fn, i->dst, sd);
}

static void
emit_cmp(FILE *out, struct ir_func *fn, struct ir_insn *i,
         const char *setcc)
{
    const char *sa, *sb, *sd;

    sa = rs(out, fn, i->a, 0);
    sb = rs(out, fn, i->b, 1);
    fprintf(out, "\tcmp %s, %s\n", sa, sb);
    sd = rd(fn, i->dst, 0);
    fprintf(out, "\t%s al\n", setcc);
    fprintf(out, "\tmovzx %s, al\n", sd);
    fprintf(out, "\tneg %s\n", sd);
    wd(out, fn, i->dst, sd);
}

static void
emit_fbinop(FILE *out, struct ir_func *fn, struct ir_insn *i,
            const char *mnem)
{
    const char *sa, *sb, *sd;

    sa = frs(out, fn, i->a);
    sb = frs(out, fn, i->b);
    sd = frd(fn, i->dst);
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmovsd %s, %s\n", sd, sa);
    fprintf(out, "\t%s %s, %s\n", mnem, sd, sb);
    fwd(out, fn, i->dst, sd);
}

/****************************************************************
 * Per-instruction emission
 ****************************************************************/

static int arg_temps[16];
static int arg_is_float[16];
static int arg_is_i64[16];
static int narg;
static int label_prefix;
static int fcmp_serial;
static int i64cmp_serial;

static void
emit_load(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *size_prefix, int sign_extend)
{
    const char *sa, *sd;

    sa = rs(out, fn, i->a, 0);
    sd = rd(fn, i->dst, 0);
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmov eax, %s\n", sa);
    if (sign_extend)
        fprintf(out, "\tmovsx %s, %s [eax]\n", sd, size_prefix);
    else
        fprintf(out, "\tmovzx %s, %s [eax]\n", sd, size_prefix);
    wd(out, fn, i->dst, sd);
}


static void
emit_prologue(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);

    fprintf(out, "\tpush ebp\n");
    fprintf(out, "\tmov ebp, esp\n");
    if (frame > 0)
        fprintf(out, "\tsub esp, %d\n", frame);
    fprintf(out, "\tpush ebx\n");
    fprintf(out, "\tpush esi\n");
    fprintf(out, "\tpush edi\n");
}

static void
emit_epilogue_no_ret(FILE *out, struct ir_func *fn)
{
    (void)fn;
    fprintf(out, "\tpop edi\n");
    fprintf(out, "\tpop esi\n");
    fprintf(out, "\tpop ebx\n");
    fprintf(out, "\tmov esp, ebp\n");
    fprintf(out, "\tpop ebp\n");
}

static void
emit_epilogue(FILE *out, struct ir_func *fn)
{
    emit_epilogue_no_ret(out, fn);
    fprintf(out, "\tret\n");
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
            const char *sa = frs(out, fn, arg_temps[k]);
            fprintf(out, "\tsub esp, 8\n");
            fprintf(out, "\tmovsd [esp], %s\n", sa);
            arg_bytes += 8;
        } else if (arg_is_i64[k]) {
            const char *hi, *lo;
            i64_rs(out, fn, arg_temps[k], &hi, &lo);
            fprintf(out, "\tpush %s\n", lo);
            fprintf(out, "\tpush %s\n", hi);
            arg_bytes += 8;
        } else {
            const char *sa = rs(out, fn, arg_temps[k], 0);
            fprintf(out, "\tpush %s\n", sa);
            arg_bytes += 4;
        }
    }
    if (indirect) {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tcall %s\n", sa);
    } else {
        fprintf(out, "\tcall %s\n", i->sym);
    }
    if (arg_bytes > 0)
        fprintf(out, "\tadd esp, %d\n", arg_bytes);
    narg = 0;

    if (i->dst >= 0) {
        if (float_ret) {
            const char *sd = frd(fn, i->dst);
            if (strcmp(sd, "xmm0") != 0)
                fprintf(out, "\tmovsd %s, xmm0\n", sd);
            fwd(out, fn, i->dst, sd);
        } else if (i->op == IR_CALL64 || i->op == IR_CALLI64) {
            const char *dhi, *dlo;
            i64_rd(fn, i->dst, &dhi, &dlo);
            if (strcmp(dhi, "eax") != 0)
                fprintf(out, "\tmov %s, eax\n", dhi);
            if (strcmp(dlo, "edx") != 0)
                fprintf(out, "\tmov %s, edx\n", dlo);
            i64_wd(out, fn, i->dst, dhi, dlo);
        } else {
            const char *sd = rd(fn, i->dst, 0);
            if (strcmp(sd, "eax") != 0)
                fprintf(out, "\tmov %s, eax\n", sd);
            wd(out, fn, i->dst, sd);
        }
    }
}

static void
emit_tailcall_flush(FILE *out, struct ir_func *fn, struct ir_insn *i,
                    int indirect)
{
    int k;
    int off = 8;

    for (k = 0; k < narg; k++) {
        if (arg_is_float[k]) {
            const char *sa = frs(out, fn, arg_temps[k]);
            fprintf(out, "\tmovsd [ebp+%d], %s\n", off, sa);
            off += 8;
        } else if (arg_is_i64[k]) {
            const char *hi, *lo;
            i64_rs(out, fn, arg_temps[k], &hi, &lo);
            fprintf(out, "\tmov [ebp+%d], %s\n", off, hi);
            fprintf(out, "\tmov [ebp+%d], %s\n", off + 4, lo);
            off += 8;
        } else {
            const char *sa = rs(out, fn, arg_temps[k], 0);
            fprintf(out, "\tmov [ebp+%d], %s\n", off, sa);
            off += 4;
        }
    }
    if (indirect) {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tmov ecx, %s\n", sa);
    }
    narg = 0;
    emit_epilogue_no_ret(out, fn);
    if (indirect)
        fprintf(out, "\tjmp ecx\n");
    else
        fprintf(out, "\tjmp %s\n", i->sym);
}

static void
emit_insn(FILE *out, struct ir_func *fn, struct ir_insn *i)
{
    switch (i->op) {
    case IR_NOP:
        break;

    case IR_LIC: {
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tmov %s, %ld\n", sd, i->imm);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_LEA: {
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tmov %s, %s\n", sd, i->sym);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ADL: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tlea %s, [ebp%+d]\n", sd, off);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_MOV: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ADD:  emit_binop(out, fn, i, "add");  break;
    case IR_SUB:  emit_binop(out, fn, i, "sub");  break;
    case IR_AND:  emit_binop(out, fn, i, "and");  break;
    case IR_OR:   emit_binop(out, fn, i, "or");   break;
    case IR_XOR:  emit_binop(out, fn, i, "xor");  break;

    case IR_MUL: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        fprintf(out, "\timul %s, %s\n", sd, sb);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_DIVS: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        fprintf(out, "\tcdq\n");
        fprintf(out, "\tidiv ecx\n");
        if (strcmp(sd, "eax") != 0)
            fprintf(out, "\tmov %s, eax\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_DIVU: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        fprintf(out, "\txor edx, edx\n");
        fprintf(out, "\tdiv ecx\n");
        if (strcmp(sd, "eax") != 0)
            fprintf(out, "\tmov %s, eax\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_MODS: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        fprintf(out, "\tcdq\n");
        fprintf(out, "\tidiv ecx\n");
        if (strcmp(sd, "edx") != 0)
            fprintf(out, "\tmov %s, edx\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_MODU: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        fprintf(out, "\txor edx, edx\n");
        fprintf(out, "\tdiv ecx\n");
        if (strcmp(sd, "edx") != 0)
            fprintf(out, "\tmov %s, edx\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_SHL: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        fprintf(out, "\tshl %s, cl\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_SHRS: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        fprintf(out, "\tsar %s, cl\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_SHRU: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        fprintf(out, "\tshr %s, cl\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_NEG: emit_unop(out, fn, i, "neg"); break;
    case IR_NOT: emit_unop(out, fn, i, "not"); break;

    case IR_LB:  emit_load(out, fn, i, "byte", 0);  break;
    case IR_LBS: emit_load(out, fn, i, "byte", 1);  break;
    case IR_LH:  emit_load(out, fn, i, "word", 0);  break;
    case IR_LHS: emit_load(out, fn, i, "word", 1);  break;

    case IR_LW: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tmov %s, [eax]\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_SB: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        fprintf(out, "\tmov [eax], cl\n");
        break;
    }

    case IR_SH: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        fprintf(out, "\tmov [eax], cx\n");
        break;
    }

    case IR_SW: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tmov [eax], %s\n", sb);
        break;
    }

    case IR_LDL: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmov %s, [ebp%+d]\n", sd, off);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_STL: {
        const char *sa = rs(out, fn, i->a, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmov [ebp%+d], %s\n", off, sa);
        break;
    }

    case IR_ALLOCA: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tadd eax, 3\n");
        fprintf(out, "\tand eax, -4\n");
        fprintf(out, "\tsub esp, eax\n");
        fprintf(out, "\tmov %s, esp\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMPEQ:  emit_cmp(out, fn, i, "sete");  break;
    case IR_CMPNE:  emit_cmp(out, fn, i, "setne"); break;
    case IR_CMPLTS: emit_cmp(out, fn, i, "setl");  break;
    case IR_CMPLES: emit_cmp(out, fn, i, "setle"); break;
    case IR_CMPGTS: emit_cmp(out, fn, i, "setg");  break;
    case IR_CMPGES: emit_cmp(out, fn, i, "setge"); break;
    case IR_CMPLTU: emit_cmp(out, fn, i, "setb");  break;
    case IR_CMPLEU: emit_cmp(out, fn, i, "setbe"); break;
    case IR_CMPGTU: emit_cmp(out, fn, i, "seta");  break;
    case IR_CMPGEU: emit_cmp(out, fn, i, "setae"); break;

    case IR_JMP:
        fprintf(out, "\tjmp .L%d_%d\n", label_prefix, i->label);
        break;
    case IR_BZ: {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\ttest %s, %s\n", sa, sa);
        fprintf(out, "\tjz .L%d_%d\n", label_prefix, i->label);
        break;
    }
    case IR_BNZ: {
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\ttest %s, %s\n", sa, sa);
        fprintf(out, "\tjnz .L%d_%d\n", label_prefix, i->label);
        break;
    }
    case IR_LABEL:
        fprintf(out, ".L%d_%d:\n", label_prefix, i->label);
        break;

    case IR_ARG:
        if (narg >= 16)
            die("x86_emit: too many args");
        arg_is_float[narg] = 0;
        arg_is_i64[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_FARG:
        if (narg >= 16)
            die("x86_emit: too many args");
        arg_is_float[narg] = 1;
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

    case IR_RET:
        emit_epilogue(out, fn);
        break;
    case IR_RETV: {
        const char *sa = rs(out, fn, i->a, 0);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        emit_epilogue(out, fn);
        break;
    }
    case IR_FRETV: {
        const char *sa = frs(out, fn, i->a);
        if (strcmp(sa, "xmm0") != 0)
            fprintf(out, "\tmovsd xmm0, %s\n", sa);
        emit_epilogue(out, fn);
        break;
    }

    /* ---- Floating point (SSE2) ---- */

    case IR_FADD: emit_fbinop(out, fn, i, "addsd"); break;
    case IR_FSUB: emit_fbinop(out, fn, i, "subsd"); break;
    case IR_FMUL: emit_fbinop(out, fn, i, "mulsd"); break;
    case IR_FDIV: emit_fbinop(out, fn, i, "divsd"); break;

    case IR_FNEG: {
        const char *sa = frs(out, fn, i->a);
        const char *sd = frd(fn, i->dst);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmovsd %s, %s\n", sd, sa);
        fprintf(out, "\txorpd %s, [__x86_signmask]\n", sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FABS: {
        const char *sa = frs(out, fn, i->a);
        const char *sd = frd(fn, i->dst);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmovsd %s, %s\n", sd, sa);
        fprintf(out, "\tandpd %s, [__x86_absmask]\n", sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FCMPEQ:
    case IR_FCMPLT:
    case IR_FCMPLE: {
        const char *sa, *sb, *sd;
        int id = fcmp_serial++;

        sa = frs(out, fn, i->a);
        sb = frs(out, fn, i->b);
        sd = rd(fn, i->dst, 0);
        if (i->op == IR_FCMPEQ) {
            fprintf(out, "\tucomisd %s, %s\n", sa, sb);
            fprintf(out, "\tmov %s, 0\n", sd);
            fprintf(out, "\tjne .Lfc%d\n", id);
            fprintf(out, "\tjp .Lfc%d\n", id);
            fprintf(out, "\tmov %s, -1\n", sd);
            fprintf(out, ".Lfc%d:\n", id);
        } else if (i->op == IR_FCMPLT) {
            fprintf(out, "\tucomisd %s, %s\n", sb, sa);
            fprintf(out, "\tmov %s, 0\n", sd);
            fprintf(out, "\tjbe .Lfc%d\n", id);
            fprintf(out, "\tmov %s, -1\n", sd);
            fprintf(out, ".Lfc%d:\n", id);
        } else {
            fprintf(out, "\tucomisd %s, %s\n", sb, sa);
            fprintf(out, "\tmov %s, 0\n", sd);
            fprintf(out, "\tjb .Lfc%d\n", id);
            fprintf(out, "\tmov %s, -1\n", sd);
            fprintf(out, ".Lfc%d:\n", id);
        }
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ITOF: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst);
        fprintf(out, "\tcvtsi2sd %s, %s\n", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FTOI: {
        const char *sa = frs(out, fn, i->a);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tcvttsd2si %s, %s\n", sd, sa);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_FLS: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tcvtss2sd %s, [eax]\n", sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FLD: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tmovsd %s, [eax]\n", sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSS: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = frs(out, fn, i->b);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tcvtsd2ss xmm0, %s\n", sb);
        fprintf(out, "\tmovss [eax], xmm0\n");
        break;
    }

    case IR_FSD: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = frs(out, fn, i->b);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tmovsd [eax], %s\n", sb);
        break;
    }

    case IR_FLDL: {
        const char *sd = frd(fn, i->dst);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmovsd %s, [ebp%+d]\n", sd, off);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSTL: {
        const char *sa = frs(out, fn, i->a);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmovsd [ebp%+d], %s\n", off, sa);
        break;
    }

    /* ---- Delimited continuations ---- */
    case IR_MARK: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);

        fprintf(out, "\tmov [ebp%+d], ebp\n", off);
        fprintf(out, "\tmov [ebp%+d], esp\n", off + 4);
        fprintf(out, "\tmov dword [ebp%+d], .Lmark%d_%d\n",
            off + 8, label_prefix, i->label);
        fprintf(out, "\tlea eax, [ebp%+d]\n", off);
        fprintf(out, "\tmov [__cont_mark_sp], eax\n");
        fprintf(out, "\txor eax, eax\n");
        fprintf(out, ".Lmark%d_%d:\n", label_prefix, i->label);
        if (strcmp(sd, "eax") != 0)
            fprintf(out, "\tmov %s, eax\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CAPTURE: {
        const char *sd = rd(fn, i->dst, 0);

        fprintf(out, "\tpush ebx\n");
        fprintf(out, "\tpush esi\n");
        fprintf(out, "\tpush edi\n");
        fprintf(out, "\tcall __cont_capture\n");
        fprintf(out, "\tpop edi\n");
        fprintf(out, "\tpop esi\n");
        fprintf(out, "\tpop ebx\n");
        if (strcmp(sd, "eax") != 0)
            fprintf(out, "\tmov %s, eax\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_RESUME: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);

        fprintf(out, "\tpush %s\n", sb);
        fprintf(out, "\tpush %s\n", sa);
        fprintf(out, "\tcall __cont_resume\n");
        break;
    }

    case IR_FUNC:
    case IR_ENDF:
        break;

    /* ---- I64 opcodes ---- */

    case IR_LIC64: {
        const char *dhi, *dlo;
        int64_t val = (int64_t)i->imm;
        uint32_t hi = (uint32_t)((uint64_t)val >> 32);
        uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);

        i64_rd(fn, i->dst, &dhi, &dlo);
        fprintf(out, "\tmov %s, 0x%x\n", dhi, hi);
        fprintf(out, "\tmov %s, 0x%x\n", dlo, lo);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_ADD64:
        emit_i64_binop(out, fn, i->dst, i->a, i->b, "add", "adc");
        break;

    case IR_SUB64:
        emit_i64_binop(out, fn, i->dst, i->a, i->b, "sub", "sbb");
        break;

    case IR_MUL64: {
        int ap = fn->temp_reg[i->a];
        int bp = fn->temp_reg[i->b];
        int dp = fn->temp_reg[i->dst];

        if (bp >= 0) {
            fprintf(out, "\tpush edi\n");
            fprintf(out, "\tpush esi\n");
        } else {
            int boff = i64spill_byte_offset(fn, i->b);
            fprintf(out, "\tpush dword [ebp%+d]\n", boff - 4);
            fprintf(out, "\tpush dword [ebp%+d]\n", boff);
        }
        if (ap >= 0) {
            fprintf(out, "\tpush edi\n");
            fprintf(out, "\tpush esi\n");
        } else {
            int aoff = i64spill_byte_offset(fn, i->a);
            fprintf(out, "\tpush dword [ebp%+d]\n", aoff - 4);
            fprintf(out, "\tpush dword [ebp%+d]\n", aoff);
        }
        fprintf(out, "\tcall __muldi3\n");
        fprintf(out, "\tadd esp, 16\n");
        if (dp >= 0) {
            fprintf(out, "\tmov esi, eax\n");
            fprintf(out, "\tmov edi, edx\n");
        } else {
            int doff = i64spill_byte_offset(fn, i->dst);
            fprintf(out, "\tmov [ebp%+d], eax\n", doff);
            fprintf(out, "\tmov [ebp%+d], edx\n", doff - 4);
        }
        break;
    }

    case IR_AND64:
        emit_i64_binop(out, fn, i->dst, i->a, i->b, "and", NULL);
        break;

    case IR_OR64:
        emit_i64_binop(out, fn, i->dst, i->a, i->b, "or", NULL);
        break;

    case IR_XOR64:
        emit_i64_binop(out, fn, i->dst, i->a, i->b, "xor", NULL);
        break;

    case IR_SHL64:
    case IR_SHRS64:
    case IR_SHRU64: {
        const char *ahi, *alo, *dhi, *dlo;
        const char *sb;
        const char *func;

        i64_rs(out, fn, i->a, &ahi, &alo);
        sb = rs(out, fn, i->b, 1);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (i->op == IR_SHL64)
            func = "__ashldi3";
        else if (i->op == IR_SHRS64)
            func = "__ashrdi3";
        else
            func = "__lshrdi3";
        fprintf(out, "\tpush %s\n", sb);
        fprintf(out, "\tpush %s\n", alo);
        fprintf(out, "\tpush %s\n", ahi);
        fprintf(out, "\tcall %s\n", func);
        fprintf(out, "\tadd esp, 12\n");
        if (strcmp(dhi, "eax") != 0)
            fprintf(out, "\tmov %s, eax\n", dhi);
        if (strcmp(dlo, "edx") != 0)
            fprintf(out, "\tmov %s, edx\n", dlo);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_NEG64: {
        const char *ahi, *alo, *dhi, *dlo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (strcmp(dlo, alo) != 0)
            fprintf(out, "\tmov %s, %s\n", dlo, alo);
        if (strcmp(dhi, ahi) != 0)
            fprintf(out, "\tmov %s, %s\n", dhi, ahi);
        fprintf(out, "\tneg %s\n", dlo);
        fprintf(out, "\tadc %s, 0\n", dhi);
        fprintf(out, "\tneg %s\n", dhi);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_CMP64EQ:
    case IR_CMP64NE: {
        const char *ahi, *alo, *bhi, *blo, *sd;
        char bh_buf[32], bl_buf[32];
        int id = i64cmp_serial++;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_src_b(fn, i->b, bh_buf, bl_buf, &bhi, &blo);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s\n", sd, sd);
        fprintf(out, "\tcmp %s, %s\n", ahi, bhi);
        fprintf(out, "\tjne .Li64c%d\n", id);
        fprintf(out, "\tcmp %s, %s\n", alo, blo);
        fprintf(out, ".Li64c%d:\n", id);
        fprintf(out, "\t%s al\n",
            i->op == IR_CMP64EQ ? "sete" : "setne");
        fprintf(out, "\tmovzx %s, al\n", sd);
        fprintf(out, "\tneg %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64LTS: {
        const char *ahi, *alo, *bhi, *blo, *sd;
        char bh_buf[32], bl_buf[32];
        int id = i64cmp_serial++;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_src_b(fn, i->b, bh_buf, bl_buf, &bhi, &blo);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s\n", sd, sd);
        fprintf(out, "\tcmp %s, %s\n", ahi, bhi);
        fprintf(out, "\tjl .Li64c%d_t\n", id);
        fprintf(out, "\tjg .Li64c%d_d\n", id);
        fprintf(out, "\tcmp %s, %s\n", alo, blo);
        fprintf(out, "\tjae .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmov %s, -1\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64LES: {
        const char *ahi, *alo, *bhi, *blo, *sd;
        char bh_buf[32], bl_buf[32];
        int id = i64cmp_serial++;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_src_b(fn, i->b, bh_buf, bl_buf, &bhi, &blo);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s\n", sd, sd);
        fprintf(out, "\tcmp %s, %s\n", ahi, bhi);
        fprintf(out, "\tjl .Li64c%d_t\n", id);
        fprintf(out, "\tjg .Li64c%d_d\n", id);
        fprintf(out, "\tcmp %s, %s\n", alo, blo);
        fprintf(out, "\tja .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmov %s, -1\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64GTS: {
        const char *ahi, *alo, *bhi, *blo, *sd;
        char bh_buf[32], bl_buf[32];
        int id = i64cmp_serial++;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_src_b(fn, i->b, bh_buf, bl_buf, &bhi, &blo);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s\n", sd, sd);
        fprintf(out, "\tcmp %s, %s\n", ahi, bhi);
        fprintf(out, "\tjg .Li64c%d_t\n", id);
        fprintf(out, "\tjl .Li64c%d_d\n", id);
        fprintf(out, "\tcmp %s, %s\n", alo, blo);
        fprintf(out, "\tjbe .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmov %s, -1\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64GES: {
        const char *ahi, *alo, *bhi, *blo, *sd;
        char bh_buf[32], bl_buf[32];
        int id = i64cmp_serial++;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_src_b(fn, i->b, bh_buf, bl_buf, &bhi, &blo);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s\n", sd, sd);
        fprintf(out, "\tcmp %s, %s\n", ahi, bhi);
        fprintf(out, "\tjg .Li64c%d_t\n", id);
        fprintf(out, "\tjl .Li64c%d_d\n", id);
        fprintf(out, "\tcmp %s, %s\n", alo, blo);
        fprintf(out, "\tjb .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmov %s, -1\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64LTU: {
        const char *ahi, *alo, *bhi, *blo, *sd;
        char bh_buf[32], bl_buf[32];
        int id = i64cmp_serial++;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_src_b(fn, i->b, bh_buf, bl_buf, &bhi, &blo);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s\n", sd, sd);
        fprintf(out, "\tcmp %s, %s\n", ahi, bhi);
        fprintf(out, "\tjb .Li64c%d_t\n", id);
        fprintf(out, "\tja .Li64c%d_d\n", id);
        fprintf(out, "\tcmp %s, %s\n", alo, blo);
        fprintf(out, "\tjae .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmov %s, -1\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64LEU: {
        const char *ahi, *alo, *bhi, *blo, *sd;
        char bh_buf[32], bl_buf[32];
        int id = i64cmp_serial++;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_src_b(fn, i->b, bh_buf, bl_buf, &bhi, &blo);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s\n", sd, sd);
        fprintf(out, "\tcmp %s, %s\n", ahi, bhi);
        fprintf(out, "\tjb .Li64c%d_t\n", id);
        fprintf(out, "\tja .Li64c%d_d\n", id);
        fprintf(out, "\tcmp %s, %s\n", alo, blo);
        fprintf(out, "\tja .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmov %s, -1\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64GTU: {
        const char *ahi, *alo, *bhi, *blo, *sd;
        char bh_buf[32], bl_buf[32];
        int id = i64cmp_serial++;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_src_b(fn, i->b, bh_buf, bl_buf, &bhi, &blo);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s\n", sd, sd);
        fprintf(out, "\tcmp %s, %s\n", ahi, bhi);
        fprintf(out, "\tja .Li64c%d_t\n", id);
        fprintf(out, "\tjb .Li64c%d_d\n", id);
        fprintf(out, "\tcmp %s, %s\n", alo, blo);
        fprintf(out, "\tjbe .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmov %s, -1\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64GEU: {
        const char *ahi, *alo, *bhi, *blo, *sd;
        char bh_buf[32], bl_buf[32];
        int id = i64cmp_serial++;

        i64_rs(out, fn, i->a, &ahi, &alo);
        i64_src_b(fn, i->b, bh_buf, bl_buf, &bhi, &blo);
        sd = rd(fn, i->dst, 0);
        fprintf(out, "\txor %s, %s\n", sd, sd);
        fprintf(out, "\tcmp %s, %s\n", ahi, bhi);
        fprintf(out, "\tja .Li64c%d_t\n", id);
        fprintf(out, "\tjb .Li64c%d_d\n", id);
        fprintf(out, "\tcmp %s, %s\n", alo, blo);
        fprintf(out, "\tjb .Li64c%d_d\n", id);
        fprintf(out, ".Li64c%d_t:\n", id);
        fprintf(out, "\tmov %s, -1\n", sd);
        fprintf(out, ".Li64c%d_d:\n", id);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_LD64: {
        const char *sa;
        const char *dhi, *dlo;

        sa = rs(out, fn, i->a, 0);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tmov %s, [eax]\n", dhi);
        fprintf(out, "\tmov %s, [eax+4]\n", dlo);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_ST64: {
        const char *sa;
        const char *bhi, *blo;

        sa = rs(out, fn, i->a, 0);
        i64_rs(out, fn, i->b, &bhi, &blo);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tmov [eax], %s\n", bhi);
        fprintf(out, "\tmov [eax+4], %s\n", blo);
        break;
    }

    case IR_LDL64: {
        const char *dhi, *dlo;
        int off = slot_offset(fn, i->slot);

        i64_rd(fn, i->dst, &dhi, &dlo);
        fprintf(out, "\tmov %s, [ebp%+d]\n", dhi, off);
        fprintf(out, "\tmov %s, [ebp%+d]\n", dlo, off + 4);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_STL64: {
        const char *ahi, *alo;
        int off = slot_offset(fn, i->slot);

        i64_rs(out, fn, i->a, &ahi, &alo);
        fprintf(out, "\tmov [ebp%+d], %s\n", off, ahi);
        fprintf(out, "\tmov [ebp%+d], %s\n", off + 4, alo);
        break;
    }

    case IR_SEXT64: {
        const char *sa;
        const char *dhi, *dlo;

        sa = rs(out, fn, i->a, 0);
        i64_rd(fn, i->dst, &dhi, &dlo);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tcdq\n");
        if (strcmp(dlo, "eax") != 0)
            fprintf(out, "\tmov %s, eax\n", dlo);
        if (strcmp(dhi, "edx") != 0)
            fprintf(out, "\tmov %s, edx\n", dhi);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_ZEXT64: {
        const char *sa;
        const char *dhi, *dlo;

        sa = rs(out, fn, i->a, 0);
        i64_rd(fn, i->dst, &dhi, &dlo);
        fprintf(out, "\tmov %s, %s\n", dlo, sa);
        fprintf(out, "\txor %s, %s\n", dhi, dhi);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_TRUNC64: {
        const char *ahi, *alo;
        const char *sd;

        i64_rs(out, fn, i->a, &ahi, &alo);
        sd = rd(fn, i->dst, 0);
        if (strcmp(alo, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, alo);
        wd(out, fn, i->dst, sd);
        (void)ahi;
        break;
    }

    case IR_ARG64:
        if (narg >= 16)
            die("x86_emit: too many args");
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
        const char *ahi, *alo;

        i64_rs(out, fn, i->a, &ahi, &alo);
        if (strcmp(ahi, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", ahi);
        if (strcmp(alo, "edx") != 0)
            fprintf(out, "\tmov edx, %s\n", alo);
        emit_epilogue(out, fn);
        break;
    }

    default:
        die("x86_emit: unhandled op %d", i->op);
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

    fprintf(out, "\nsection .text\n");
    if (!fn->is_local)
        fprintf(out, "global %s\n", fn->name);
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
}

/****************************************************************
 * Globals
 ****************************************************************/

static void
emit_string_bytes(FILE *out, const char *s, int n)
{
    int k;

    fputs("\tdb ", out);
    for (k = 0; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (k > 0)
            fputs(", ", out);
        if (c >= 0x20 && c < 0x7f && c != '\'' && c != '\\')
            fprintf(out, "'%c'", c);
        else
            fprintf(out, "0x%02x", c);
    }
    fputc('\n', out);
}

static void
emit_globals(FILE *out, struct ir_program *prog)
{
    struct ir_global *g;

    fputs("\nsection .data\n", out);
    for (g = prog->globals; g; g = g->next) {
        int elsz;

        switch (g->base_type) {
        case IR_I8:  elsz = 1; break;
        case IR_I16: elsz = 2; break;
        case IR_F64: elsz = 8; break;
        case IR_I64: elsz = 8; break;
        default:     elsz = 4; break;
        }

        fprintf(out, "align 4\n");
        if (!g->is_local)
            fprintf(out, "global %s\n", g->name);
        fprintf(out, "%s:\n", g->name);
        if (g->init_string) {
            emit_string_bytes(out, g->init_string,
                      g->init_strlen);
        } else if (g->init_count > 0) {
            int k;
            for (k = 0; k < g->init_count; k++) {
                if (g->init_syms && g->init_syms[k])
                    fprintf(out, "\tdd %s\n",
                        g->init_syms[k]);
                else if (g->base_type == IR_F64 ||
                     g->base_type == IR_I64) {
                    uint64_t bits = (uint64_t)g->init_ivals[k];
                    fprintf(out, "\tdd 0x%08x\n",
                        (unsigned)(bits & 0xFFFFFFFF));
                    fprintf(out, "\tdd 0x%08x\n",
                        (unsigned)(bits >> 32));
                } else if (elsz == 1)
                    fprintf(out, "\tdb %" PRId64 "\n",
                        g->init_ivals[k]);
                else if (elsz == 2)
                    fprintf(out, "\tdw %" PRId64 "\n",
                        g->init_ivals[k]);
                else
                    fprintf(out, "\tdd %" PRId64 "\n",
                        g->init_ivals[k]);
            }
            if (g->arr_size > g->init_count)
                fprintf(out, "\ttimes %d db 0\n",
                    (g->arr_size - g->init_count) * elsz);
        } else {
            int sz = (g->arr_size > 0)
                 ? g->arr_size * elsz
                 : elsz;
            fprintf(out, "\ttimes %d db 0\n", sz);
        }
    }
}

static void
emit_fp_constants(FILE *out)
{
    fputs("\nsection .rodata\n", out);
    fputs("align 16\n", out);
    fputs("__x86_signmask:\n", out);
    fputs("\tdd 0x00000000, 0x80000000, 0x00000000, 0x80000000\n", out);
    fputs("__x86_absmask:\n", out);
    fputs("\tdd 0xFFFFFFFF, 0x7FFFFFFF, 0xFFFFFFFF, 0x7FFFFFFF\n", out);
}

/****************************************************************
 * Extern declarations (NASM requires explicit extern for undefined symbols)
 ****************************************************************/

static int
is_defined(struct ir_program *prog, const char *sym)
{
    struct ir_func *fn;
    struct ir_global *g;

    for (fn = prog->funcs; fn; fn = fn->next)
        if (strcmp(fn->name, sym) == 0)
            return 1;
    for (g = prog->globals; g; g = g->next)
        if (strcmp(g->name, sym) == 0)
            return 1;
    return 0;
}

static void
emit_extern_once(FILE *out, const char **seen, int *nseen, const char *sym)
{
    int k;

    for (k = 0; k < *nseen; k++)
        if (strcmp(seen[k], sym) == 0)
            return;
    if (*nseen < 256)
        seen[(*nseen)++] = sym;
    fprintf(out, "extern %s\n", sym);
}

static void
emit_externs(FILE *out, struct ir_program *prog)
{
    struct ir_func *fn;
    struct ir_insn *i;
    const char *seen[256];
    int nseen = 0;

    for (fn = prog->funcs; fn; fn = fn->next) {
        for (i = fn->head; i; i = i->next) {
            if (i->sym &&
                (i->op == IR_CALL || i->op == IR_TAILCALL ||
                 i->op == IR_FCALL || i->op == IR_CALL64)) {
                if (!is_defined(prog, i->sym))
                    emit_extern_once(out, seen, &nseen, i->sym);
            }
            switch (i->op) {
            case IR_MUL64:
                emit_extern_once(out, seen, &nseen, "__muldi3");
                break;
            case IR_SHL64:
                emit_extern_once(out, seen, &nseen, "__ashldi3");
                break;
            case IR_SHRS64:
                emit_extern_once(out, seen, &nseen, "__ashrdi3");
                break;
            case IR_SHRU64:
                emit_extern_once(out, seen, &nseen, "__lshrdi3");
                break;
            case IR_CAPTURE:
                emit_extern_once(out, seen, &nseen, "__cont_capture");
                break;
            case IR_RESUME:
                emit_extern_once(out, seen, &nseen, "__cont_resume");
                break;
            case IR_MARK:
                emit_extern_once(out, seen, &nseen, "__cont_mark_sp");
                break;
            default:
                break;
            }
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

    fputs("; generated i686 (IA-32) assembly, NASM syntax\n", out);
    emit_externs(out, prog);
    for (fn = prog->funcs; fn; fn = fn->next)
        emit_function(out, fn);
    emit_globals(out, prog);
    emit_fp_constants(out);
}
