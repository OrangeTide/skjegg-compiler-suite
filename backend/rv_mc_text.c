/* rv_mc_text.c : the GAS text sink (see rv_mc_text.h) plus the RISC-V AOT driver
   (target_emit).

   One implementation of backend/rv_mc.h that writes RV32 (RV32IMFD + Zfh) GAS
   assembly.  Each rv_mc_* call prints one instruction (or the assembler-assisted
   form of a large frame offset routed through t6); the selector drives it exactly
   as it drives the JIT's byte sink, so the two paths emit the same code from one
   selection logic.  Registers arrive as hardware numbers and are indexed into the
   ABI-name tables.  The RISC-V parallel of backend/arm64_mc_text.c. */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "rv_mc.h"
#include "rv_mc_text.h"
#include "rv_select.h"

/* ---- Optional recording sink + local peephole (prototype) --------------------
   When SKJ_RV_PEEPHOLE is set in the environment, each rv_mc_* call records a
   tagged machine op into a per-function buffer instead of printing straight to
   out.  After the selector finishes a function, a block-local peephole runs over
   the buffer (constant/address reuse plus dead-materialisation and self-move
   removal) and the survivors are flushed to out.  With the variable unset the
   emit helpers fall straight through to fputs, so the output is byte-identical to
   the direct sink.  This is the RISC-V proving ground for the buffered-MC
   peephole described in doc/internals/three-address-opt.md. */

enum { K_OTHER = 0, K_LI, K_LA, K_LEA, K_MV, K_LOAD, K_STORE };

/* Exact memory mnemonic, so forwarding respects sign/zero extension: a value read
   by lb is not the value read by lbu, and only a full word (sw then lw) can be
   forwarded from a stored register. */
enum { MC_LB = 1, MC_LH, MC_LW, MC_LBU, MC_LHU, MC_SB, MC_SH, MC_SW };

struct rvop {
    char text[128];         /* the formatted assembly, one or more lines */
    unsigned char kind;     /* K_OTHER / K_LI / K_LA / K_LEA / K_MV / K_LOAD / K_STORE */
    unsigned char clears;   /* a barrier: clear the known-value map after it */
    unsigned char dead;     /* dropped by the peephole */
    short wreg;             /* integer reg this op defines, or -1 */
    short rd, rs;           /* materialisation dst / move dst,src */
    long imm;               /* K_LI value */
    const char *sym;        /* K_LA symbol */
    int labid;              /* K_LEA label id */
    /* K_LOAD / K_STORE: a base+offset memory access. */
    short mbase;            /* base register */
    int moff;              /* byte offset */
    short mw;              /* access width in bytes (1/2/4/8) */
    short mvreg;           /* the GPR moved to/from memory, or -1 for a float access */
    short mcode;           /* exact mnemonic (MC_*), 0 for a float access */
};

struct rv_mc {
    FILE *out;
    int serial;             /* per-function label namespace */
    int nlab;               /* next label id */
    const char *errsym;     /* parity with the byte sink; never set here */
    int rec;                /* recording enabled (SKJ_RV_PEEPHOLE) */
    struct rvop *ops;       /* per-function recorded ops */
    int nops, cap;
};

static struct rvop *
nextop(struct rv_mc *m)
{
    if (m->nops == m->cap) {
        int nc = m->cap ? m->cap * 2 : 256;
        struct rvop *p = realloc(m->ops, (size_t)nc * sizeof *p);
        if (!p)
            return NULL;
        m->ops = p;
        m->cap = nc;
    }
    struct rvop *o = &m->ops[m->nops++];
    memset(o, 0, sizeof *o);
    o->wreg = -1;
    return o;
}

/* Record a plain (K_OTHER) op whose text is already formatted. */
static void
emit_rec(struct rv_mc *m, const char *text, int wreg, int clears)
{
    struct rvop *o;
    if (!m->rec || !(o = nextop(m))) {
        fputs(text, m->out);
        return;
    }
    o->kind = K_OTHER;
    o->wreg = (short)wreg;
    o->clears = (unsigned char)clears;
    snprintf(o->text, sizeof o->text, "%s", text);
}

/* printf-style convenience over emit_rec. */
static void
E(struct rv_mc *m, int wreg, int clears, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit_rec(m, buf, wreg, clears);
}

/* ABI register names by hardware number. */
static const char *
xn(int r)
{
    static const char *t[32] = {
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
        "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
    };
    return t[r & 31];
}

