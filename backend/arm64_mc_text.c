/* arm64_mc_text.c : the GAS text sink (see arm64_mc_text.h) plus the AArch64 AOT
   driver (target_emit).

   One implementation of backend/arm64_mc.h that writes AArch64 GAS assembly.
   Each mc64_* call prints one instruction (or the assembler-assisted form of a
   materialised immediate / symbol / out-of-range memory offset); the selector
   drives it exactly as it drives the JIT's byte sink, so the two paths emit the
   same code from one selection logic.  Registers arrive as hardware numbers and
   are indexed into the name tables; a width rides the sf/sz argument.  The
   AArch64 parallel of backend/mc_text.c + backend/x86_aot.c. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir.h"
#include "arm64_mc.h"
#include "arm64_mc_text.h"
#include "arm64_select.h"

struct mc64 {
    FILE *out;
    int serial;             /* per-function label namespace */
    int nlab;               /* next label id */
    const char *errsym;     /* parity with the byte sink; never set here */
};

/* Register names by hardware number.  31 prints as the stack pointer, which is
   the only role register 31 takes in a printed operand here (the zero-register
   forms are emitted through the cmp/neg/mvn/cset pseudo-mnemonics, which do not
   name it). */
static const char *
xn(int r)
{
    static const char *t[32] = {
        "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11",
        "x12","x13","x14","x15","x16","x17","x18","x19","x20","x21","x22",
        "x23","x24","x25","x26","x27","x28","x29","x30","sp",
    };
    return t[r & 31];
}

static const char *
wn(int r)
{
    static const char *t[32] = {
        "w0","w1","w2","w3","w4","w5","w6","w7","w8","w9","w10","w11",
        "w12","w13","w14","w15","w16","w17","w18","w19","w20","w21","w22",
        "w23","w24","w25","w26","w27","w28","w29","w30","wsp",
    };
    return t[r & 31];
}

static const char *gn(int sf, int r) { return sf ? xn(r) : wn(r); }

/* FP/SIMD register name: sz 1 = half (h), 2 = single (s), 3 = double (d). */
static const char *
fn(int sz, int r)
{
    static const char *h[32] = {
        "h0","h1","h2","h3","h4","h5","h6","h7","h8","h9","h10","h11",
        "h12","h13","h14","h15","h16","h17","h18","h19","h20","h21","h22",
        "h23","h24","h25","h26","h27","h28","h29","h30","h31",
    };
    static const char *s[32] = {
        "s0","s1","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11",
        "s12","s13","s14","s15","s16","s17","s18","s19","s20","s21","s22",
        "s23","s24","s25","s26","s27","s28","s29","s30","s31",
    };
    static const char *d[32] = {
        "d0","d1","d2","d3","d4","d5","d6","d7","d8","d9","d10","d11",
        "d12","d13","d14","d15","d16","d17","d18","d19","d20","d21","d22",
        "d23","d24","d25","d26","d27","d28","d29","d30","d31",
    };
    return (sz == 1 ? h : sz == 2 ? s : d)[r & 31];
}

static const char *ccname[16] = {
    "eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","al","nv",
};

struct mc64 *
mct64_new(FILE *out)
{
    struct mc64 *m = calloc(1, sizeof *m);
    if (m)
        m->out = out;
    return m;
}

void mct64_free(struct mc64 *m) { free(m); }

void
mct64_begin_func(struct mc64 *m, int serial)
{
    m->serial = serial;
    m->nlab = 0;
}

const char *mct64_errsym(struct mc64 *m) { return m->errsym; }

/****************************************************************
 * addimm / memop: the assembler-assisted forms.  These mirror
 * backend/arm64_emit.c's helpers so the text output stays behavior-identical.
 ****************************************************************/

static void
addimm(struct mc64 *m, const char *dst, const char *src, long imm)
{
    const char *op;
    long a;

    if (imm == 0) {
        if (strcmp(dst, src) != 0)
            fprintf(m->out, "\tmov %s, %s\n", dst, src);
        return;
    }
    op = imm > 0 ? "add" : "sub";
    a = imm > 0 ? imm : -imm;
    if (a <= 4095)
        fprintf(m->out, "\t%s %s, %s, #%ld\n", op, dst, src, a);
    else if ((a & 0xfff) == 0 && (a >> 12) <= 4095)
        fprintf(m->out, "\t%s %s, %s, #%ld, lsl #12\n", op, dst, src, a >> 12);
    else {
        fprintf(m->out, "\tldr x15, =%ld\n", imm);
        fprintf(m->out, "\tadd %s, %s, x15\n", dst, src);
    }
}

