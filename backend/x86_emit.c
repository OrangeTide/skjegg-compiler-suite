/* x86_emit.c : x86 back-end, emits NASM-syntax assembly */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
/*
 * One source, two targets, chosen at compile time by X86_BITS (32 or 64),
 * the way gcc's -m32/-m64 pick an ABI.  The 32-bit integer core, all SSE
 * float, compares, branches and the continuation machinery are shared
 * verbatim; only the pointer/stack width, the i64 representation, the
 * register file, the prologue/epilogue and the runtime differ.
 *
 *   X86_BITS=32: i686 (IA-32), Linux ELF32.  i64 is synthesised from
 *                register pairs plus the __muldi3 family of helpers.
 *   X86_BITS=64: x86-64, Linux ELF64, ILP32-style.  Every address is a
 *                32-bit value in a register's 32-bit view (a 32-bit write
 *                zeroes the upper half), and the 64-bit view of the same
 *                register is the zero-extended (< 4GB) address fed to a
 *                load/store.  A `long long` (IR_I64) is a single native
 *                64-bit register, not a pair, so no __muldi3 helpers.
 *
 * Stack-based calling convention (both modes):
 *   - Args pushed right-to-left, one word each (WORD bytes); caller pops.
 *   - Return value in eax/rax (integer) or xmm0 (double).
 *   - Callee-save allocatable: 32: ebx, esi, edi;  64: rbx, r12-r15.
 *   - Scratch: eax/ecx/edx (+ their 64-bit views), xmm0.
 *
 * Frame layout (after prologue), with BP the frame pointer:
 *
 *      BP + 2*WORD + WORD*i  param i
 *      BP + WORD             return address
 *      BP + 0               saved old BP
 *      BP - locals          bottom of locals / spill slots
 */

#include "ir.h"

#include <stdio.h>
#include <string.h>

#ifndef X86_BITS
#define X86_BITS 32
#endif

#if X86_BITS == 64

#define WORD 8
#define BP "rbp"
#define SP "rsp"
#define CXP "rcx"

/* 32-bit views (integer core / values) and 64-bit views (addresses / i64) */
static const char *iregs[] = { "ebx", "r12d", "r13d", "r14d", "r15d" };
static const char *qregs[] = { "rbx", "r12",  "r13",  "r14",  "r15"  };
static const char *scratch[]  = { "eax", "ecx", "edx" };
static const char *qscratch[] = { "rax", "rcx", "rdx" };

#else

#define WORD 4
#define BP "ebp"
#define SP "esp"
#define CXP "ecx"

static const char *iregs[] = { "ebx", "esi", "edi" };
#define qregs iregs
static const char *scratch[] = { "eax", "ecx", "edx" };
#define qscratch scratch

#endif

static const char *fpregs[] = {
    "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
};

#if defined(CC_PSABI) && X86_BITS == 64
/****************************************************************
 * System V AMD64 calling convention (cc, P1: fixed scalar/ptr/float args)
 *
 * Integer/pointer args -> rdi,rsi,rdx,rcx,r8,r9; float -> xmm0..xmm7; the rest
 * on a 16-byte-aligned stack.  Return in rax / xmm0.  A register param gets an
 * 8-byte "home" slot just below rbp that the prologue spills it into, so the
 * slot-based lowering downstream is unchanged; an overflow (stack) param is
 * read from the incoming stack at rbp+16+8*k.
 ****************************************************************/

/* integer/pointer arg registers (64-bit view; an int arg rides the low 32,
   which the callee's slot read picks up) */
