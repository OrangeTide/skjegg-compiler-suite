/* test_fp.c : IEEE-754 conformance tests for the F extension */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The differential tools compare the emulator against qemu. These tests
 * compare it against the specification instead, with expected results
 * worked out by hand from the RISC-V and IEEE-754 rules. That matters for
 * the cases where an implementation could be consistently wrong in the
 * same way as its reference, and it reaches the encodings the fuzzer
 * cannot generate: reserved rounding modes must raise an illegal
 * instruction, which would end a reference process rather than produce a
 * comparable result. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rv32.h"
#include "machine.h"

/****************************************************************
 * Single-instruction execution
 ****************************************************************/

#define CODE_ADDR   0x1000

/* Fixed operand registers keep the encodings short */
#define RS1 1
#define RS2 2
#define RS3 3
#define RD  10

static rv_cpu cpu;
static machine mach;

static uint32_t
enc_fp(uint32_t f7, uint32_t rs2, uint32_t rm)
{
    return (f7 << 25) | (rs2 << 20) | (RS1 << 15) | (rm << 12) |
           (RD << 7) | 0x53;
}

static uint32_t
enc_fma(uint32_t opcode, uint32_t rm)
{
    return (RS3 << 27) | (RS2 << 20) | (RS1 << 15) | (rm << 12) |
           (RD << 7) | opcode;
}

/** Execute one encoding with the given operands and rounding mode. */
static void
run(uint32_t insn, uint32_t a, uint32_t b, uint32_t c, uint32_t frm)
{
    rv_reset(&cpu, CODE_ADDR);
    mach.mem[CODE_ADDR + 0] = (uint8_t)insn;
    mach.mem[CODE_ADDR + 1] = (uint8_t)(insn >> 8);
    mach.mem[CODE_ADDR + 2] = (uint8_t)(insn >> 16);
    mach.mem[CODE_ADDR + 3] = (uint8_t)(insn >> 24);

    cpu.f[RS1] = a;
    cpu.f[RS2] = b;
    cpu.f[RS3] = c;
    cpu.x[RS1] = a;             /* for the integer-to-float conversions */
    cpu.fcsr = (frm & 7) << 5;
    rv_step(&cpu);
}

/****************************************************************
 * Test reporting
 ****************************************************************/

static int tests, failures;

static const char *
flag_names(uint32_t f, char *buf, size_t n)
{
    snprintf(buf, n, "%s%s%s%s%s%s",
             f & RV_FFLAG_NV ? "NV " : "",
             f & RV_FFLAG_DZ ? "DZ " : "",
             f & RV_FFLAG_OF ? "OF " : "",
             f & RV_FFLAG_UF ? "UF " : "",
             f & RV_FFLAG_NX ? "NX " : "",
             f == 0 ? "-" : "");
    return buf;
}

static void
check(const char *name, uint32_t got, uint32_t want, uint32_t got_flags,
      uint32_t want_flags)
{
    char b1[32], b2[32];

    tests++;
    if (got == want && got_flags == want_flags) {
        printf("  ok   %-38s %08x\n", name, got);
        return;
    }
    failures++;
    printf("  FAIL %-38s got %08x [%s] want %08x [%s]\n", name, got,
           flag_names(got_flags, b1, sizeof(b1)), want,
           flag_names(want_flags, b2, sizeof(b2)));
}

/** Run an instruction producing a floating-point result and check it. */
static void
expect_f(const char *name, uint32_t insn, uint32_t a, uint32_t b, uint32_t c,
         uint32_t frm, uint32_t want, uint32_t want_flags)
{
    run(insn, a, b, c, frm);
    check(name, cpu.f[RD], want, cpu.fcsr & 0x1f, want_flags);
}

/** Run an instruction producing an integer result and check it. */
static void
expect_x(const char *name, uint32_t insn, uint32_t a, uint32_t b,
         uint32_t frm, uint32_t want, uint32_t want_flags)
{
    run(insn, a, b, 0, frm);
    check(name, cpu.x[RD], want, cpu.fcsr & 0x1f, want_flags);
}

