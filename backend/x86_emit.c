/* x86_emit.c : x86 back-end, emits NASM-syntax assembly */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
/*
 * x86-64, Linux ELF64, ILP32-style, emitting the toolkit stack calling
 * convention (the SysV register convention is backend/x86_select.c).  Every
 * address is a 32-bit value in a register's 32-bit view (a 32-bit write zeroes
 * the upper half), and the 64-bit view of the same register is the
 * zero-extended (< 4GB) address fed to a load/store.  A `long long` (IR_I64)
 * is a single native 64-bit register, not a pair, so there are no __muldi3
 * helpers.  (This file once also targeted i686 via X86_BITS=32; that target
 * was dropped, so it is x86-64 only.)
 *
 * Stack-based calling convention:
 *   - Args pushed right-to-left, one word each (WORD bytes); caller pops.
 *   - Return value in eax/rax (integer) or xmm0 (double).
 *   - Callee-save allocatable: rbx, r12-r15.  Scratch: rax/rcx/rdx, xmm0.
 *
 * Frame layout (after prologue), with rbp the frame pointer:
 *
 *      rbp + 2*WORD + WORD*i  param i
 *      rbp + WORD             return address
 *      rbp + 0                saved old rbp
 *      rbp - locals           bottom of locals / spill slots
 */

#include "ir.h"

#include <stdio.h>
#include <string.h>



#define WORD 8
#define BP "rbp"
#define SP "rsp"
#define CXP "rcx"

/* 32-bit views (integer core / values) and 64-bit views (addresses / i64) */
static const char *iregs[] = { "ebx", "r12d", "r13d", "r14d", "r15d" };
static const char *qregs[] = { "rbx", "r12",  "r13",  "r14",  "r15"  };
static const char *scratch[]  = { "eax", "ecx", "edx" };
static const char *qscratch[] = { "rax", "rcx", "rdx" };


static const char *fpregs[] = {
    "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
};


/* This file emits the toolkit stack calling convention (and every x86-32
   target); the SysV register convention moved to backend/x86_select.c.  A
   stack-convention function reserves no register-param home slots and no
   register save area, so these are always 0.  They are kept (rather than folded
   away) so the frame-layout call sites read the same on every target. */
static int
param_home(struct ir_func *fn)
{
    (void)fn;
    return 0;
}

static int
va_save(struct ir_func *fn)
{
    (void)fn;
    return 0;
}

/* total reserved area between rbp and the locals (param homes + va save) */
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
        int sz = (fn->slot_size[i] + (WORD - 1)) & ~(WORD - 1);
        s += sz;
    }
    return s;
}

static int
slot_offset(struct ir_func *fn, int slot)
{
    int i, off;

    if (slot < fn->nparams) {
        off = 2 * WORD;
        for (i = 0; i < slot; i++) {
            int sz = (fn->slot_size[i] + (WORD - 1)) & ~(WORD - 1);
            off += sz;
        }
        return off;
    }
    off = 0;
    for (i = fn->nparams; i <= slot; i++) {
        int sz = (fn->slot_size[i] + (WORD - 1)) & ~(WORD - 1);
        off += sz;
    }
    return -frame_reserve(fn) - off;
}

static int
frame_size(struct ir_func *fn)
{
    return frame_reserve(fn) + locals_size(fn) + fn->nspills * WORD
           + fn->nfspills * 8 + fn->ni64spills * 8;
}

static int
spill_byte_offset(struct ir_func *fn, int temp)
{
    return -frame_reserve(fn) - locals_size(fn) - (fn->temp_spill[temp] + WORD);
}