static const char *
fnn(int r)
{
    static const char *t[32] = {
        "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
        "fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
        "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
        "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
    };
    return t[r & 31];
}

struct rv_mc *
rvt_new(FILE *out)
{
    struct rv_mc *m = calloc(1, sizeof *m);
    if (m) {
        m->out = out;
        m->rec = getenv("SKJ_RV_PEEPHOLE") != NULL;
    }
    return m;
}

void rvt_free(struct rv_mc *m) { if (m) free(m->ops); free(m); }

void
rvt_begin_func(struct rv_mc *m, int serial)
{
    m->serial = serial;
    m->nlab = 0;
    m->nops = 0;
}

/* Record a materialisation (li / la / la-of-label) or a move, which the peephole
   reasons about by value identity.  text is the already-formatted line. */
static void
rec_mat(struct rv_mc *m, int kind, int rd, int rs, long imm,
        const char *sym, int labid, const char *text)
{
    struct rvop *o;
    if (!m->rec || !(o = nextop(m))) {
        fputs(text, m->out);
        return;
    }
    o->kind = (unsigned char)kind;
    o->rd = (short)rd;
    o->rs = (short)rs;
    o->wreg = (short)rd;
    o->imm = imm;
    o->sym = sym;
    o->labid = labid;
    snprintf(o->text, sizeof o->text, "%s", text);
}

const char *rvt_errsym(struct rv_mc *m) { return m->errsym; }

/* Debug info (-g): the text sink has no address-to-line table to build, so it
   records the line as a comment for a human reading the assembly.  The real
   address mapping is built by the JIT byte sink and, later, by the DWARF line
   program the assembler will emit. */
void rv_mc_loc(struct rv_mc *m, int line)
{ E(m, -1, 0, "\t.loc 1 %d\n", line); }

/****************************************************************
 * Base-relative load/store with large-offset lowering (mirrors rv_emit.c's
 * emit_mem: a 12-bit signed immediate reaches only -2048..+2047 from the base,
 * so an out-of-range offset is materialised in t6).
 ****************************************************************/

#define IMM12_MIN (-2048)
#define IMM12_MAX 2047

/* A base+offset load or store.  isfloat picks the register bank for the operand
   name, regnum is its hardware number, w is the access width, and isload says
   whether it defines regnum (a load) or reads it (a store).  The in-range form is
   recorded as a structured K_LOAD/K_STORE the peephole can forward through; the
   large-offset form routes through t6, so it is a plain barrier. */
static void
memop(struct rv_mc *m, const char *mnem, int code, int isfloat, int regnum,
      int base, int off, int w, int isload)
{
    const char *reg = isfloat ? fnn(regnum) : xn(regnum);
    int wreg = (isload && !isfloat) ? regnum : -1;

    if (off < IMM12_MIN || off > IMM12_MAX) {
        E(m, wreg, 1, "\tli t6, %d\n\tadd t6, %s, t6\n\t%s %s, 0(t6)\n",
          off, xn(base), mnem, reg);
        return;
    }
    if (!m->rec) {
        fprintf(m->out, "\t%s %s, %d(%s)\n", mnem, reg, off, xn(base));
        return;
    }
    struct rvop *o = nextop(m);
    if (!o) {
        fprintf(m->out, "\t%s %s, %d(%s)\n", mnem, reg, off, xn(base));
        return;
    }
    o->kind = (unsigned char)(isload ? K_LOAD : K_STORE);
    o->wreg = (short)wreg;
    o->mbase = (short)base;
    o->moff = off;
    o->mw = (short)w;
    o->mvreg = (short)(isfloat ? -1 : regnum);
    o->mcode = (short)code;
    snprintf(o->text, sizeof o->text, "\t%s %s, %d(%s)\n", mnem, reg, off, xn(base));
}

/****************************************************************
 * Control flow
 ****************************************************************/

int rv_mc_new_label(struct rv_mc *m) { return m->nlab++; }
/* Labels are join points and the calls/jumps clobber or end a straight-line run,
   so each clears the known-value map.  Conditional branches do not: the registers
   survive the branch, and the target label clears on its own. */