/* <mnem> reg, [base, #off], with a scratch-base fallback for a large offset.
   scale is the access size in bytes (for the offset range check). */
static void
memop(struct mc64 *m, const char *mnem, int scale, const char *reg, int base, int off)
{
    int fits = (off >= -256 && off <= 255) ||
               (off >= 0 && (off % scale) == 0 && (off / scale) <= 4095);
    if (fits) {
        fprintf(m->out, "\t%s %s, [%s, #%d]\n", mnem, reg, xn(base), off);
    } else {
        addimm(m, "x15", xn(base), off);
        fprintf(m->out, "\t%s %s, [x15]\n", mnem, reg);
    }
}

/****************************************************************
 * Control flow
 ****************************************************************/

int mc64_new_label(struct mc64 *m) { return m->nlab++; }
void mc64_label(struct mc64 *m, int id) { fprintf(m->out, ".L%d_%d:\n", m->serial, id); }
void mc64_b(struct mc64 *m, int id) { fprintf(m->out, "\tb .L%d_%d\n", m->serial, id); }
void mc64_cbz(struct mc64 *m, int sf, int Rt, int id)
{ fprintf(m->out, "\tcbz %s, .L%d_%d\n", gn(sf, Rt), m->serial, id); }
void mc64_cbnz(struct mc64 *m, int sf, int Rt, int id)
{ fprintf(m->out, "\tcbnz %s, .L%d_%d\n", gn(sf, Rt), m->serial, id); }
void mc64_ret(struct mc64 *m) { fprintf(m->out, "\tret\n"); }
void mc64_blr(struct mc64 *m, int Rn) { fprintf(m->out, "\tblr %s\n", xn(Rn)); }
void mc64_br(struct mc64 *m, int Rn) { fprintf(m->out, "\tbr %s\n", xn(Rn)); }

/****************************************************************
 * Moves / immediates / addresses
 ****************************************************************/

void mc64_mov(struct mc64 *m, int sf, int Rd, int Rn)
{ fprintf(m->out, "\tmov %s, %s\n", gn(sf, Rd), gn(sf, Rn)); }
void mc64_movimm(struct mc64 *m, int sf, int Rd, uint64_t v)
{ fprintf(m->out, "\tldr %s, =%" PRIu64 "\n", gn(sf, Rd), v); }
void mc64_lea_sym(struct mc64 *m, int sf, int Rd, const char *name)
{ fprintf(m->out, "\tldr %s, =%s\n", gn(sf, Rd), name); }
void mc64_addimm(struct mc64 *m, int Rd, int Rn, long imm)
{ addimm(m, xn(Rd), xn(Rn), imm); }

/****************************************************************
 * Integer arithmetic and logic
 ****************************************************************/

static void
r3(struct mc64 *m, const char *mnem, int sf, int Rd, int Rn, int Rm)
{ fprintf(m->out, "\t%s %s, %s, %s\n", mnem, gn(sf, Rd), gn(sf, Rn), gn(sf, Rm)); }

void mc64_add(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "add", sf, Rd, Rn, Rm); }
void mc64_sub(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "sub", sf, Rd, Rn, Rm); }
void mc64_and(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "and", sf, Rd, Rn, Rm); }
void mc64_orr(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "orr", sf, Rd, Rn, Rm); }
void mc64_eor(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "eor", sf, Rd, Rn, Rm); }
void mc64_mul(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "mul", sf, Rd, Rn, Rm); }
void mc64_sdiv(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "sdiv", sf, Rd, Rn, Rm); }
void mc64_udiv(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "udiv", sf, Rd, Rn, Rm); }
void mc64_lsl(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "lsl", sf, Rd, Rn, Rm); }
void mc64_asr(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "asr", sf, Rd, Rn, Rm); }
void mc64_lsr(struct mc64 *m, int sf, int Rd, int Rn, int Rm) { r3(m, "lsr", sf, Rd, Rn, Rm); }

void mc64_msub(struct mc64 *m, int sf, int Rd, int Rn, int Rm, int Ra)
{ fprintf(m->out, "\tmsub %s, %s, %s, %s\n", gn(sf, Rd), gn(sf, Rn), gn(sf, Rm), gn(sf, Ra)); }
void mc64_neg(struct mc64 *m, int sf, int Rd, int Rm)
{ fprintf(m->out, "\tneg %s, %s\n", gn(sf, Rd), gn(sf, Rm)); }
void mc64_mvn(struct mc64 *m, int sf, int Rd, int Rm)
{ fprintf(m->out, "\tmvn %s, %s\n", gn(sf, Rd), gn(sf, Rm)); }
void mc64_sxtw(struct mc64 *m, int Rd, int Rn)
{ fprintf(m->out, "\tsxtw %s, %s\n", xn(Rd), wn(Rn)); }