/** An encoding that the specification reserves must raise an illegal
 * instruction exception rather than quietly doing something. */
static void
expect_illegal(const char *name, uint32_t insn, uint32_t frm)
{
    tests++;
    run(insn, 0x3f800000, 0x3f800000, 0, frm);
    if (cpu.in_trap && cpu.mcause == RV_CAUSE_ILLEGAL_INSN &&
        cpu.mtval == insn) {
        printf("  ok   %-38s illegal instruction\n", name);
        return;
    }
    failures++;
    printf("  FAIL %-38s expected illegal instruction, in_trap=%d "
           "mcause=%u\n", name, cpu.in_trap, cpu.mcause);
}

/****************************************************************
 * Operand constants
 ****************************************************************/

#define POS_ZERO    0x00000000u
#define NEG_ZERO    0x80000000u
#define ONE         0x3f800000u
#define NEG_ONE     0xbf800000u
#define TWO         0x40000000u
#define FOUR        0x40800000u
#define POS_INF     0x7f800000u
#define NEG_INF     0xff800000u
#define CANON_NAN   0x7fc00000u
#define QUIET_NAN   0x7fd12345u     /* quiet, with a payload */
#define SIG_NAN     0x7fa00000u     /* signalling */
#define MIN_SUB     0x00000001u     /* 2^-149 */
#define MAX_SUB     0x007fffffu     /* 2^-126 - 2^-149 */
#define MIN_NORM    0x00800000u     /* 2^-126 */
#define MAX_NORM    0x7f7fffffu     /* (2 - 2^-23) * 2^127 */
#define BELOW_2_125 0x00ffffffu     /* 2^-125 - 2^-149 */
#define TWO_M24     0x33800000u     /* 2^-24, half an ulp at 1.0 */
#define TWO_M25     0x33000000u     /* 2^-25, a quarter ulp at 1.0 */
#define ONE_PLUS    0x3f800001u     /* 1 + 2^-23 */
#define NEG_ONE_PLUS 0xbf800001u
#define TWO_M22     0x34800000u     /* 2^-22 */

/* funct7 values for the OP-FP encodings */
#define F7_ADD      0x00
#define F7_SUB      0x04
#define F7_MUL      0x08
#define F7_DIV      0x0c
#define F7_SQRT     0x2c
#define F7_SGNJ     0x10
#define F7_MINMAX   0x14
#define F7_CMP      0x50
#define F7_CVT_INT  0x60    /* fcvt.w.s / fcvt.wu.s */
#define F7_CVT_FLT  0x68    /* fcvt.s.w / fcvt.s.wu */
#define F7_MV_X     0x70    /* fmv.x.w / fclass.s */
#define F7_MV_F     0x78    /* fmv.w.x */

/****************************************************************
 * Tests
 ****************************************************************/