void rv_mc_label(struct rv_mc *m, int id) { E(m, -1, 1, ".L%d_%d:\n", m->serial, id); }
void rv_mc_j(struct rv_mc *m, int id) { E(m, -1, 1, "\tj .L%d_%d\n", m->serial, id); }
void rv_mc_beqz(struct rv_mc *m, int rs, int id)
{ E(m, -1, 0, "\tbeqz %s, .L%d_%d\n", xn(rs), m->serial, id); }
void rv_mc_bnez(struct rv_mc *m, int rs, int id)
{ E(m, -1, 0, "\tbnez %s, .L%d_%d\n", xn(rs), m->serial, id); }
void rv_mc_bne(struct rv_mc *m, int rs1, int rs2, int id)
{ E(m, -1, 0, "\tbne %s, %s, .L%d_%d\n", xn(rs1), xn(rs2), m->serial, id); }
void rv_mc_ret(struct rv_mc *m) { E(m, -1, 1, "\tret\n"); }
void rv_mc_jr(struct rv_mc *m, int rs) { E(m, -1, 1, "\tjr %s\n", xn(rs)); }
void rv_mc_j_sym(struct rv_mc *m, const char *sym) { E(m, -1, 1, "\tj %s\n", sym); }
void rv_mc_call(struct rv_mc *m, const char *sym) { E(m, -1, 1, "\tjal ra, %s\n", sym); }
void rv_mc_jalr(struct rv_mc *m, int rs) { E(m, -1, 1, "\tjalr ra, %s, 0\n", xn(rs)); }
void rv_mc_lea_label(struct rv_mc *m, int rd, int id)
{
    char b[128];
    snprintf(b, sizeof b, "\tla %s, .L%d_%d\n", xn(rd), m->serial, id);
    rec_mat(m, K_LEA, rd, 0, 0, NULL, id, b);
}

/****************************************************************
 * Moves / immediates / addresses
 ****************************************************************/

void rv_mc_li(struct rv_mc *m, int rd, long imm)
{
    char b[128];
    snprintf(b, sizeof b, "\tli %s, %ld\n", xn(rd), imm);
    rec_mat(m, K_LI, rd, 0, imm, NULL, 0, b);
}
void rv_mc_la(struct rv_mc *m, int rd, const char *sym)
{
    char b[128];
    snprintf(b, sizeof b, "\tla %s, %s\n", xn(rd), sym);
    rec_mat(m, K_LA, rd, 0, 0, sym, 0, b);
}
void rv_mc_mv(struct rv_mc *m, int rd, int rs)
{
    char b[128];
    snprintf(b, sizeof b, "\tmv %s, %s\n", xn(rd), xn(rs));
    rec_mat(m, K_MV, rd, rs, 0, NULL, 0, b);
}

void
rv_mc_addbig(struct rv_mc *m, int rd, int base, int imm)
{
    if (imm >= IMM12_MIN && imm <= IMM12_MAX) {
        E(m, rd, 0, "\taddi %s, %s, %d\n", xn(rd), xn(base), imm);
        return;
    }
    /* clobbers t6: a barrier. */
    E(m, rd, 1, "\tli t6, %d\n\tadd %s, %s, t6\n", imm, xn(rd), xn(base));
}

void rv_mc_addi(struct rv_mc *m, int rd, int rs, int imm) { E(m, rd, 0, "\taddi %s, %s, %d\n", xn(rd), xn(rs), imm); }
void rv_mc_andi(struct rv_mc *m, int rd, int rs, int imm) { E(m, rd, 0, "\tandi %s, %s, %d\n", xn(rd), xn(rs), imm); }
void rv_mc_xori(struct rv_mc *m, int rd, int rs, int imm) { E(m, rd, 0, "\txori %s, %s, %d\n", xn(rd), xn(rs), imm); }
void rv_mc_srai(struct rv_mc *m, int rd, int rs, int shamt) { E(m, rd, 0, "\tsrai %s, %s, %d\n", xn(rd), xn(rs), shamt); }
void rv_mc_slli(struct rv_mc *m, int rd, int rs, int shamt) { E(m, rd, 0, "\tslli %s, %s, %d\n", xn(rd), xn(rs), shamt); }
void rv_mc_srli(struct rv_mc *m, int rd, int rs, int shamt) { E(m, rd, 0, "\tsrli %s, %s, %d\n", xn(rd), xn(rs), shamt); }

/****************************************************************
 * Integer arithmetic and logic
 ****************************************************************/

static void
r3(struct rv_mc *m, const char *mnem, int rd, int rs1, int rs2)
{ E(m, rd, 0, "\t%s %s, %s, %s\n", mnem, xn(rd), xn(rs1), xn(rs2)); }