static int
fspill_byte_offset(struct ir_func *fn, int temp)
{
    return -frame_reserve(fn) - locals_size(fn) - fn->nspills * WORD
           - (fn->temp_spill[temp] + 8);
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
    fprintf(out, "\tmov %s, [" BP "%+d]\n",
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

/*
 * Pointer-width address register for a temp holding an address.  On x86-64
 * (ILP32) a reload zero-extends the 32-bit address into the full register and
 * we return its 64-bit view for use as a [base]; on x86-32 this is just rs().
 */
static const char *
ars(FILE *out, struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return qregs[r];
    fprintf(out, "\tmov %s, [" BP "%+d]\n",
        scratch[scr], spill_byte_offset(fn, t));
    return qscratch[scr];
}

/* 64-bit view of a temp's register (for IR_I64 opcodes). */
static const char *
rsq(FILE *out, struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return qregs[r];
    fprintf(out, "\tmov %s, [" BP "%+d]\n",
        qscratch[scr], spill_byte_offset(fn, t));
    return qscratch[scr];
}

static const char *
rdq(struct ir_func *fn, int t, int scr)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return qregs[r];
    return qscratch[scr];
}

static void
wdq(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tmov [" BP "%+d], %s\n",
        spill_byte_offset(fn, t), reg);
}

static void
wd(FILE *out, struct ir_func *fn, int t, const char *reg)
{
    if (fn->temp_reg[t] >= 0)
        return;
    fprintf(out, "\tmov [" BP "%+d], %s\n",
        spill_byte_offset(fn, t), reg);
}

static const char *
frs(FILE *out, struct ir_func *fn, int t)
{
    int r = fn->temp_reg[t];

    if (r >= 0)
        return fpregs[r];
    fprintf(out, "\tmovsd xmm0, [" BP "%+d]\n",
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
    fprintf(out, "\tmovsd [" BP "%+d], %s\n",
        fspill_byte_offset(fn, t), reg);
}

/****************************************************************
 * I64 register-pair helpers (x86-32 only; x86-64 does i64 natively)
 *
 * Pair 0 = (esi, edi): hi=esi, lo=edi
 * Scratch for spill reload: eax (hi), edx (lo)
 ****************************************************************/

/****************************************************************
 * Binary / unary / compare helpers
 ****************************************************************/

/* native 64-bit two-address binop: dst = a OP b (q-registers) */
static void
emit_qbinop(FILE *out, struct ir_func *fn, struct ir_insn *i,
            const char *mnem)
{
    const char *sa, *sb, *sd;

    sa = rsq(out, fn, i->a, 0);
    sb = rsq(out, fn, i->b, 1);
    sd = rdq(fn, i->dst, 0);
    if (strcmp(sb, sd) == 0 && strcmp(sa, sd) != 0) {
        fprintf(out, "\tmov rcx, %s\n", sb);
        sb = "rcx";
    }
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmov %s, %s\n", sd, sa);
    fprintf(out, "\t%s %s, %s\n", mnem, sd, sb);
    wdq(out, fn, i->dst, sd);
}

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
    fprintf(out, "\tmovzx %s, al\n", sd);   /* 0/1, matching the other backends */
    wd(out, fn, i->dst, sd);
}

/* base is the SSE mnemonic without its precision suffix ("add", "sub", ...);
 * the width tag (i->imm == FWIDTH_F32) picks the ss/sd form. */
static void
emit_fbinop(FILE *out, struct ir_func *fn, struct ir_insn *i,
            const char *base)
{
    const char *sa, *sb, *sd;
    const char *sfx = i->imm == FWIDTH_F32 ? "ss" : "sd";

    sa = frs(out, fn, i->a);
    sb = frs(out, fn, i->b);
    sd = frd(fn, i->dst);
    if (strcmp(sa, sd) != 0)
        fprintf(out, "\tmovsd %s, %s\n", sd, sa);
    fprintf(out, "\t%s%s %s, %s\n", base, sfx, sd, sb);
    fwd(out, fn, i->dst, sd);
}

/****************************************************************
 * Per-instruction emission
 ****************************************************************/

/* Maximum arguments in one call. Mirrors the C front end's cap (lower.c:
   "too many arguments"); the two must agree, or a call the front end accepts
   would be rejected here. */
#define X86_MAX_ARGS 32

static int arg_temps[X86_MAX_ARGS];
static int arg_is_float[X86_MAX_ARGS];
static int arg_is_i64[X86_MAX_ARGS];
static int arg_mem[X86_MAX_ARGS];   /* >0: a by-value MEMORY struct arg of this size */
static int narg;
static int label_prefix;
static int fcmp_serial;