static void
test_rounding_modes(void)
{
    uint32_t add;

    printf("\nrounding modes: 1.0 + 2^-25, below the midpoint\n");
    add = enc_fp(F7_ADD, RS2, RV_RM_DYN);
    expect_f("rne", add, ONE, TWO_M25, 0, RV_RM_RNE, ONE, RV_FFLAG_NX);
    expect_f("rtz", add, ONE, TWO_M25, 0, RV_RM_RTZ, ONE, RV_FFLAG_NX);
    expect_f("rdn", add, ONE, TWO_M25, 0, RV_RM_RDN, ONE, RV_FFLAG_NX);
    expect_f("rup", add, ONE, TWO_M25, 0, RV_RM_RUP, ONE_PLUS, RV_FFLAG_NX);
    expect_f("rmm", add, ONE, TWO_M25, 0, RV_RM_RMM, ONE, RV_FFLAG_NX);

    printf("\nrounding modes: 1.0 + 2^-24, exactly the midpoint\n");
    expect_f("rne ties to even", add, ONE, TWO_M24, 0, RV_RM_RNE, ONE,
             RV_FFLAG_NX);
    expect_f("rtz", add, ONE, TWO_M24, 0, RV_RM_RTZ, ONE, RV_FFLAG_NX);
    expect_f("rdn", add, ONE, TWO_M24, 0, RV_RM_RDN, ONE, RV_FFLAG_NX);
    expect_f("rup", add, ONE, TWO_M24, 0, RV_RM_RUP, ONE_PLUS, RV_FFLAG_NX);
    expect_f("rmm ties away from zero", add, ONE, TWO_M24, 0, RV_RM_RMM,
             ONE_PLUS, RV_FFLAG_NX);

    printf("\nrounding modes: -1.0 - 2^-24, the sign-mirrored midpoint\n");
    expect_f("rne ties to even", add, NEG_ONE, TWO_M24 | 0x80000000u, 0,
             RV_RM_RNE, NEG_ONE, RV_FFLAG_NX);
    expect_f("rtz", add, NEG_ONE, TWO_M24 | 0x80000000u, 0, RV_RM_RTZ,
             NEG_ONE, RV_FFLAG_NX);
    expect_f("rdn", add, NEG_ONE, TWO_M24 | 0x80000000u, 0, RV_RM_RDN,
             NEG_ONE_PLUS, RV_FFLAG_NX);
    expect_f("rup", add, NEG_ONE, TWO_M24 | 0x80000000u, 0, RV_RM_RUP,
             NEG_ONE, RV_FFLAG_NX);
    expect_f("rmm ties away from zero", add, NEG_ONE, TWO_M24 | 0x80000000u,
             0, RV_RM_RMM, NEG_ONE_PLUS, RV_FFLAG_NX);

    printf("\nstatic rounding overrides the dynamic mode\n");
    expect_f("static rup with frm=rtz",
             enc_fp(F7_ADD, RS2, RV_RM_RUP), ONE, TWO_M25, 0, RV_RM_RTZ,
             ONE_PLUS, RV_FFLAG_NX);
}

static void
test_overflow_underflow(void)
{
    uint32_t add = enc_fp(F7_ADD, RS2, RV_RM_DYN);
    uint32_t mul = enc_fp(F7_MUL, RS2, RV_RM_DYN);
    uint32_t div = enc_fp(F7_DIV, RS2, RV_RM_DYN);

    printf("\noverflow delivers different results per rounding mode\n");
    expect_f("rne to infinity", add, MAX_NORM, MAX_NORM, 0, RV_RM_RNE,
             POS_INF, RV_FFLAG_OF | RV_FFLAG_NX);
    expect_f("rtz to the largest finite", add, MAX_NORM, MAX_NORM, 0,
             RV_RM_RTZ, MAX_NORM, RV_FFLAG_OF | RV_FFLAG_NX);
    expect_f("rdn to the largest finite", add, MAX_NORM, MAX_NORM, 0,
             RV_RM_RDN, MAX_NORM, RV_FFLAG_OF | RV_FFLAG_NX);
    expect_f("rup to infinity", add, MAX_NORM, MAX_NORM, 0, RV_RM_RUP,
             POS_INF, RV_FFLAG_OF | RV_FFLAG_NX);
    expect_f("negative rdn to infinity", add, MAX_NORM | 0x80000000u,
             MAX_NORM | 0x80000000u, 0, RV_RM_RDN, NEG_INF,
             RV_FFLAG_OF | RV_FFLAG_NX);
    expect_f("negative rup to largest finite", add, MAX_NORM | 0x80000000u,
             MAX_NORM | 0x80000000u, 0, RV_RM_RUP, MAX_NORM | 0x80000000u,
             RV_FFLAG_OF | RV_FFLAG_NX);

    printf("\nunderflow is only flagged when the result is also inexact\n");
    expect_f("exact subnormal result", mul, MIN_NORM, 0x3f000000, 0,
             RV_RM_RNE, 0x00400000, 0);
    expect_f("inexact tiny result ties to even", div, MIN_SUB, TWO, 0,
             RV_RM_RNE, POS_ZERO, RV_FFLAG_UF | RV_FFLAG_NX);
    expect_f("inexact tiny result rounds up", div, MIN_SUB, TWO, 0,
             RV_RM_RUP, MIN_SUB, RV_FFLAG_UF | RV_FFLAG_NX);

    printf("\nunderflow when the delivered result rounds up to normal\n");
    /* (2^-125 - 2^-149) / 2 is exactly halfway between the largest
     * subnormal and the smallest normal, so rounding to nearest delivers
     * the normal value. Underflow is still raised.
     *
     * "Tininess is detected after rounding", which the RISC-V spec
     * requires, is IEEE's phrase for rounding as though the exponent
     * range were unbounded and asking whether that result still lies
     * below 2^-126. It is not a question about the value delivered. Here
     * the exact result is 2^-126 - 2^-150, which needs only 24 bits and
     * so survives that rounding untouched, below the smallest normal and
     * therefore tiny. Only the delivered value climbs to 2^-126, because
     * the subnormal range has a bit less precision to round in.
     *
     * (This expected UF flag was absent until 2026-08-06, when a
     * differential run against qemu-riscv32 disagreed about an fnmadd.s;
     * qemu raises it here too, and so does Berkeley SoftFloat's
     * after-rounding mode, which is where this rule is normative.) */
    expect_f("rounds up into the normal range", div, BELOW_2_125, TWO, 0,
             RV_RM_RNE, MIN_NORM, RV_FFLAG_UF | RV_FFLAG_NX);
    expect_f("truncates to a subnormal", div, BELOW_2_125, TWO, 0,
             RV_RM_RTZ, MAX_SUB, RV_FFLAG_UF | RV_FFLAG_NX);
}