void rv_mc_add(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "add", rd, rs1, rs2); }
void rv_mc_sub(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "sub", rd, rs1, rs2); }
void rv_mc_mul(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "mul", rd, rs1, rs2); }
void rv_mc_mulh(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "mulh", rd, rs1, rs2); }
void rv_mc_and(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "and", rd, rs1, rs2); }
void rv_mc_or(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "or", rd, rs1, rs2); }
void rv_mc_xor(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "xor", rd, rs1, rs2); }
void rv_mc_sll(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "sll", rd, rs1, rs2); }
void rv_mc_sra(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "sra", rd, rs1, rs2); }
void rv_mc_srl(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "srl", rd, rs1, rs2); }
void rv_mc_div(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "div", rd, rs1, rs2); }
void rv_mc_divu(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "divu", rd, rs1, rs2); }
void rv_mc_rem(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "rem", rd, rs1, rs2); }
void rv_mc_remu(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "remu", rd, rs1, rs2); }
void rv_mc_slt(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "slt", rd, rs1, rs2); }
void rv_mc_sltu(struct rv_mc *m, int rd, int rs1, int rs2) { r3(m, "sltu", rd, rs1, rs2); }

void rv_mc_neg(struct rv_mc *m, int rd, int rs) { E(m, rd, 0, "\tneg %s, %s\n", xn(rd), xn(rs)); }
void rv_mc_not(struct rv_mc *m, int rd, int rs) { E(m, rd, 0, "\tnot %s, %s\n", xn(rd), xn(rs)); }
void rv_mc_seqz(struct rv_mc *m, int rd, int rs) { E(m, rd, 0, "\tseqz %s, %s\n", xn(rd), xn(rs)); }
void rv_mc_snez(struct rv_mc *m, int rd, int rs) { E(m, rd, 0, "\tsnez %s, %s\n", xn(rd), xn(rs)); }

/****************************************************************
 * Loads and stores
 ****************************************************************/

void rv_mc_lb(struct rv_mc *m, int rd, int base, int off) { memop(m, "lb", MC_LB, 0, rd, base, off, 1, 1); }
void rv_mc_lh(struct rv_mc *m, int rd, int base, int off) { memop(m, "lh", MC_LH, 0, rd, base, off, 2, 1); }
void rv_mc_lw(struct rv_mc *m, int rd, int base, int off) { memop(m, "lw", MC_LW, 0, rd, base, off, 4, 1); }
void rv_mc_lbu(struct rv_mc *m, int rd, int base, int off) { memop(m, "lbu", MC_LBU, 0, rd, base, off, 1, 1); }
void rv_mc_lhu(struct rv_mc *m, int rd, int base, int off) { memop(m, "lhu", MC_LHU, 0, rd, base, off, 2, 1); }
void rv_mc_sb(struct rv_mc *m, int rs, int base, int off) { memop(m, "sb", MC_SB, 0, rs, base, off, 1, 0); }
void rv_mc_sh(struct rv_mc *m, int rs, int base, int off) { memop(m, "sh", MC_SH, 0, rs, base, off, 2, 0); }
void rv_mc_sw(struct rv_mc *m, int rs, int base, int off) { memop(m, "sw", MC_SW, 0, rs, base, off, 4, 0); }

void rv_mc_flw(struct rv_mc *m, int frd, int base, int off) { memop(m, "flw", 0, 1, frd, base, off, 4, 1); }
void rv_mc_fld(struct rv_mc *m, int frd, int base, int off) { memop(m, "fld", 0, 1, frd, base, off, 8, 1); }
void rv_mc_flh(struct rv_mc *m, int frd, int base, int off) { memop(m, "flh", 0, 1, frd, base, off, 2, 1); }
void rv_mc_fsw(struct rv_mc *m, int frs, int base, int off) { memop(m, "fsw", 0, 1, frs, base, off, 4, 0); }
void rv_mc_fsd(struct rv_mc *m, int frs, int base, int off) { memop(m, "fsd", 0, 1, frs, base, off, 8, 0); }
void rv_mc_fsh(struct rv_mc *m, int frs, int base, int off) { memop(m, "fsh", 0, 1, frs, base, off, 2, 0); }

/****************************************************************
 * Float data processing
 ****************************************************************/

static void
f3(struct rv_mc *m, const char *base, int f32, int fd, int fs1, int fs2)
{ E(m, -1, 0, "\t%s.%s %s, %s, %s\n", base, f32 ? "s" : "d", fnn(fd), fnn(fs1), fnn(fs2)); }