static void
emit_load(FILE *out, struct ir_func *fn, struct ir_insn *i,
          const char *size_prefix, int sign_extend)
{
    const char *sa, *sd;

    sa = ars(out, fn, i->a, 0);
    sd = rd(fn, i->dst, 0);
    if (sign_extend)
        fprintf(out, "\tmovsx %s, %s [%s]\n", sd, size_prefix, sa);
    else
        fprintf(out, "\tmovzx %s, %s [%s]\n", sd, size_prefix, sa);
    wd(out, fn, i->dst, sd);
}


/* Callee-saved registers pushed by the prologue, low index pushed first. */
static const char *saved_regs[] = { "rbx", "r12", "r13", "r14", "r15" };
#define NSAVED ((int)(sizeof(saved_regs) / sizeof(saved_regs[0])))

static void
emit_prologue(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int k;

    fprintf(out, "\tpush " BP "\n");
    fprintf(out, "\tmov " BP ", " SP "\n");
    if (frame > 0)
        fprintf(out, "\tsub " SP ", %d\n", frame);
    for (k = 0; k < NSAVED; k++)
        fprintf(out, "\tpush %s\n", saved_regs[k]);
}

static void
emit_epilogue_no_ret(FILE *out, struct ir_func *fn)
{
    int k;
    (void)fn;
    for (k = NSAVED - 1; k >= 0; k--)
        fprintf(out, "\tpop %s\n", saved_regs[k]);
    fprintf(out, "\tmov " SP ", " BP "\n");
    fprintf(out, "\tpop " BP "\n");
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
    int float_ret = (i->op == IR_FCALL || i->op == IR_FCALLI);

    int arg_bytes = 0;
    for (k = narg - 1; k >= 0; k--) {
        if (arg_is_float[k]) {
            const char *sa = frs(out, fn, arg_temps[k]);
            fprintf(out, "\tsub " SP ", 8\n");
            fprintf(out, "\tmovsd [" SP "], %s\n", sa);
            arg_bytes += 8;
        } else {
            /* int and i64 both push one 8-byte word (native) */
            const char *sa = rsq(out, fn, arg_temps[k], 0);
            fprintf(out, "\tpush %s\n", sa);
            arg_bytes += 8;
        }
    }
    if (indirect) {
        const char *sa = ars(out, fn, i->a, 0);
        fprintf(out, "\tcall %s\n", sa);
    } else {
        fprintf(out, "\tcall %s\n", i->sym);
    }
    if (arg_bytes > 0)
        fprintf(out, "\tadd " SP ", %d\n", arg_bytes);
    narg = 0;

    if (i->dst >= 0) {
        if (float_ret) {
            const char *sd = frd(fn, i->dst);
            if (strcmp(sd, "xmm0") != 0)
                fprintf(out, "\tmovsd %s, xmm0\n", sd);
            fwd(out, fn, i->dst, sd);
        } else if (i->op == IR_CALL64 || i->op == IR_CALLI64) {
            const char *dq = rdq(fn, i->dst, 0);
            if (strcmp(dq, "rax") != 0)
                fprintf(out, "\tmov %s, rax\n", dq);
            wdq(out, fn, i->dst, dq);
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
    int off = 2 * WORD;

    for (k = 0; k < narg; k++) {
        if (arg_is_float[k]) {
            const char *sa = frs(out, fn, arg_temps[k]);
            fprintf(out, "\tmovsd [" BP "+%d], %s\n", off, sa);
            off += 8;
        } else {
            const char *sa = rsq(out, fn, arg_temps[k], 0);
            fprintf(out, "\tmov [" BP "+%d], %s\n", off, sa);
            off += 8;
        }
    }
    if (indirect) {
        const char *sa = ars(out, fn, i->a, 0);
        fprintf(out, "\tmov " CXP ", %s\n", sa);
    }
    narg = 0;
    emit_epilogue_no_ret(out, fn);
    if (indirect)
        fprintf(out, "\tjmp " CXP "\n");
    else
        fprintf(out, "\tjmp %s\n", i->sym);
}

static void
emit_insn(FILE *out, struct ir_func *fn, struct ir_insn *i)
{
    switch (i->op) {
    case IR_NOP:
        break;

    case IR_ASM:
        /* basic inline asm: the string is emitted verbatim */
        fprintf(out, "%s\n", i->sym);
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
        fprintf(out, "\tlea %s, [" BP "%+d]\n", sd, off);
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
        const char *sa = ars(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\tmov %s, [%s]\n", sd, sa);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_SB: {
        const char *sb = rs(out, fn, i->b, 1);
        const char *sa = ars(out, fn, i->a, 0);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        fprintf(out, "\tmov [%s], cl\n", sa);
        break;
    }

    case IR_SH: {
        const char *sb = rs(out, fn, i->b, 1);
        const char *sa = ars(out, fn, i->a, 0);
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        fprintf(out, "\tmov [%s], cx\n", sa);
        break;
    }

    case IR_SW: {
        const char *sb = rs(out, fn, i->b, 1);
        const char *sa = ars(out, fn, i->a, 0);
        fprintf(out, "\tmov [%s], %s\n", sa, sb);
        break;
    }

    case IR_LDL: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmov %s, [" BP "%+d]\n", sd, off);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_STL: {
        const char *sa = rs(out, fn, i->a, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmov [" BP "%+d], %s\n", off, sa);
        break;
    }

    case IR_ALLOCA: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tadd eax, 15\n");     /* keep rsp 16-aligned */
        fprintf(out, "\tand eax, -16\n");
        fprintf(out, "\tsub rsp, rax\n");
        fprintf(out, "\tmov %s, esp\n", sd);  /* esp is the low-32 stack view */
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
        if (narg >= X86_MAX_ARGS)
            die("x86_emit: too many args");
        arg_is_float[narg] = 0;
        arg_is_i64[narg] = 0;
        arg_mem[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_FARG:
        if (narg >= X86_MAX_ARGS)
            die("x86_emit: too many args");
        arg_is_float[narg] = 1;
        arg_is_i64[narg] = 0;
        arg_mem[narg] = 0;
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

    case IR_FADD: emit_fbinop(out, fn, i, "add"); break;
    case IR_FSUB: emit_fbinop(out, fn, i, "sub"); break;
    case IR_FMUL: emit_fbinop(out, fn, i, "mul"); break;
    case IR_FDIV: emit_fbinop(out, fn, i, "div"); break;

    case IR_FNEG: {
        int f32 = i->imm == FWIDTH_F32;
        const char *sa = frs(out, fn, i->a);
        const char *sd = frd(fn, i->dst);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmovsd %s, %s\n", sd, sa);
        fprintf(out, "\t%s %s, [__x86_signmask%s]\n",
            f32 ? "xorps" : "xorpd", sd, f32 ? "_ss" : "");
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FABS: {
        int f32 = i->imm == FWIDTH_F32;
        const char *sa = frs(out, fn, i->a);
        const char *sd = frd(fn, i->dst);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmovsd %s, %s\n", sd, sa);
        fprintf(out, "\t%s %s, [__x86_absmask%s]\n",
            f32 ? "andps" : "andpd", sd, f32 ? "_ss" : "");
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FCMPEQ:
    case IR_FCMPLT:
    case IR_FCMPLE: {
        const char *sa, *sb, *sd;
        const char *ucomi = i->imm == FWIDTH_F32 ? "ucomiss" : "ucomisd";
        int id = fcmp_serial++;

        sa = frs(out, fn, i->a);
        sb = frs(out, fn, i->b);
        sd = rd(fn, i->dst, 0);
        if (i->op == IR_FCMPEQ) {
            fprintf(out, "\t%s %s, %s\n", ucomi, sa, sb);
            fprintf(out, "\tmov %s, 0\n", sd);
            fprintf(out, "\tjne .Lfc%d\n", id);
            fprintf(out, "\tjp .Lfc%d\n", id);
            fprintf(out, "\tmov %s, 1\n", sd);
            fprintf(out, ".Lfc%d:\n", id);
        } else if (i->op == IR_FCMPLT) {
            fprintf(out, "\t%s %s, %s\n", ucomi, sb, sa);
            fprintf(out, "\tmov %s, 0\n", sd);
            fprintf(out, "\tjbe .Lfc%d\n", id);
            fprintf(out, "\tmov %s, 1\n", sd);
            fprintf(out, ".Lfc%d:\n", id);
        } else {
            fprintf(out, "\t%s %s, %s\n", ucomi, sb, sa);
            fprintf(out, "\tmov %s, 0\n", sd);
            fprintf(out, "\tjb .Lfc%d\n", id);
            fprintf(out, "\tmov %s, 1\n", sd);
            fprintf(out, ".Lfc%d:\n", id);
        }
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ITOF: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst);
        fprintf(out, "\t%s %s, %s\n",
            i->imm == FWIDTH_F32 ? "cvtsi2ss" : "cvtsi2sd", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FTOI: {
        const char *sa = frs(out, fn, i->a);
        const char *sd = rd(fn, i->dst, 0);
        fprintf(out, "\t%s %s, %s\n",
            i->imm == FWIDTH_F32 ? "cvttss2si" : "cvttsd2si", sd, sa);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_F32TOF64: {
        const char *sa = frs(out, fn, i->a);
        const char *sd = frd(fn, i->dst);
        fprintf(out, "\tcvtss2sd %s, %s\n", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_F64TOF32: {
        const char *sa = frs(out, fn, i->a);
        const char *sd = frd(fn, i->dst);
        fprintf(out, "\tcvtsd2ss %s, %s\n", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FLS: {
        const char *sa = ars(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst);
        fprintf(out, "\tcvtss2sd %s, dword [%s]\n", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FLD: {
        const char *sa = ars(out, fn, i->a, 0);
        const char *sd = frd(fn, i->dst);
        fprintf(out, "\t%s %s, [%s]\n",
            i->imm == FWIDTH_F32 ? "movss" : "movsd", sd, sa);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSS: {
        const char *sb = frs(out, fn, i->b);
        const char *sa = ars(out, fn, i->a, 0);
        fprintf(out, "\tcvtsd2ss xmm0, %s\n", sb);
        fprintf(out, "\tmovss [%s], xmm0\n", sa);
        break;
    }

    case IR_FSD: {
        const char *sb = frs(out, fn, i->b);
        const char *sa = ars(out, fn, i->a, 0);
        fprintf(out, "\t%s [%s], %s\n",
            i->imm == FWIDTH_F32 ? "movss" : "movsd", sa, sb);
        break;
    }

    /* _Float16: the integer-only softfloat helpers (half.c) never touch xmm
     * and preserve the callee-saved allocatable registers, so the mid-selection
     * call is safe.  On x86-32 they use the stack ABI; on x86-64 the SysV
     * register ABI (arg in edi, result in eax). */
    case IR_FLH: {
        const char *sd = frd(fn, i->dst);
        const char *sa = ars(out, fn, i->a, 0);
        fprintf(out, "\tmovzx edi, word [%s]\n", sa);
        fprintf(out, "\tcall __skj_extendhfsf\n");     /* eax = single bits */
        fprintf(out, "\tmovd xmm0, eax\n");
        fprintf(out, "\tcvtss2sd %s, xmm0\n", sd);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSH: {
        const char *sb = frs(out, fn, i->b);
        fprintf(out, "\tcvtsd2ss xmm0, %s\n", sb);
        fprintf(out, "\tmovd edi, xmm0\n");             /* single bits -> arg */
        fprintf(out, "\tcall __skj_truncsfhf\n");        /* eax = half bits */
        fprintf(out, "\tmov ecx, eax\n");                /* save half (call-safe reg) */
        {
            const char *sa = ars(out, fn, i->a, 0);      /* reload addr after call */
            fprintf(out, "\tmov word [%s], cx\n", sa);
        }
        break;
    }

    case IR_FLDL: {
        const char *sd = frd(fn, i->dst);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\t%s %s, [" BP "%+d]\n",
            i->imm == FWIDTH_F32 ? "movss" : "movsd", sd, off);
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSTL: {
        const char *sa = frs(out, fn, i->a);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\t%s [" BP "%+d], %s\n",
            i->imm == FWIDTH_F32 ? "movss" : "movsd", off, sa);
        break;
    }

    /* ---- Delimited continuations ----
     * The mark metadata (fp, sp, re-entry PC) is stored as 32-bit words: on
     * x86-64 (ILP32) these are low-memory addresses, and ebp/esp are the
     * 32-bit views of the frame/stack pointers in both modes. */
    case IR_MARK: {
        const char *sd = rd(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);

        fprintf(out, "\tmov [" BP "%+d], ebp\n", off);
        fprintf(out, "\tmov [" BP "%+d], esp\n", off + 4);
        fprintf(out, "\tmov dword [" BP "%+d], .Lmark%d_%d\n",
            off + 8, label_prefix, i->label);
        fprintf(out, "\tlea eax, [" BP "%+d]\n", off);
        fprintf(out, "\tmov [__cont_mark_sp], eax\n");
        /* stash the continuation-arena high-water mark (first entry only,
         * before the re-entry label) for IR_CONT_UNWIND to restore */
        fprintf(out, "\tmov eax, [__cont_arena_ptr]\n");
        fprintf(out, "\tmov [" BP "%+d], eax\n", off + 12);
        fprintf(out, "\txor eax, eax\n");
        fprintf(out, ".Lmark%d_%d:\n", label_prefix, i->label);
        if (strcmp(sd, "eax") != 0)
            fprintf(out, "\tmov %s, eax\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CAPTURE: {
        const char *sd = rd(fn, i->dst, 0);
        int k;

        /* push the callee-saved allocatables into the captured segment */
        for (k = 0; k < NSAVED; k++)
            fprintf(out, "\tpush %s\n", saved_regs[k]);
        fprintf(out, "\tcall __cont_capture\n");
        for (k = NSAVED - 1; k >= 0; k--)
            fprintf(out, "\tpop %s\n", saved_regs[k]);
        if (strcmp(sd, "eax") != 0)
            fprintf(out, "\tmov %s, eax\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_CONT_UNWIND: {
        /* restore the continuation arena to the mark-time high-water mark,
         * reclaiming every buffer captured within the closing reset extent */
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmov eax, [" BP "%+d]\n", off + 12);
        fprintf(out, "\tmov [__cont_arena_ptr], eax\n");
        break;
    }

    case IR_RESUME: {
        const char *sa = rsq(out, fn, i->a, 0);
        const char *sb = rsq(out, fn, i->b, 1);
        fprintf(out, "\tpush %s\n", sb);
        fprintf(out, "\tpush %s\n", sa);
        fprintf(out, "\tcall __cont_resume\n");
        break;
    }

    case IR_FUNC:
    case IR_ENDF:
        break;

    /* ---- I64 opcodes ---- */

    /* native single-register 64-bit */
    case IR_LIC64: {
        const char *sd = rdq(fn, i->dst, 0);
        fprintf(out, "\tmov %s, %lld\n", sd, (long long)i->imm);
        wdq(out, fn, i->dst, sd);
        break;
    }

    case IR_ADD64: emit_qbinop(out, fn, i, "add"); break;
    case IR_SUB64: emit_qbinop(out, fn, i, "sub"); break;
    case IR_MUL64: emit_qbinop(out, fn, i, "imul"); break;
    case IR_AND64: emit_qbinop(out, fn, i, "and"); break;
    case IR_OR64:  emit_qbinop(out, fn, i, "or");  break;
    case IR_XOR64: emit_qbinop(out, fn, i, "xor"); break;

    case IR_SHL64: case IR_SHRS64: case IR_SHRU64: {
        const char *sb = rs(out, fn, i->b, 1);       /* shift count (int) */
        const char *sa = rsq(out, fn, i->a, 0);
        const char *sd = rdq(fn, i->dst, 0);
        const char *op = i->op == IR_SHL64 ? "shl"
                       : i->op == IR_SHRS64 ? "sar" : "shr";
        if (strcmp(sb, "ecx") != 0)
            fprintf(out, "\tmov ecx, %s\n", sb);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        fprintf(out, "\t%s %s, cl\n", op, sd);
        wdq(out, fn, i->dst, sd);
        break;
    }

    case IR_NEG64: {
        const char *sa = rsq(out, fn, i->a, 0);
        const char *sd = rdq(fn, i->dst, 0);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        fprintf(out, "\tneg %s\n", sd);
        wdq(out, fn, i->dst, sd);
        break;
    }

    case IR_CMP64EQ: case IR_CMP64NE:
    case IR_CMP64LTS: case IR_CMP64LES: case IR_CMP64GTS: case IR_CMP64GES:
    case IR_CMP64LTU: case IR_CMP64LEU: case IR_CMP64GTU: case IR_CMP64GEU: {
        const char *sa = rsq(out, fn, i->a, 0);
        const char *sb = rsq(out, fn, i->b, 1);
        const char *sd = rd(fn, i->dst, 0);          /* int (bool) result */
        const char *cc;
        switch (i->op) {
        case IR_CMP64EQ:  cc = "sete";  break;
        case IR_CMP64NE:  cc = "setne"; break;
        case IR_CMP64LTS: cc = "setl";  break;
        case IR_CMP64LES: cc = "setle"; break;
        case IR_CMP64GTS: cc = "setg";  break;
        case IR_CMP64GES: cc = "setge"; break;
        case IR_CMP64LTU: cc = "setb";  break;
        case IR_CMP64LEU: cc = "setbe"; break;
        case IR_CMP64GTU: cc = "seta";  break;
        default:          cc = "setae"; break;
        }
        fprintf(out, "\tcmp %s, %s\n", sa, sb);
        fprintf(out, "\t%s al\n", cc);
        fprintf(out, "\tmovzx %s, al\n", sd);
        fprintf(out, "\tneg %s\n", sd);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_LD64: {
        const char *sa = ars(out, fn, i->a, 0);
        const char *sd = rdq(fn, i->dst, 1);
        fprintf(out, "\tmov %s, [%s]\n", sd, sa);
        wdq(out, fn, i->dst, sd);
        break;
    }

    case IR_ST64: {
        const char *sb = rsq(out, fn, i->b, 1);
        const char *sa = ars(out, fn, i->a, 0);
        fprintf(out, "\tmov [%s], %s\n", sa, sb);
        break;
    }

    case IR_LDL64: {
        const char *sd = rdq(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmov %s, [" BP "%+d]\n", sd, off);
        wdq(out, fn, i->dst, sd);
        break;
    }

    case IR_STL64: {
        const char *sa = rsq(out, fn, i->a, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tmov [" BP "%+d], %s\n", off, sa);
        break;
    }

    case IR_LEA64: {                            /* 64-bit symbol address */
        const char *sd = rdq(fn, i->dst, 0);
        fprintf(out, "\tmov %s, %s\n", sd, i->sym);
        wdq(out, fn, i->dst, sd);
        break;
    }

    case IR_ADL64: {                            /* 64-bit local-slot address */
        const char *sd = rdq(fn, i->dst, 0);
        int off = slot_offset(fn, i->slot);
        fprintf(out, "\tlea %s, [" BP "%+d]\n", sd, off);
        wdq(out, fn, i->dst, sd);
        break;
    }


    case IR_SEXT64: {
        const char *sa = rs(out, fn, i->a, 0);       /* 32-bit source */
        const char *sd = rdq(fn, i->dst, 0);
        fprintf(out, "\tmovsxd %s, %s\n", sd, sa);
        wdq(out, fn, i->dst, sd);
        break;
    }

    case IR_ZEXT64: {
        const char *sa = rs(out, fn, i->a, 0);
        const char *sd = rd(fn, i->dst, 0);          /* 32-bit view zeroes high */
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        wdq(out, fn, i->dst, rdq(fn, i->dst, 0));
        break;
    }

    case IR_TRUNC64: {
        const char *sa = rs(out, fn, i->a, 0);       /* low-32 view of the i64 */
        const char *sd = rd(fn, i->dst, 0);
        if (strcmp(sa, sd) != 0)
            fprintf(out, "\tmov %s, %s\n", sd, sa);
        wd(out, fn, i->dst, sd);
        break;
    }

    case IR_ARG64:
        if (narg >= X86_MAX_ARGS)
            die("x86_emit: too many args");
        arg_is_float[narg] = 0;
        arg_is_i64[narg] = 1;
        arg_mem[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_CALL64:  emit_call_flush(out, fn, i, 0); break;
    case IR_CALLI64: emit_call_flush(out, fn, i, 1); break;

    case IR_RETV64: {
        const char *sa = rsq(out, fn, i->a, 0);
        if (strcmp(sa, "rax") != 0)
            fprintf(out, "\tmov rax, %s\n", sa);
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

/* emit an aggregate global's byte image: pad to each item's offset, emit the
   sized value (8-byte low dword first, little-endian), pad to the full size */
static void
emit_init_image(FILE *out, struct ir_global *g)
{
    struct ir_init *it;
    int cur = 0;

    for (it = g->inits; it; it = it->next) {
        if (it->offset > cur) {
            fprintf(out, "times %d db 0\n", it->offset - cur);
            cur = it->offset;
        }
        if (it->sym)
            fprintf(out, "\t%s %s\n", it->size == 8 ? "dq" : "dd", it->sym);
        else if (it->size == 8) {
            uint64_t b = (uint64_t)it->ival;
            fprintf(out, "\tdd 0x%08x\n", (unsigned)(b & 0xFFFFFFFF));
            fprintf(out, "\tdd 0x%08x\n", (unsigned)(b >> 32));
        } else if (it->size == 2)
            fprintf(out, "\tdw 0x%04x\n", (unsigned)(it->ival & 0xFFFF));
        else if (it->size == 1)
            fprintf(out, "\tdb 0x%02x\n", (unsigned)(it->ival & 0xFF));
        else
            fprintf(out, "\tdd 0x%08x\n", (unsigned)it->ival);
        cur += it->size;
    }
    if (g->arr_size > cur)
        fprintf(out, "times %d db 0\n", g->arr_size - cur);
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

        {
            /* natural alignment, raised by an _Alignas request (g->align) */
            int alb = (g->base_type == IR_F64 || g->base_type == IR_I64)
                      ? 8 : 4;
            if (g->align > alb)
                alb = g->align;
            fprintf(out, "align %d\n", alb);
        }
        if (!g->is_local)
            fprintf(out, "global %s\n", g->name);
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
                        elsz == 8 ? "dq" : "dd", g->init_syms[k]);
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
    fputs("__x86_signmask_ss:\n", out);
    fputs("\tdd 0x80000000, 0x80000000, 0x80000000, 0x80000000\n", out);
    fputs("__x86_absmask_ss:\n", out);
    fputs("\tdd 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF\n", out);
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
                 i->op == IR_FCALL || i->op == IR_CALL64 ||
                 i->op == IR_CALL_AGG ||
                 i->op == IR_LEA || i->op == IR_LEA64)) {
                /* A symbol-address load (IR_LEA/LEA64) can reference an
                 * external data symbol, e.g. Excelsior's __exc_self, which
                 * NASM needs declared extern.  is_defined filters out the
                 * program's own globals and functions. */
                if (!is_defined(prog, i->sym))
                    emit_extern_once(out, seen, &nseen, i->sym);
            }
            switch (i->op) {
            case IR_FLH:
                emit_extern_once(out, seen, &nseen, "__skj_extendhfsf");
                break;
            case IR_FSH:
                emit_extern_once(out, seen, &nseen, "__skj_truncsfhf");
                break;
            case IR_CAPTURE:
                emit_extern_once(out, seen, &nseen, "__cont_capture");
                break;
            case IR_RESUME:
                emit_extern_once(out, seen, &nseen, "__cont_resume");
                break;
            case IR_MARK:
                emit_extern_once(out, seen, &nseen, "__cont_mark_sp");
                emit_extern_once(out, seen, &nseen, "__cont_arena_ptr");
                break;
            case IR_CONT_UNWIND:
                emit_extern_once(out, seen, &nseen, "__cont_arena_ptr");
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
    /* mark the stack non-executable so the GNU linker does not warn (and
       default to an executable stack) about the missing note */
    fputs("\nsection .note.GNU-stack noalloc noexec nowrite progbits\n", out);
}