static void
test_nan_handling(void)
{
    uint32_t add = enc_fp(F7_ADD, RS2, RV_RM_DYN);
    uint32_t mul = enc_fp(F7_MUL, RS2, RV_RM_DYN);
    uint32_t sub = enc_fp(F7_SUB, RS2, RV_RM_DYN);

    printf("\nevery NaN result is the canonical quiet NaN\n");
    expect_f("quiet NaN payload is discarded", add, QUIET_NAN, ONE, 0,
             RV_RM_RNE, CANON_NAN, 0);
    expect_f("signalling NaN raises invalid", add, SIG_NAN, ONE, 0,
             RV_RM_RNE, CANON_NAN, RV_FFLAG_NV);
    expect_f("infinity minus infinity", sub, POS_INF, POS_INF, 0,
             RV_RM_RNE, CANON_NAN, RV_FFLAG_NV);
    expect_f("zero times infinity", mul, POS_ZERO, POS_INF, 0, RV_RM_RNE,
             CANON_NAN, RV_FFLAG_NV);

    printf("\nsign injection is a bit operation and never signals\n");
    expect_f("fsgnj.s of a canonical NaN", enc_fp(F7_SGNJ, RS2, 0),
             CANON_NAN, NEG_ZERO, 0, RV_RM_RNE, 0xffc00000, 0);
    expect_f("fsgnjn.s", enc_fp(F7_SGNJ, RS2, 1), ONE, NEG_ZERO, 0,
             RV_RM_RNE, ONE, 0);
    expect_f("fsgnjx.s", enc_fp(F7_SGNJ, RS2, 2), NEG_ONE, NEG_ZERO, 0,
             RV_RM_RNE, ONE, 0);
    expect_f("fsgnj.s of a signalling NaN", enc_fp(F7_SGNJ, RS2, 0),
             SIG_NAN, POS_ZERO, 0, RV_RM_RNE, SIG_NAN, 0);
}