void rv_mc_fadd(struct rv_mc *m, int f32, int fd, int fs1, int fs2) { f3(m, "fadd", f32, fd, fs1, fs2); }
void rv_mc_fsub(struct rv_mc *m, int f32, int fd, int fs1, int fs2) { f3(m, "fsub", f32, fd, fs1, fs2); }
void rv_mc_fmul(struct rv_mc *m, int f32, int fd, int fs1, int fs2) { f3(m, "fmul", f32, fd, fs1, fs2); }
void rv_mc_fdiv(struct rv_mc *m, int f32, int fd, int fs1, int fs2) { f3(m, "fdiv", f32, fd, fs1, fs2); }

void rv_mc_fneg(struct rv_mc *m, int f32, int fd, int fs)
{ E(m, -1, 0, "\tfneg.%s %s, %s\n", f32 ? "s" : "d", fnn(fd), fnn(fs)); }
void rv_mc_fabs(struct rv_mc *m, int f32, int fd, int fs)
{ E(m, -1, 0, "\tfabs.%s %s, %s\n", f32 ? "s" : "d", fnn(fd), fnn(fs)); }
void rv_mc_fsqrt(struct rv_mc *m, int f32, int fd, int fs)
{ E(m, -1, 0, "\tfsqrt.%s %s, %s\n", f32 ? "s" : "d", fnn(fd), fnn(fs)); }

void rv_mc_feq(struct rv_mc *m, int f32, int rd, int fs1, int fs2)
{ E(m, rd, 0, "\tfeq.%s %s, %s, %s\n", f32 ? "s" : "d", xn(rd), fnn(fs1), fnn(fs2)); }
void rv_mc_flt(struct rv_mc *m, int f32, int rd, int fs1, int fs2)
{ E(m, rd, 0, "\tflt.%s %s, %s, %s\n", f32 ? "s" : "d", xn(rd), fnn(fs1), fnn(fs2)); }
void rv_mc_fle(struct rv_mc *m, int f32, int rd, int fs1, int fs2)
{ E(m, rd, 0, "\tfle.%s %s, %s, %s\n", f32 ? "s" : "d", xn(rd), fnn(fs1), fnn(fs2)); }

void rv_mc_fcvt_f_w(struct rv_mc *m, int f32, int fd, int rs)
{ E(m, -1, 0, "\tfcvt.%s.w %s, %s\n", f32 ? "s" : "d", fnn(fd), xn(rs)); }
void rv_mc_fcvt_w_f(struct rv_mc *m, int f32, int rd, int fs)
{ E(m, rd, 0, "\tfcvt.w.%s %s, %s, rtz\n", f32 ? "s" : "d", xn(rd), fnn(fs)); }
void rv_mc_fround(struct rv_mc *m, int f32, int rd, int fs)
{ E(m, rd, 0, "\tfcvt.w.%s %s, %s, rne\n", f32 ? "s" : "d", xn(rd), fnn(fs)); }
void rv_mc_fcvt_d_s(struct rv_mc *m, int fd, int fs) { E(m, -1, 0, "\tfcvt.d.s %s, %s\n", fnn(fd), fnn(fs)); }
void rv_mc_fcvt_s_d(struct rv_mc *m, int fd, int fs) { E(m, -1, 0, "\tfcvt.s.d %s, %s\n", fnn(fd), fnn(fs)); }
void rv_mc_fcvt_d_h(struct rv_mc *m, int fd, int fs) { E(m, -1, 0, "\tfcvt.d.h %s, %s\n", fnn(fd), fnn(fs)); }
void rv_mc_fcvt_h_d(struct rv_mc *m, int fd, int fs) { E(m, -1, 0, "\tfcvt.h.d %s, %s\n", fnn(fd), fnn(fs)); }
void rv_mc_fmv_x_w(struct rv_mc *m, int rd, int fs) { E(m, rd, 0, "\tfmv.x.w %s, %s\n", xn(rd), fnn(fs)); }
void rv_mc_fmv_w_x(struct rv_mc *m, int fd, int rs) { E(m, -1, 0, "\tfmv.w.x %s, %s\n", fnn(fd), xn(rs)); }

/****************************************************************
 * Inline asm
 ****************************************************************/

void rv_mc_asm(struct rv_mc *m, const char *text) { E(m, -1, 1, "%s\n", text); }