static const char *sysv_iarg[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

/* where a parameter is passed.  A scalar is one eightbyte; a small struct is
   1-2 eightbytes (each INTEGER -> gp or SSE -> xmm).  is_reg is set only when
   every eightbyte fits in registers (SysV's all-or-nothing rule); otherwise
   the whole parameter is on the incoming stack. */
struct sysv_ploc {
    int is_reg;         /* all eightbytes in registers */
    int neb;            /* eightbyte count (1-2) */
    int hidx;           /* first home-slot index (reg params) */
    int soff;           /* first stack-slot index (stack params) */
    int cls[2];         /* class per eightbyte: 0 INTEGER, 1 SSE */
    int ridx[2];        /* register index within its class per eightbyte */
};

static void
sysv_param(struct ir_func *fn, int slot, struct sysv_ploc *out)
{
    int int_used = 0, sse_used = 0, nstack = 0, nreg = 0, i, j;

    for (i = 0; ; i++) {
        struct sysv_ploc p;
        int neb = fn->param_neb ? fn->param_neb[i] : 1;
        int need_i = 0, need_s = 0;
        int mem = fn->param_cls && fn->param_cls[4 * i] == -2;
        if (neb < 1) neb = 1;
        p.neb = neb;
        if (mem) {
            /* MEMORY struct: always on the incoming stack, neb 8-byte slots */
            p.is_reg = 0;
            p.hidx = 0;
            p.soff = nstack;
            p.cls[0] = p.cls[1] = -2;
            nstack += neb;
            if (i == slot) {
                *out = p;
                return;
            }
            continue;
        }
        for (j = 0; j < neb; j++) {
            p.cls[j] = fn->param_cls ? fn->param_cls[4 * i + j] : 0;
            if (p.cls[j] == 1) need_s++; else need_i++;
        }
        if (int_used + need_i <= 6 && sse_used + need_s <= 8) {
            p.is_reg = 1;
            p.hidx = nreg;
            p.soff = 0;
            for (j = 0; j < neb; j++)
                p.ridx[j] = (p.cls[j] == 1) ? sse_used++ : int_used++;
            nreg += neb;
        } else {
            p.is_reg = 0;
            p.hidx = 0;
            p.soff = nstack;
            nstack += neb;
        }
        if (i == slot) {
            *out = p;
            return;
        }
    }
}

/* number of register home slots (8 bytes each) below rbp */
static int
sysv_nreg_params(struct ir_func *fn)
{
    struct sysv_ploc p;
    int i, n = 0;
    for (i = 0; i < fn->nparams; i++) {
        sysv_param(fn, i, &p);
        if (p.is_reg)
            n += p.neb;
    }
    return n;
}

#define SYSV_HOME(fn) (sysv_nreg_params(fn) * 8)
#endif

/* bytes reserved just below rbp for register-param home slots (0 unless the
   SysV convention is in effect) */
static int
param_home(struct ir_func *fn)
{
#if defined(CC_PSABI) && X86_BITS == 64
    return SYSV_HOME(fn);
#else
    (void)fn;
    return 0;
#endif
}

/* SysV register save area (48 gp + 128 fp = 176 bytes) for a variadic
   function, placed just below the param homes; va_start points at its base */
static int
va_save(struct ir_func *fn)
{
#if defined(CC_PSABI) && X86_BITS == 64
    return fn->is_variadic ? 176 : 0;
#else
    (void)fn;
    return 0;
#endif
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
#if defined(CC_PSABI) && X86_BITS == 64
        /* SysV: a register param lives in its home slot(s) below rbp, an
           overflow param on the incoming stack at rbp+16+8*k.  A struct's
           base is the lowest of its homes so eightbyte j sits at base+8*j. */
        struct sysv_ploc p;
        sysv_param(fn, slot, &p);
        if (p.is_reg)
            return -8 * (p.hidx + p.neb);
        return 2 * WORD + 8 * p.soff;
#else
        off = 2 * WORD;
        for (i = 0; i < slot; i++) {
            int sz = (fn->slot_size[i] + (WORD - 1)) & ~(WORD - 1);
            off += sz;
        }
        return off;
#endif
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

#if X86_BITS == 32
static int
i64spill_byte_offset(struct ir_func *fn, int temp)
{
    return -locals_size(fn) - fn->nspills * WORD
           - fn->nfspills * 8 - (fn->temp_spill[temp] + 8);
}
#endif

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

#if X86_BITS == 64
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
#endif /* X86_BITS == 64 */

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
#if X86_BITS == 32

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
        fprintf(out, "\tmov eax, [" BP "%+d]\n", off);
        fprintf(out, "\tmov edx, [" BP "%+d]\n", off - 4);
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
    fprintf(out, "\tmov [" BP "%+d], %s\n", off, hi);
    fprintf(out, "\tmov [" BP "%+d], %s\n", off - 4, lo);
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
        sprintf(hi_buf, "[" BP "%+d]", off);
        sprintf(lo_buf, "[" BP "%+d]", off - 4);
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
            fprintf(out, "\tmov esi, [" BP "%+d]\n", aoff);
            fprintf(out, "\tmov edi, [" BP "%+d]\n", aoff - 4);
        }
        fprintf(out, "\t%s edi, %s\n", op, bl);
        fprintf(out, "\t%s esi, %s\n", op_carry ? op_carry : op, bh);
    } else {
        if (ap >= 0) {
            fprintf(out, "\tmov eax, esi\n");
            fprintf(out, "\tmov edx, edi\n");
        } else {
            int aoff = i64spill_byte_offset(fn, a_t);
            fprintf(out, "\tmov eax, [" BP "%+d]\n", aoff);
            fprintf(out, "\tmov edx, [" BP "%+d]\n", aoff - 4);
        }
        fprintf(out, "\t%s edx, %s\n", op, bl);
        fprintf(out, "\t%s eax, %s\n", op_carry ? op_carry : op, bh);
        {
            int doff = i64spill_byte_offset(fn, dst_t);
            fprintf(out, "\tmov [" BP "%+d], eax\n", doff);
            fprintf(out, "\tmov [" BP "%+d], edx\n", doff - 4);
        }
    }
}
#endif /* X86_BITS == 32 */