static void
test_min_max(void)
{
    uint32_t fmin = enc_fp(F7_MINMAX, RS2, 0);
    uint32_t fmax = enc_fp(F7_MINMAX, RS2, 1);

    printf("\nfmin.s and fmax.s order the zeroes and ignore quiet NaNs\n");
    expect_f("fmin(-0, +0)", fmin, NEG_ZERO, POS_ZERO, 0, RV_RM_RNE,
             NEG_ZERO, 0);
    expect_f("fmax(-0, +0)", fmax, NEG_ZERO, POS_ZERO, 0, RV_RM_RNE,
             POS_ZERO, 0);
    expect_f("fmin(+0, -0)", fmin, POS_ZERO, NEG_ZERO, 0, RV_RM_RNE,
             NEG_ZERO, 0);
    expect_f("fmin(qNaN, 2.0)", fmin, QUIET_NAN, TWO, 0, RV_RM_RNE, TWO, 0);
    expect_f("fmax(2.0, qNaN)", fmax, TWO, QUIET_NAN, 0, RV_RM_RNE, TWO, 0);
    expect_f("fmin(sNaN, 2.0) signals", fmin, SIG_NAN, TWO, 0, RV_RM_RNE,
             TWO, RV_FFLAG_NV);
    expect_f("fmin(qNaN, qNaN)", fmin, QUIET_NAN, QUIET_NAN, 0, RV_RM_RNE,
             CANON_NAN, 0);
}

static void
test_divide_sqrt(void)
{
    uint32_t div = enc_fp(F7_DIV, RS2, RV_RM_DYN);
    uint32_t sqrt_insn = enc_fp(F7_SQRT, 0, RV_RM_DYN);

    printf("\ndivision by zero is distinct from an invalid operation\n");
    expect_f("1.0 / +0", div, ONE, POS_ZERO, 0, RV_RM_RNE, POS_INF,
             RV_FFLAG_DZ);
    expect_f("-1.0 / +0", div, NEG_ONE, POS_ZERO, 0, RV_RM_RNE, NEG_INF,
             RV_FFLAG_DZ);
    expect_f("1.0 / -0", div, ONE, NEG_ZERO, 0, RV_RM_RNE, NEG_INF,
             RV_FFLAG_DZ);
    expect_f("0 / 0", div, POS_ZERO, POS_ZERO, 0, RV_RM_RNE, CANON_NAN,
             RV_FFLAG_NV);
    expect_f("inf / inf", div, POS_INF, POS_INF, 0, RV_RM_RNE, CANON_NAN,
             RV_FFLAG_NV);
    expect_f("1.0 / 3.0 is inexact", div, ONE, 0x40400000, 0, RV_RM_RNE,
             0x3eaaaaab, RV_FFLAG_NX);

    printf("\nsquare root\n");
    expect_f("sqrt(4.0)", sqrt_insn, FOUR, 0, 0, RV_RM_RNE, TWO, 0);
    expect_f("sqrt(-0) keeps the sign", sqrt_insn, NEG_ZERO, 0, 0,
             RV_RM_RNE, NEG_ZERO, 0);
    expect_f("sqrt(-1.0) is invalid", sqrt_insn, NEG_ONE, 0, 0, RV_RM_RNE,
             CANON_NAN, RV_FFLAG_NV);
    expect_f("sqrt(+inf)", sqrt_insn, POS_INF, 0, 0, RV_RM_RNE, POS_INF, 0);
    expect_f("sqrt(2.0) is inexact", sqrt_insn, TWO, 0, 0, RV_RM_RNE,
             0x3fb504f3, RV_FFLAG_NX);
}