/****************************************************************
 * Globals (ported from rv_emit.c)
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
            fprintf(out, "\t.word %s\n", sym_ref(ref, sizeof ref, it->sym, it->ival));
        } else if (it->size == 8) {
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

/* true when a global is zero-filled (no initializer): it belongs in .bss
   (NOBITS), so its bytes cost no file space, rather than in .data. */
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
    case IR_I64: return 8;
    case IR_F64: return 8;
    default:     return 4;          /* incl. IR_F32 (4-byte bit pattern) */
    }
}

/* alignment (power-of-two exponent) + optional .globl + label */
static void
emit_global_head(FILE *out, struct ir_global *g)
{
    int alb = g->base_type == IR_F64 ? 8 : 4, e = 0;
    if (g->align > alb)
        alb = g->align;
    while ((1 << e) < alb)
        e++;
    fprintf(out, "\t.align %d\n", e);
    if (!g->is_local)
        fprintf(out, "\t.globl %s\n", g->name);
    fprintf(out, "%s:\n", g->name);
}

static void
emit_global_def(FILE *out, struct ir_global *g)
{
    int elsz = global_elsz(g);

    if (g->is_strobj) {
        /* A static Kobold string object: the {refcount, kind, len, cap} header
           in target endianness, then the text and a trailing NUL.  cap == len:
           a static string carries no spare capacity.  The header words assemble
           per-target (little-endian here), where str_core.c reads them with
           native loads. */
        fprintf(out, "\t.word 0x%08x\n", 0xffffffffu);  /* KP_RC_STATIC */
        fprintf(out, "\t.word 0x%08x\n", 1u);           /* KP_OBJ_STRING */
        fprintf(out, "\t.word %d\n", g->init_strlen);   /* byte length */
        fprintf(out, "\t.word %d\n", g->init_strlen);   /* capacity (== length) */
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
                fprintf(out, "\t.word %s\n",
                    sym_ref(ref, sizeof ref, g->init_syms[k], g->init_ivals[k]));
            } else if (g->base_type == IR_I64 || g->base_type == IR_F64) {
                uint64_t bits = (uint64_t)g->init_ivals[k];
                fprintf(out, "\t.word 0x%08x\n", (unsigned)(bits & 0xFFFFFFFF));
                fprintf(out, "\t.word 0x%08x\n", (unsigned)(bits >> 32));
            } else if (elsz == 1)
                fprintf(out, "\t.byte %" PRId64 "\n", g->init_ivals[k]);
            else if (elsz == 2)
                fprintf(out, "\t.short %" PRId64 "\n", g->init_ivals[k]);
            else
                fprintf(out, "\t.word %" PRId64 "\n", g->init_ivals[k]);
        }
        if (g->arr_size > g->init_count)
            fprintf(out, "\t.space %d\n", (g->arr_size - g->init_count) * elsz);
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

    /* Zero-filled globals: .bss (NOBITS), no file space for their zeros. */
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
 * Block-local peephole over the recorded op buffer
 ****************************************************************/

/* RV32 li cost: a value in the signed 12-bit range is one addi, a value whose low
   12 bits are zero is one lui, otherwise lui+addi is two.  Reusing a live copy as
   a one-instruction mv only pays when the li would have been two. */
static int
li_insns(long v)
{
    if (v >= IMM12_MIN && v <= IMM12_MAX)
        return 1;
    if (((unsigned long)v & 0xFFFUL) == 0)
        return 1;
    return 2;
}

struct known {
    int valid;
    int kind;               /* K_LI / K_LA / K_LEA */
    long imm;
    const char *sym;
    int labid;
};

static int
holds(const struct known *k, int kind, long imm, const char *sym, int labid)
{
    if (!k->valid || k->kind != kind)
        return 0;
    if (kind == K_LI)
        return k->imm == imm;
    if (kind == K_LA)
        return k->sym && sym && strcmp(k->sym, sym) == 0;
    if (kind == K_LEA)
        return k->labid == labid;
    return 0;
}

/* The transform.  Within a straight-line run (reset at every clearing op), track
   which integer register holds which materialised constant or address.  A repeat
   materialisation into a register that already holds the value is dropped; a
   materialisation of a value still live in another register becomes a mv, which
   is one instruction where a large li or any la/la-of-label was two or more; a
   self-move is dropped. */
static long rv_peep_drop, rv_peep_reuse, rv_peep_selfmv;
static long rv_peep_ldfwd, rv_peep_lddrop;

/* The slot map: which register currently holds the value of a known memory cell.
   A cell is a (base register, byte offset, width) plus the exact access mnemonic,
   so sign/zero extension is respected.  from_store marks a cell whose value came
   from a store (forwardable only full-word, sw then lw); otherwise it came from a
   load and forwards only to the identical load mnemonic. */