void mc64_cmp(struct mc64 *m, int sf, int Rn, int Rm)
{ fprintf(m->out, "\tcmp %s, %s\n", gn(sf, Rn), gn(sf, Rm)); }
void mc64_cset(struct mc64 *m, int sf, int Rd, int cc)
{ fprintf(m->out, "\tcset %s, %s\n", gn(sf, Rd), ccname[cc & 0xf]); }

/****************************************************************
 * Loads and stores
 ****************************************************************/

void mc64_ldrb(struct mc64 *m, int Rt, int base, int off) { memop(m, "ldrb", 1, wn(Rt), base, off); }
void mc64_ldrsb(struct mc64 *m, int Rt, int base, int off) { memop(m, "ldrsb", 1, wn(Rt), base, off); }
void mc64_ldrh(struct mc64 *m, int Rt, int base, int off) { memop(m, "ldrh", 2, wn(Rt), base, off); }
void mc64_ldrsh(struct mc64 *m, int Rt, int base, int off) { memop(m, "ldrsh", 2, wn(Rt), base, off); }
void mc64_ldrw(struct mc64 *m, int Rt, int base, int off) { memop(m, "ldr", 4, wn(Rt), base, off); }
void mc64_ldrx(struct mc64 *m, int Rt, int base, int off) { memop(m, "ldr", 8, xn(Rt), base, off); }
void mc64_strb(struct mc64 *m, int Rt, int base, int off) { memop(m, "strb", 1, wn(Rt), base, off); }
void mc64_strh(struct mc64 *m, int Rt, int base, int off) { memop(m, "strh", 2, wn(Rt), base, off); }
void mc64_strw(struct mc64 *m, int Rt, int base, int off) { memop(m, "str", 4, wn(Rt), base, off); }
void mc64_strx(struct mc64 *m, int Rt, int base, int off) { memop(m, "str", 8, xn(Rt), base, off); }

void mc64_ldr_fp(struct mc64 *m, int sz, int Rt, int base, int off)
{ memop(m, "ldr", 1 << sz, fn(sz, Rt), base, off); }
void mc64_str_fp(struct mc64 *m, int sz, int Rt, int base, int off)
{ memop(m, "str", 1 << sz, fn(sz, Rt), base, off); }

/****************************************************************
 * FP data processing
 ****************************************************************/

static void
f3(struct mc64 *m, const char *mnem, int sz, int Rd, int Rn, int Rm)
{ fprintf(m->out, "\t%s %s, %s, %s\n", mnem, fn(sz, Rd), fn(sz, Rn), fn(sz, Rm)); }

void mc64_fadd(struct mc64 *m, int sz, int Rd, int Rn, int Rm) { f3(m, "fadd", sz, Rd, Rn, Rm); }
void mc64_fsub(struct mc64 *m, int sz, int Rd, int Rn, int Rm) { f3(m, "fsub", sz, Rd, Rn, Rm); }
void mc64_fmul(struct mc64 *m, int sz, int Rd, int Rn, int Rm) { f3(m, "fmul", sz, Rd, Rn, Rm); }
void mc64_fdiv(struct mc64 *m, int sz, int Rd, int Rn, int Rm) { f3(m, "fdiv", sz, Rd, Rn, Rm); }
void mc64_fmov(struct mc64 *m, int sz, int Rd, int Rn) { fprintf(m->out, "\tfmov %s, %s\n", fn(sz, Rd), fn(sz, Rn)); }
void mc64_fneg(struct mc64 *m, int sz, int Rd, int Rn) { fprintf(m->out, "\tfneg %s, %s\n", fn(sz, Rd), fn(sz, Rn)); }
void mc64_fabs(struct mc64 *m, int sz, int Rd, int Rn) { fprintf(m->out, "\tfabs %s, %s\n", fn(sz, Rd), fn(sz, Rn)); }
void mc64_fsqrt(struct mc64 *m, int sz, int Rd, int Rn) { fprintf(m->out, "\tfsqrt %s, %s\n", fn(sz, Rd), fn(sz, Rn)); }
void mc64_fcmp(struct mc64 *m, int sz, int Rn, int Rm) { fprintf(m->out, "\tfcmp %s, %s\n", fn(sz, Rn), fn(sz, Rm)); }
void mc64_fcvt(struct mc64 *m, int dstsz, int srcsz, int Rd, int Rn)
{ fprintf(m->out, "\tfcvt %s, %s\n", fn(dstsz, Rd), fn(srcsz, Rn)); }
void mc64_scvtf(struct mc64 *m, int sz, int Rd, int Rn) { fprintf(m->out, "\tscvtf %s, %s\n", fn(sz, Rd), wn(Rn)); }
void mc64_fcvtzs(struct mc64 *m, int sz, int Rd, int Rn) { fprintf(m->out, "\tfcvtzs %s, %s\n", wn(Rd), fn(sz, Rn)); }
void mc64_fcvtns(struct mc64 *m, int sz, int Rd, int Rn) { fprintf(m->out, "\tfcvtns %s, %s\n", wn(Rd), fn(sz, Rn)); }