static void
test_fused_multiply_add(void)
{
    printf("\nthe fused forms round once, not twice\n");
    /* (1+2^-23)^2 - 1 is 2^-22 + 2^-46 exactly. Rounding the product
     * first would give 2^-22 with no loss; the fused form has to report
     * the result as inexact. */
    expect_f("fmadd.s rounds the exact product", enc_fma(0x43, RV_RM_DYN),
             ONE_PLUS, ONE_PLUS, NEG_ONE, RV_RM_RNE, TWO_M22, RV_FFLAG_NX);
    expect_f("fmsub.s", enc_fma(0x47, RV_RM_DYN), ONE_PLUS, ONE_PLUS, ONE,
             RV_RM_RNE, TWO_M22, RV_FFLAG_NX);
    expect_f("fnmsub.s negates the product", enc_fma(0x4b, RV_RM_DYN),
             ONE_PLUS, ONE_PLUS, ONE, RV_RM_RNE, TWO_M22 | 0x80000000u,
             RV_FFLAG_NX);
    expect_f("fnmadd.s negates both", enc_fma(0x4f, RV_RM_DYN), ONE_PLUS,
             ONE_PLUS, NEG_ONE, RV_RM_RNE, TWO_M22 | 0x80000000u,
             RV_FFLAG_NX);

    printf("\nfused special cases\n");
    expect_f("0 * inf + qNaN is still invalid", enc_fma(0x43, RV_RM_DYN),
             POS_ZERO, POS_INF, QUIET_NAN, RV_RM_RNE, CANON_NAN,
             RV_FFLAG_NV);
    expect_f("inf * 1 - inf", enc_fma(0x47, RV_RM_DYN), POS_INF, ONE,
             POS_INF, RV_RM_RNE, CANON_NAN, RV_FFLAG_NV);
    expect_f("2 * 3 + 1", enc_fma(0x43, RV_RM_DYN), TWO, 0x40400000, ONE,
             RV_RM_RNE, 0x40e00000, 0);
}

static void
test_conversions(void)
{
    uint32_t to_w = enc_fp(F7_CVT_INT, 0, RV_RM_DYN);
    uint32_t to_wu = enc_fp(F7_CVT_INT, 1, RV_RM_DYN);
    uint32_t from_w = enc_fp(F7_CVT_FLT, 0, RV_RM_DYN);
    uint32_t from_wu = enc_fp(F7_CVT_FLT, 1, RV_RM_DYN);

    printf("\nfloat to integer saturates instead of wrapping\n");
    expect_x("fcvt.w.s(NaN)", to_w, CANON_NAN, 0, RV_RM_RTZ, 0x7fffffff,
             RV_FFLAG_NV);
    expect_x("fcvt.wu.s(NaN)", to_wu, CANON_NAN, 0, RV_RM_RTZ, 0xffffffff,
             RV_FFLAG_NV);
    expect_x("fcvt.w.s(+inf)", to_w, POS_INF, 0, RV_RM_RTZ, 0x7fffffff,
             RV_FFLAG_NV);
    expect_x("fcvt.w.s(-inf)", to_w, NEG_INF, 0, RV_RM_RTZ, 0x80000000,
             RV_FFLAG_NV);
    expect_x("fcvt.w.s(2^31)", to_w, 0x4f000000, 0, RV_RM_RTZ, 0x7fffffff,
             RV_FFLAG_NV);
    expect_x("fcvt.w.s(-2^31) is exact", to_w, 0xcf000000, 0, RV_RM_RTZ,
             0x80000000, 0);
    expect_x("fcvt.wu.s(-1.0) is invalid", to_wu, NEG_ONE, 0, RV_RM_RTZ, 0,
             RV_FFLAG_NV);
    expect_x("fcvt.wu.s(-0.5) is merely inexact", to_wu, 0xbf000000, 0,
             RV_RM_RTZ, 0, RV_FFLAG_NX);
    expect_x("fcvt.w.s(-2.5) rtz", to_w, 0xc0200000, 0, RV_RM_RTZ,
             (uint32_t)-2, RV_FFLAG_NX);
    expect_x("fcvt.w.s(-2.5) rdn", to_w, 0xc0200000, 0, RV_RM_RDN,
             (uint32_t)-3, RV_FFLAG_NX);
    expect_x("fcvt.w.s(-2.5) rne", to_w, 0xc0200000, 0, RV_RM_RNE,
             (uint32_t)-2, RV_FFLAG_NX);
    expect_x("fcvt.w.s(-2.5) rmm", to_w, 0xc0200000, 0, RV_RM_RMM,
             (uint32_t)-3, RV_FFLAG_NX);
    expect_x("fcvt.w.s(2.5) rne ties to even", to_w, 0x40200000, 0,
             RV_RM_RNE, 2, RV_FFLAG_NX);
    expect_x("fcvt.w.s(3.5) rne ties to even", to_w, 0x40600000, 0,
             RV_RM_RNE, 4, RV_FFLAG_NX);

    printf("\ninteger to float rounds when the value does not fit\n");
    expect_f("fcvt.s.w(-2^31) is exact", from_w, 0x80000000, 0, 0,
             RV_RM_RNE, 0xcf000000, 0);
    expect_f("fcvt.s.w(2^24+1) rne", from_w, 0x01000001, 0, 0, RV_RM_RNE,
             0x4b800000, RV_FFLAG_NX);
    expect_f("fcvt.s.w(2^24+1) rup", from_w, 0x01000001, 0, 0, RV_RM_RUP,
             0x4b800001, RV_FFLAG_NX);
    expect_f("fcvt.s.wu(0xffffffff)", from_wu, 0xffffffff, 0, 0, RV_RM_RNE,
             0x4f800000, RV_FFLAG_NX);
    expect_f("fcvt.s.w(-1)", from_w, 0xffffffff, 0, 0, RV_RM_RNE, NEG_ONE,
             0);

    printf("\nbit-level moves do not interpret the value\n");
    expect_x("fmv.x.w of a signalling NaN", enc_fp(F7_MV_X, 0, 0), SIG_NAN,
             0, RV_RM_RNE, SIG_NAN, 0);
    expect_f("fmv.w.x", enc_fp(F7_MV_F, 0, 0), SIG_NAN, 0, 0, RV_RM_RNE,
             SIG_NAN, 0);
}