#define NSLOT 32
struct slot {
    int valid, base, off, w, reg, from_store, mcode;
};

static int
mem_overlap(int o1, int w1, int o2, int w2)
{
    return o1 < o2 + w2 && o2 < o1 + w1;
}

/* Conservative alias test: different base registers may point at the same
   address, so they always may-alias; the same base aliases only on byte overlap. */
static int
may_alias(int b1, int o1, int w1, int b2, int o2, int w2)
{
    return b1 != b2 || mem_overlap(o1, w1, o2, w2);
}

/* A write to register r ends any cell that r held or that was addressed through
   r (its base identity is gone). */
static void
slot_kill_reg(struct slot *sl, int r)
{
    int k;
    for (k = 0; k < NSLOT; k++)
        if (sl[k].valid && (sl[k].reg == r || sl[k].base == r))
            sl[k].valid = 0;
}

static void
slot_kill_aliases(struct slot *sl, int base, int off, int w)
{
    int k;
    for (k = 0; k < NSLOT; k++)
        if (sl[k].valid && may_alias(base, off, w, sl[k].base, sl[k].off, sl[k].w))
            sl[k].valid = 0;
}

static void
slot_set(struct slot *sl, int *cursor, int base, int off, int w, int reg,
         int from_store, int mcode)
{
    int k, slot = -1;
    for (k = 0; k < NSLOT; k++) {
        if (sl[k].valid && sl[k].base == base && sl[k].off == off && sl[k].w == w) {
            slot = k;
            break;
        }
        if (slot < 0 && !sl[k].valid)
            slot = k;
    }
    if (slot < 0) {                              /* full: evict round-robin */
        slot = *cursor;
        *cursor = (*cursor + 1) % NSLOT;
    }
    sl[slot].valid = 1;
    sl[slot].base = base;
    sl[slot].off = off;
    sl[slot].w = w;
    sl[slot].reg = reg;
    sl[slot].from_store = from_store;
    sl[slot].mcode = mcode;
}

static void
rv_peephole(struct rv_mc *m)
{
    struct known kn[32];
    struct slot sl[NSLOT];
    int cursor = 0;
    int i, r, k;

    memset(kn, 0, sizeof kn);
    memset(sl, 0, sizeof sl);
    for (i = 0; i < m->nops; i++) {
        struct rvop *o = &m->ops[i];

        if (o->clears) {
            memset(kn, 0, sizeof kn);
            memset(sl, 0, sizeof sl);
            continue;
        }
        switch (o->kind) {
        case K_LI:
        case K_LA:
        case K_LEA: {
            int rd = o->rd, src = -1;

            if (rd == 0)                         /* materialise into x0: leave it */
                break;
            if (holds(&kn[rd], o->kind, o->imm, o->sym, o->labid)) {
                o->dead = 1;                     /* already in rd: redundant */
                rv_peep_drop++;
                continue;
            }
            for (r = 1; r < 32; r++) {
                if (r != rd && holds(&kn[r], o->kind, o->imm, o->sym, o->labid)) {
                    src = r;
                    break;
                }
            }
            if (src >= 0 && (o->kind != K_LI || li_insns(o->imm) >= 2)) {
                snprintf(o->text, sizeof o->text, "\tmv %s, %s\n", xn(rd), xn(src));
                rv_peep_reuse++;
            }
            slot_kill_reg(sl, rd);               /* rd no longer holds its cell */
            kn[rd].valid = 1;
            kn[rd].kind = o->kind;
            kn[rd].imm = o->imm;
            kn[rd].sym = o->sym;
            kn[rd].labid = o->labid;
            break;
        }
        case K_MV: {
            int rd = o->rd, rs = o->rs;

            if (rd == rs) {
                o->dead = 1;                     /* mv rd, rd */
                rv_peep_selfmv++;
                continue;
            }
            slot_kill_reg(sl, rd);
            kn[rd] = kn[rs];                     /* rd now mirrors rs */
            break;
        }
        case K_LOAD: {
            int B = o->mbase, off = o->moff, w = o->mw, rd = o->mvreg;

            if (rd < 0)                          /* float load: no int cell */
                break;
            for (k = 0; k < NSLOT; k++) {
                struct slot *s = &sl[k];
                int ok;
                if (!s->valid || s->base != B || s->off != off)
                    continue;
                ok = s->from_store ? (s->mcode == MC_SW && o->mcode == MC_LW)
                                   : (s->mcode == o->mcode);
                if (!ok)
                    continue;
                if (s->reg == rd) {              /* already in rd: redundant load */
                    o->dead = 1;
                    rv_peep_lddrop++;
                } else {
                    snprintf(o->text, sizeof o->text, "\tmv %s, %s\n",
                             xn(rd), xn(s->reg));
                    rv_peep_ldfwd++;
                }
                break;
            }
            if (o->dead)
                continue;                        /* rd still holds the cell */
            slot_kill_reg(sl, rd);               /* rd is overwritten */
            kn[rd].valid = 0;
            if (B != rd)                         /* record rd = cell, unless we clobbered the base */
                slot_set(sl, &cursor, B, off, w, rd, 0, o->mcode);
            break;
        }
        case K_STORE: {
            int B = o->mbase, off = o->moff, w = o->mw, rs = o->mvreg;

            slot_kill_aliases(sl, B, off, w);
            if (rs >= 0 && o->mcode == MC_SW)    /* only a full word is forwardable */
                slot_set(sl, &cursor, B, off, w, rs, 1, MC_SW);
            break;
        }
        default:
            if (o->wreg >= 0) {
                kn[o->wreg].valid = 0;
                slot_kill_reg(sl, o->wreg);
            }
            break;
        }
    }
}