/****************************************************************
 * Binary / unary / compare helpers
 ****************************************************************/

#if X86_BITS == 64
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
#endif

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

static int arg_temps[16];
static int arg_is_float[16];
static int arg_is_i64[16];
static int arg_mem[16];         /* >0: a by-value MEMORY struct arg of this size */
static int narg;
static int label_prefix;
static int fcmp_serial;
#if X86_BITS == 32
static int i64cmp_serial;
#endif

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
#if X86_BITS == 64
static const char *saved_regs[] = { "rbx", "r12", "r13", "r14", "r15" };
#else
static const char *saved_regs[] = { "ebx", "esi", "edi" };
#endif
#define NSAVED ((int)(sizeof(saved_regs) / sizeof(saved_regs[0])))

static void
emit_prologue(FILE *out, struct ir_func *fn)
{
    int frame = frame_size(fn);
    int k;

#if defined(CC_PSABI) && X86_BITS == 64
    /* pad so rsp is 16-aligned after the callee-saved pushes: at entry rsp ≡ 8
       (mod 16), and the prologue subtracts 8 (push rbp) + frame + NSAVED*8, so
       (frame + NSAVED*8) must be a multiple of 16 (self-adjusts to NSAVED) */
    {
        int over = (frame + NSAVED * 8) % 16;
        if (over)
            frame += 16 - over;
    }
#endif
    fprintf(out, "\tpush " BP "\n");
    fprintf(out, "\tmov " BP ", " SP "\n");
    if (frame > 0)
        fprintf(out, "\tsub " SP ", %d\n", frame);
    for (k = 0; k < NSAVED; k++)
        fprintf(out, "\tpush %s\n", saved_regs[k]);
#if defined(CC_PSABI) && X86_BITS == 64
    /* spill register params into their home slots below rbp.  For a struct
       parameter (1-2 eightbytes) each eightbyte j is stored at base+8*j,
       where base is the lowest home, so its fields read back contiguously. */
    for (k = 0; k < fn->nparams; k++) {
        struct sysv_ploc p;
        int j;
        sysv_param(fn, k, &p);
        if (!p.is_reg)
            continue;
        for (j = 0; j < p.neb; j++) {
            int off = -8 * (p.hidx + p.neb) + 8 * j;
            if (p.cls[j] == 1)
                fprintf(out, "\tmovsd [rbp%+d], xmm%d\n", off, p.ridx[j]);
            else
                fprintf(out, "\tmov [rbp%+d], %s\n", off, sysv_iarg[p.ridx[j]]);
        }
    }
    /* variadic: save the arg registers to the register save area (base at
       rbp - frame_reserve) so va_arg can walk them.  Store the low 8 bytes of
       each xmm (a double), which is all cc's va_arg reads. */
    if (fn->is_variadic) {
        int base = -frame_reserve(fn);
        for (k = 0; k < 6; k++)
            fprintf(out, "\tmov [rbp%+d], %s\n", base + 8 * k, sysv_iarg[k]);
        for (k = 0; k < 8; k++)
            fprintf(out, "\tmovsd [rbp%+d], xmm%d\n", base + 48 + 16 * k, k);
    }
#endif
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

#if defined(CC_PSABI) && X86_BITS == 64
    {
        /* System V AMD64: classify each arg into an integer register, an SSE
           register, or the overflow stack. */
        int ireg[16], sreg[16], on_stack[16], ssize[16];
        int int_used = 0, sse_used = 0, stack_bytes = 0;
        for (k = 0; k < narg; k++) {
            ireg[k] = sreg[k] = -1;
            on_stack[k] = 0;
            ssize[k] = 0;
            if (arg_mem[k] > 0) {           /* MEMORY struct: always on stack */
                on_stack[k] = 1;
                ssize[k] = (arg_mem[k] + 7) & ~7;
            } else if (arg_is_float[k]) {
                if (sse_used < 8) sreg[k] = sse_used++;
                else { on_stack[k] = 1; ssize[k] = 8; }
            } else {
                if (int_used < 6) ireg[k] = int_used++;
                else { on_stack[k] = 1; ssize[k] = 8; }
            }
            stack_bytes += ssize[k];
        }
        /* overflow args, right-to-left; pad so rsp stays 16-aligned */
        int pad = (stack_bytes % 16) ? 8 : 0;
        if (pad)
            fprintf(out, "\tsub " SP ", 8\n");
        for (k = narg - 1; k >= 0; k--) {
            if (!on_stack[k]) continue;
            if (arg_mem[k] > 0) {
                /* copy the struct's bytes into a stack block */
                int t = arg_temps[k], r = fn->temp_reg[t];
                int off = 0, rem = arg_mem[k];
                fprintf(out, "\tsub " SP ", %d\n", ssize[k]);
                if (r >= 0)
                    fprintf(out, "\tmov r11, %s\n", qregs[r]);
                else
                    fprintf(out, "\tmov r11, [" BP "%+d]\n",
                        spill_byte_offset(fn, t));
                while (rem >= 8) {
                    fprintf(out, "\tmov r10, [r11+%d]\n\tmov [" SP "+%d], r10\n",
                        off, off);
                    off += 8; rem -= 8;
                }
                if (rem >= 4) {
                    fprintf(out, "\tmov r10d, [r11+%d]\n\tmov [" SP "+%d], r10d\n",
                        off, off);
                    off += 4; rem -= 4;
                }
                if (rem >= 2) {
                    fprintf(out, "\tmovzx r10d, word [r11+%d]\n"
                        "\tmov [" SP "+%d], r10w\n", off, off);
                    off += 2; rem -= 2;
                }
                if (rem >= 1)
                    fprintf(out, "\tmovzx r10d, byte [r11+%d]\n"
                        "\tmov [" SP "+%d], r10b\n", off, off);
            } else if (arg_is_float[k]) {
                const char *sa = frs(out, fn, arg_temps[k]);
                fprintf(out, "\tsub " SP ", 8\n\tmovsd [" SP "], %s\n", sa);
            } else {
                const char *sa = rsq(out, fn, arg_temps[k], 0);
                fprintf(out, "\tpush %s\n", sa);
            }
        }
        /* float register args: stage through a scratch stack block so a move
           into xmmN never clobbers a not-yet-placed source (xmm1..7 overlap
           the arg registers) */
        if (sse_used > 0) {
            int blk = (sse_used * 8 + 15) & ~15;
            fprintf(out, "\tsub " SP ", %d\n", blk);
            for (k = 0; k < narg; k++)
                if (!on_stack[k] && arg_is_float[k]) {
                    const char *sa = frs(out, fn, arg_temps[k]);
                    fprintf(out, "\tmovsd [" SP "+%d], %s\n", sreg[k] * 8, sa);
                }
            for (k = 0; k < narg; k++)
                if (!on_stack[k] && arg_is_float[k])
                    fprintf(out, "\tmovsd xmm%d, [" SP "+%d]\n",
                        sreg[k], sreg[k] * 8);
            fprintf(out, "\tadd " SP ", %d\n", blk);
        }
        /* int register args: load straight into the target (rdi..r9 are
           disjoint from the allocatable rbx/r12-r15, so no clobber) */
        for (k = 0; k < narg; k++) {
            if (on_stack[k] || arg_is_float[k]) continue;
            int t = arg_temps[k], r = fn->temp_reg[t];
            const char *treg = sysv_iarg[ireg[k]];
            if (r >= 0)
                fprintf(out, "\tmov %s, %s\n", treg, qregs[r]);
            else
                fprintf(out, "\tmov %s, [" BP "%+d]\n",
                    treg, spill_byte_offset(fn, t));
        }
        /* materialize an indirect target into r11 (a scratch that is neither
           an arg register nor rax) before setting al, so reloading a spilled
           target does not clobber the SSE count */
        if (indirect) {
            int r = fn->temp_reg[i->a];
            if (r >= 0)
                fprintf(out, "\tmov r11, %s\n", qregs[r]);
            else
                fprintf(out, "\tmov r11, [" BP "%+d]\n",
                    spill_byte_offset(fn, i->a));
        }
        /* al = number of SSE registers used (needed if the callee is variadic;
           ignored otherwise) */
        fprintf(out, "\tmov eax, %d\n", sse_used);
        if (indirect)
            fprintf(out, "\tcall r11\n");
        else
            fprintf(out, "\tcall %s\n", i->sym);
        if (stack_bytes + pad > 0)
            fprintf(out, "\tadd " SP ", %d\n", stack_bytes + pad);
        narg = 0;
    }
#else
    int arg_bytes = 0;
    for (k = narg - 1; k >= 0; k--) {
        if (arg_is_float[k]) {
            const char *sa = frs(out, fn, arg_temps[k]);
            fprintf(out, "\tsub " SP ", 8\n");
            fprintf(out, "\tmovsd [" SP "], %s\n", sa);
            arg_bytes += 8;
#if X86_BITS == 64
        } else {
            /* int and i64 both push one 8-byte word (native) */
            const char *sa = rsq(out, fn, arg_temps[k], 0);
            fprintf(out, "\tpush %s\n", sa);
            arg_bytes += 8;
        }
#else
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
#endif
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
#endif

    if (i->dst >= 0) {
        if (float_ret) {
            const char *sd = frd(fn, i->dst);
            if (strcmp(sd, "xmm0") != 0)
                fprintf(out, "\tmovsd %s, xmm0\n", sd);
            fwd(out, fn, i->dst, sd);
        } else if (i->op == IR_CALL64 || i->op == IR_CALLI64) {
#if X86_BITS == 64
            const char *dq = rdq(fn, i->dst, 0);
            if (strcmp(dq, "rax") != 0)
                fprintf(out, "\tmov %s, rax\n", dq);
            wdq(out, fn, i->dst, dq);
#else
            const char *dhi, *dlo;
            i64_rd(fn, i->dst, &dhi, &dlo);
            if (strcmp(dhi, "eax") != 0)
                fprintf(out, "\tmov %s, eax\n", dhi);
            if (strcmp(dlo, "edx") != 0)
                fprintf(out, "\tmov %s, edx\n", dlo);
            i64_wd(out, fn, i->dst, dhi, dlo);
#endif
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
#if X86_BITS == 64
        } else {
            const char *sa = rsq(out, fn, arg_temps[k], 0);
            fprintf(out, "\tmov [" BP "+%d], %s\n", off, sa);
            off += 8;
        }
#else
        } else if (arg_is_i64[k]) {
            const char *hi, *lo;
            i64_rs(out, fn, arg_temps[k], &hi, &lo);
            fprintf(out, "\tmov [" BP "+%d], %s\n", off, hi);
            fprintf(out, "\tmov [" BP "+%d], %s\n", off + 4, lo);
            off += 8;
        } else {
            const char *sa = rs(out, fn, arg_temps[k], 0);
            fprintf(out, "\tmov [" BP "+%d], %s\n", off, sa);
            off += 4;
        }
#endif
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
#if X86_BITS == 64
        fprintf(out, "\tadd eax, 15\n");     /* keep rsp 16-aligned */
        fprintf(out, "\tand eax, -16\n");
        fprintf(out, "\tsub rsp, rax\n");
#else
        fprintf(out, "\tadd eax, 3\n");
        fprintf(out, "\tand eax, -4\n");
        fprintf(out, "\tsub esp, eax\n");
#endif
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
        if (narg >= 16)
            die("x86_emit: too many args");
        arg_is_float[narg] = 0;
        arg_is_i64[narg] = 0;
        arg_mem[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
    case IR_FARG:
        if (narg >= 16)
            die("x86_emit: too many args");
        arg_is_float[narg] = 1;
        arg_is_i64[narg] = 0;
        arg_mem[narg] = 0;
        arg_temps[narg++] = i->a;
        break;
#if defined(CC_PSABI) && X86_BITS == 64
    case IR_ARG_MEM:
        if (narg >= 16)
            die("x86_emit: too many args");
        arg_is_float[narg] = 0;
        arg_is_i64[narg] = 0;
        arg_mem[narg] = i->imm;         /* by-value struct byte size */
        arg_temps[narg++] = i->a;       /* the struct's address */
        break;
#endif
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
#if defined(CC_PSABI) && X86_BITS == 64
    case IR_CALL_AGG:
    case IR_CALLI_AGG: {
        /* make the call, then store the returned struct's eightbytes (in
           rax/rdx and xmm0/xmm1 per the imm-encoded classes) into the
           destination slot */
        int neb = i->imm & 7;
        int rc[2];
        int j, iidx = 0, sidx = 0;
        const char *iret[2] = { "rax", "rdx" };
        int base;
        /* 2-bit class per slot at bits 3+2*j (0 int, 1 SSE); x86-64 never
           produces the float-single class */
        rc[0] = (i->imm >> 3) & 3;
        rc[1] = (i->imm >> 5) & 3;
        emit_call_flush(out, fn, i, i->op == IR_CALLI_AGG);
        base = slot_offset(fn, i->slot);
        for (j = 0; j < neb; j++) {
            if (rc[j] == 1)
                fprintf(out, "\tmovsd [rbp%+d], xmm%d\n", base + 8 * j, sidx++);
            else
                fprintf(out, "\tmov [rbp%+d], %s\n", base + 8 * j, iret[iidx++]);
        }
        break;
    }
#endif
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
#if defined(CC_PSABI) && X86_BITS == 64
    case IR_RETV_AGG: {
        /* pack the struct at [i->a] into the return registers: each eightbyte
           j goes to rax/rdx (INTEGER) or xmm0/xmm1 (SSE) per fn->ret_cls */
        int r = fn->temp_reg[i->a];
        const char *iret[2] = { "rax", "rdx" };
        int j, iidx = 0, sidx = 0;
        const char *ab;
        if (r >= 0) {
            ab = qregs[r];
        } else {
            fprintf(out, "\tmov r11, [rbp%+d]\n", spill_byte_offset(fn, i->a));
            ab = "r11";
        }
        for (j = 0; j < fn->ret_neb; j++) {
            if (fn->ret_cls[j] == 1)
                fprintf(out, "\tmovsd xmm%d, [%s%+d]\n", sidx++, ab, 8 * j);
            else
                fprintf(out, "\tmov %s, [%s%+d]\n", iret[iidx++], ab, 8 * j);
        }
        emit_epilogue(out, fn);
        break;
    }
#endif

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
#if X86_BITS == 64
        const char *sa = ars(out, fn, i->a, 0);
        fprintf(out, "\tmovzx edi, word [%s]\n", sa);
        fprintf(out, "\tcall __skj_extendhfsf\n");     /* eax = single bits */
        fprintf(out, "\tmovd xmm0, eax\n");
        fprintf(out, "\tcvtss2sd %s, xmm0\n", sd);
#else
        const char *sa = rs(out, fn, i->a, 0);
        if (strcmp(sa, "eax") != 0)
            fprintf(out, "\tmov eax, %s\n", sa);
        fprintf(out, "\tmovzx eax, word [eax]\n");
        fprintf(out, "\tpush eax\n");
        fprintf(out, "\tcall __skj_extendhfsf\n");     /* eax = single bits */
        fprintf(out, "\tadd " SP ", 4\n");
        fprintf(out, "\tpush eax\n");
        fprintf(out, "\tcvtss2sd %s, dword [" SP "]\n", sd);
        fprintf(out, "\tadd " SP ", 4\n");
#endif
        fwd(out, fn, i->dst, sd);
        break;
    }

    case IR_FSH: {
        const char *sb = frs(out, fn, i->b);
#if X86_BITS == 64
        fprintf(out, "\tcvtsd2ss xmm0, %s\n", sb);
        fprintf(out, "\tmovd edi, xmm0\n");             /* single bits -> arg */
        fprintf(out, "\tcall __skj_truncsfhf\n");        /* eax = half bits */
        fprintf(out, "\tmov ecx, eax\n");                /* save half (call-safe reg) */
        {
            const char *sa = ars(out, fn, i->a, 0);      /* reload addr after call */
            fprintf(out, "\tmov word [%s], cx\n", sa);
        }
#else
        const char *sa = rs(out, fn, i->a, 0);
        fprintf(out, "\tcvtsd2ss xmm0, %s\n", sb);       /* round double -> single */
        fprintf(out, "\tsub " SP ", 4\n");
        fprintf(out, "\tmovss dword [" SP "], xmm0\n");
        fprintf(out, "\tcall __skj_truncsfhf\n");        /* arg at [esp]; eax = half */
        fprintf(out, "\tadd " SP ", 4\n");
        fprintf(out, "\tmov edx, eax\n");                /* half bits (low 16) */
        fprintf(out, "\tmov eax, %s\n", sa);             /* store address */
        fprintf(out, "\tmov word [eax], dx\n");
#endif
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

    case IR_RESUME: {
#if X86_BITS == 64
        const char *sa = rsq(out, fn, i->a, 0);
        const char *sb = rsq(out, fn, i->b, 1);
#else
        const char *sa = rs(out, fn, i->a, 0);
        const char *sb = rs(out, fn, i->b, 1);
#endif
        fprintf(out, "\tpush %s\n", sb);
        fprintf(out, "\tpush %s\n", sa);
        fprintf(out, "\tcall __cont_resume\n");
        break;
    }

    case IR_FUNC:
    case IR_ENDF:
        break;

    /* ---- I64 opcodes ---- */

#if X86_BITS == 64
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

#if defined(CC_PSABI)
    case IR_VA_START: {
        /* fill the va_list at [i->a] from the named-param counts and the
           register save area (SysV) */
        int ng = 0, nf = 0, ns = 0, k, j;
        for (k = 0; k < fn->nparams; k++) {
            struct sysv_ploc p;
            sysv_param(fn, k, &p);
            if (p.cls[0] == -2) {       /* MEMORY struct: all stack */
                ns += p.neb;
                continue;
            }
            for (j = 0; j < p.neb; j++) {
                if (p.cls[j] == 1 && nf < 8) nf++;
                else if (p.cls[j] != 1 && ng < 6) ng++;
                else ns++;
            }
        }
        int r = fn->temp_reg[i->a];
        const char *ap;
        if (r >= 0) {
            ap = qregs[r];
        } else {
            fprintf(out, "\tmov r11, [" BP "%+d]\n",
                spill_byte_offset(fn, i->a));
            ap = "r11";
        }
        fprintf(out, "\tmov dword [%s], %d\n", ap, 8 * ng);
        fprintf(out, "\tmov dword [%s+4], %d\n", ap, 48 + 16 * nf);
        fprintf(out, "\tlea rax, [rbp+%d]\n", 16 + 8 * ns);
        fprintf(out, "\tmov [%s+8], rax\n", ap);
        fprintf(out, "\tlea rax, [rbp%+d]\n", -frame_reserve(fn));
        fprintf(out, "\tmov [%s+16], rax\n", ap);
        break;
    }
#endif

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
        if (narg >= 16)
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
#else
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
            fprintf(out, "\tpush dword [" BP "%+d]\n", boff - 4);
            fprintf(out, "\tpush dword [" BP "%+d]\n", boff);
        }
        if (ap >= 0) {
            fprintf(out, "\tpush edi\n");
            fprintf(out, "\tpush esi\n");
        } else {
            int aoff = i64spill_byte_offset(fn, i->a);
            fprintf(out, "\tpush dword [" BP "%+d]\n", aoff - 4);
            fprintf(out, "\tpush dword [" BP "%+d]\n", aoff);
        }
        fprintf(out, "\tcall __muldi3\n");
        fprintf(out, "\tadd " SP ", 16\n");
        if (dp >= 0) {
            fprintf(out, "\tmov esi, eax\n");
            fprintf(out, "\tmov edi, edx\n");
        } else {
            int doff = i64spill_byte_offset(fn, i->dst);
            fprintf(out, "\tmov [" BP "%+d], eax\n", doff);
            fprintf(out, "\tmov [" BP "%+d], edx\n", doff - 4);
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
        fprintf(out, "\tadd " SP ", 12\n");
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
        fprintf(out, "\tmov %s, [" BP "%+d]\n", dhi, off);
        fprintf(out, "\tmov %s, [" BP "%+d]\n", dlo, off + 4);
        i64_wd(out, fn, i->dst, dhi, dlo);
        break;
    }

    case IR_STL64: {
        const char *ahi, *alo;
        int off = slot_offset(fn, i->slot);

        i64_rs(out, fn, i->a, &ahi, &alo);
        fprintf(out, "\tmov [" BP "%+d], %s\n", off, ahi);
        fprintf(out, "\tmov [" BP "%+d], %s\n", off + 4, alo);
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
        arg_mem[narg] = 0;
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
#endif /* X86_BITS */

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
                 i->op == IR_CALL_AGG)) {
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
#if X86_BITS == 32
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
#endif
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
    /* mark the stack non-executable so the GNU linker does not warn (and
       default to an executable stack) about the missing note */
    fputs("\nsection .note.GNU-stack noalloc noexec nowrite progbits\n", out);
}