static void
test_compare_classify(void)
{
    uint32_t fle = enc_fp(F7_CMP, RS2, 0);
    uint32_t flt = enc_fp(F7_CMP, RS2, 1);
    uint32_t feq = enc_fp(F7_CMP, RS2, 2);
    uint32_t fclass = enc_fp(F7_MV_X, 0, 1);

    printf("\ncomparisons: quiet for feq.s, signalling for flt.s "
           "and fle.s\n");
    expect_x("feq(-0, +0)", feq, NEG_ZERO, POS_ZERO, RV_RM_RNE, 1, 0);
    expect_x("feq(qNaN, 1.0) is quiet", feq, QUIET_NAN, ONE, RV_RM_RNE, 0,
             0);
    expect_x("feq(sNaN, 1.0) signals", feq, SIG_NAN, ONE, RV_RM_RNE, 0,
             RV_FFLAG_NV);
    expect_x("flt(qNaN, 1.0) signals", flt, QUIET_NAN, ONE, RV_RM_RNE, 0,
             RV_FFLAG_NV);
    expect_x("fle(qNaN, 1.0) signals", fle, QUIET_NAN, ONE, RV_RM_RNE, 0,
             RV_FFLAG_NV);
    expect_x("flt(-1.0, 1.0)", flt, NEG_ONE, ONE, RV_RM_RNE, 1, 0);
    expect_x("fle(1.0, 1.0)", fle, ONE, ONE, RV_RM_RNE, 1, 0);

    printf("\nfclass.s covers all ten classes\n");
    expect_x("negative infinity", fclass, NEG_INF, 0, RV_RM_RNE, 1u << 0, 0);
    expect_x("negative normal", fclass, NEG_ONE, 0, RV_RM_RNE, 1u << 1, 0);
    expect_x("negative subnormal", fclass, MAX_SUB | 0x80000000u, 0,
             RV_RM_RNE, 1u << 2, 0);
    expect_x("negative zero", fclass, NEG_ZERO, 0, RV_RM_RNE, 1u << 3, 0);
    expect_x("positive zero", fclass, POS_ZERO, 0, RV_RM_RNE, 1u << 4, 0);
    expect_x("positive subnormal", fclass, MIN_SUB, 0, RV_RM_RNE, 1u << 5,
             0);
    expect_x("positive normal", fclass, ONE, 0, RV_RM_RNE, 1u << 6, 0);
    expect_x("positive infinity", fclass, POS_INF, 0, RV_RM_RNE, 1u << 7, 0);
    expect_x("signalling NaN", fclass, SIG_NAN, 0, RV_RM_RNE, 1u << 8, 0);
    expect_x("quiet NaN", fclass, QUIET_NAN, 0, RV_RM_RNE, 1u << 9, 0);
}