/* Prototype measurement hook: SKJ_RV_PEEP_STATS prints transform tallies at exit. */
static void
rv_peep_report(void)
{
    if (getenv("SKJ_RV_PEEP_STATS"))
        fprintf(stderr,
                "rv-peep: redundant-drop=%ld reuse->mv=%ld self-mv=%ld "
                "load-fwd=%ld load-drop=%ld\n",
                rv_peep_drop, rv_peep_reuse, rv_peep_selfmv,
                rv_peep_ldfwd, rv_peep_lddrop);
}

static void
rv_flush(struct rv_mc *m)
{
    int i;
    for (i = 0; i < m->nops; i++)
        if (!m->ops[i].dead)
            fputs(m->ops[i].text, m->out);
    m->nops = 0;
}

/* Run after the selector finishes a function: peephole the buffer (when
   recording), then write the survivors.  A no-op when recording is off. */
static void
rvt_end_func(struct rv_mc *m)
{
    if (!m->rec)
        return;
    rv_peephole(m);
    rv_flush(m);
}

/****************************************************************
 * Entry point
 ****************************************************************/

void
target_emit(FILE *out, struct ir_program *prog)
{
    struct ir_func *fn;
    struct rv_mc *m;
    int serial = 0;

    fputs("# skjegg: generated RISC-V (RV32IMFD + Zfh) assembly\n", out);

    /* Under -g a front end sets src_file; emit the DWARF .file hint once so the
       per-statement .loc directives (rv_mc_loc) attach to it.  The assembler
       turns .file/.loc into the .debug_line/.debug_info sections. */
    for (fn = prog->funcs; fn; fn = fn->next) {
        if (fn->src_file) {
            fprintf(out, "\t.file 1 \"%s\"\n", fn->src_file);
            break;
        }
    }

    m = rvt_new(out);
    if (!m) {
        fputs("rv_mc_text: out of memory\n", stderr);
        exit(1);
    }
    atexit(rv_peep_report);
    for (fn = prog->funcs; fn; fn = fn->next) {
        char err[128];
        int fserial = ++serial;

        fprintf(out, "\n\t.text\n\t.align 2\n");
        if (!fn->is_local)
            fprintf(out, "\t.globl %s\n", fn->name);
        /* STT_FUNC + a size so a debugger maps an address to this function and
           a backtrace knows its extent; the function body stays wholly in
           .text, so .Lfunc_end - name is its byte length. */
        fprintf(out, "\t.type %s, @function\n", fn->name);
        fprintf(out, "%s:\n", fn->name);

        rvt_begin_func(m, fserial);
        if (rv_select_func(m, fn,
                           target_aot_trap_guarded ? RV_TRAP_GUARDED
                                                   : RV_TRAP_HARDWARE,
                           err, sizeof err) != 0) {
            fprintf(stderr, "rv_mc_text: %s\n", err);
            exit(1);
        }
        rvt_end_func(m);
        fprintf(out, ".Lfunc_end%d:\n", fserial);
        fprintf(out, "\t.size %s, .Lfunc_end%d-%s\n",
                fn->name, fserial, fn->name);
    }
    rvt_free(m);

    emit_globals(out, prog);
}