/****************************************************************
 * Calls, traps, markers, inline asm
 ****************************************************************/

void mc64_call_sym(struct mc64 *m, const char *name) { fprintf(m->out, "\tbl %s\n", name); }
void mc64_trap_div_zero(struct mc64 *m) { fprintf(m->out, "\tbl kp_trap_div_zero\n"); }
void mc64_trap_overflow(struct mc64 *m) { fprintf(m->out, "\tbl kp_trap_overflow\n"); }
void mc64_loc(struct mc64 *m, int line) { (void)m; (void)line; }   /* AOT ignores -g markers */
void mc64_asm(struct mc64 *m, const char *text) { fprintf(m->out, "%s\n", text); }

/****************************************************************
 * Globals (ported from arm64_emit.c)
 ****************************************************************/

static void
emit_string_bytes(FILE *out, const char *s, int n)
{
    int k;
    fputs("\t.ascii \"", out);
    for (k = 0; k < n; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c == '\\') fputs("\\\\", out);
        else if (c == '"') fputs("\\\"", out);
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c == '\r') fputs("\\r", out);
        else if (c >= 0x20 && c < 0x7f) fputc(c, out);
        else fprintf(out, "\\%03o", c);
    }
    fputs("\"\n", out);
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
        if (it->sym)
            fprintf(out, "\t%s %s\n", it->size == 8 ? ".quad" : ".word", it->sym);
        else if (it->size == 8)
            fprintf(out, "\t.quad 0x%016llx\n", (unsigned long long)(uint64_t)it->ival);
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
            emit_string_bytes(out, g->init_string, g->init_strlen);
        } else if (g->init_count > 0) {
            int k;
            for (k = 0; k < g->init_count; k++) {
                if (g->init_syms && g->init_syms[k])
                    fprintf(out, "\t%s %s\n", elsz == 8 ? ".quad" : ".word", g->init_syms[k]);
                else if (g->base_type == IR_I64 || g->base_type == IR_F64)
                    fprintf(out, "\t.quad %" PRId64 "\n", g->init_ivals[k]);
                else if (elsz == 1)
                    fprintf(out, "\t.byte %" PRId64 "\n", g->init_ivals[k]);
                else if (elsz == 2)
                    fprintf(out, "\t.short %" PRId64 "\n", g->init_ivals[k]);
                else
                    fprintf(out, "\t.word %" PRId64 "\n", g->init_ivals[k]);
            }
            if (g->arr_size > g->init_count)
                fprintf(out, "\t.space %d\n", (g->arr_size - g->init_count) * elsz);
        } else {
            int sz = (g->arr_size > 0) ? g->arr_size * elsz : elsz;
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
    struct mc64 *m;
    int serial = 0;

    fputs("// generated AArch64 (ARM64) assembly\n", out);

    m = mct64_new(out);
    if (!m) {
        fputs("arm64_mc_text: out of memory\n", stderr);
        exit(1);
    }
    for (fn = prog->funcs; fn; fn = fn->next) {
        char err[128];

        fputs("\n\t.text\n\t.align 2\n", out);
        if (!fn->is_local)
            fprintf(out, "\t.globl %s\n", fn->name);
        fprintf(out, "%s:\n", fn->name);

        mct64_begin_func(m, ++serial);
        if (arm64_select_func(m, fn, ARM64_TRAP_HARDWARE, err, sizeof err) != 0) {
            fprintf(stderr, "arm64_mc_text: %s\n", err);
            exit(1);
        }
        /* dump this function's literal pool while it is still in range */
        fputs("\t.ltorg\n", out);
    }
    mct64_free(m);

    emit_globals(out, prog);
    fputs("\n\t.section .note.GNU-stack,\"\",%progbits\n", out);
}