static void
test_reserved_encodings(void)
{
    printf("\nreserved rounding modes must raise illegal instruction\n");
    expect_illegal("static rm 5", enc_fp(F7_ADD, RS2, 5), RV_RM_RNE);
    expect_illegal("static rm 6", enc_fp(F7_ADD, RS2, 6), RV_RM_RNE);
    expect_illegal("dynamic rm with frm 5", enc_fp(F7_ADD, RS2, RV_RM_DYN),
                   5);
    expect_illegal("dynamic rm with frm 6", enc_fp(F7_ADD, RS2, RV_RM_DYN),
                   6);
    expect_illegal("dynamic rm with frm 7", enc_fp(F7_ADD, RS2, RV_RM_DYN),
                   7);
    expect_illegal("fmadd.s with rm 5", enc_fma(0x43, 5), RV_RM_RNE);

    printf("\nother reserved fields\n");
    expect_illegal("fsqrt.s with rs2 not zero", enc_fp(F7_SQRT, 1,
                   RV_RM_RNE), RV_RM_RNE);
    expect_illegal("double-precision format", enc_fp(F7_ADD | 1, RS2,
                   RV_RM_RNE), RV_RM_RNE);
    expect_illegal("fclass.s with rs2 not zero", enc_fp(F7_MV_X, 1, 1),
                   RV_RM_RNE);
}

static void
test_flag_accrual(void)
{
    uint32_t add = enc_fp(F7_ADD, RS2, RV_RM_DYN);

    printf("\nexception flags accrue and are never cleared by hardware\n");
    tests++;
    rv_reset(&cpu, CODE_ADDR);
    mach.mem[CODE_ADDR + 0] = (uint8_t)add;
    mach.mem[CODE_ADDR + 1] = (uint8_t)(add >> 8);
    mach.mem[CODE_ADDR + 2] = (uint8_t)(add >> 16);
    mach.mem[CODE_ADDR + 3] = (uint8_t)(add >> 24);
    cpu.f[RS1] = ONE;
    cpu.f[RS2] = TWO;
    cpu.fcsr = RV_FFLAG_NV | RV_FFLAG_DZ;   /* already set by earlier work */
    rv_step(&cpu);
    if ((cpu.fcsr & 0x1f) == (RV_FFLAG_NV | RV_FFLAG_DZ)) {
        printf("  ok   %-38s %02x\n", "an exact result preserves flags",
               cpu.fcsr & 0x1f);
    } else {
        failures++;
        printf("  FAIL %-38s got %02x want %02x\n",
               "an exact result preserves flags", cpu.fcsr & 0x1f,
               RV_FFLAG_NV | RV_FFLAG_DZ);
    }
}

int
main(void)
{
    if (machine_init(&mach, &cpu))
        return 1;

    printf("IEEE-754 conformance tests for RV32F\n");

    test_rounding_modes();
    test_overflow_underflow();
    test_nan_handling();
    test_min_max();
    test_divide_sqrt();
    test_fused_multiply_add();
    test_conversions();
    test_compare_classify();
    test_reserved_encodings();
    test_flag_accrual();

    printf("\n%d tests, %d failures\n", tests, failures);
    machine_free(&mach);
    return failures ? 1 : 0;
}
