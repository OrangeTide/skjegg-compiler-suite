/* f32_vectors.h : the cross-backend single-precision differential table.
 *
 * This is the *input* side of the f32 determinism check: a frozen list of
 * (operation, operand-a, operand-b) triples, chosen to hit the cases the
 * existing test_f32.c deliberately avoids (it uses only exact integer
 * values, which every IEEE-ish FPU agrees on).  The *expected* side lives
 * in f32_golden.inc, generated from the RV32 emulator's f32 core by
 * f32_oracle.c and committed as a golden master.
 *
 * Operands are raw 32-bit patterns.  For a float input that is operand-a
 * (or -b) reinterpreted as an IEEE binary32.  For F2I operand-a is a
 * binary32 pattern and the result is an int.  For I2F operand-a is a
 * signed int32 and the result is a binary32 pattern.  Operand-b is unused
 * by the unary ops (NEG, I2F, F2I).
 *
 * The op set is exactly what the F-only RV32 core supports and what a
 * pure-f32 Excelsior would emit: add/sub/mul/div, neg, the three ordered
 * compares, and the two int<->single conversions.  There is no f32<->f64
 * here: a deterministic f32 Excelsior has no double, and the F-only core
 * cannot serve as its oracle anyway.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#ifndef F32_VECTORS_H
#define F32_VECTORS_H

enum f32_op {
    OP_ADD,     /* binary, float result */
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_NEG,     /* unary  (a), float result */
    OP_EQ,      /* binary, int result (0/1) */
    OP_LT,
    OP_LE,
    OP_I2F,     /* unary  (a = signed int32), float result */
    OP_F2I      /* unary  (a = binary32),     int result   */
};

/* Whether an op's result is compared as raw float bits or as an integer. */
#define F32_RESULT_IS_INT(op) \
    ((op) == OP_EQ || (op) == OP_LT || (op) == OP_LE || (op) == OP_F2I)

struct f32_vec {
    unsigned char op;
    unsigned int  a;
    unsigned int  b;
    const char   *note;
};

/* Handy binary32 bit patterns. */
#define F_POS_ZERO   0x00000000u
#define F_NEG_ZERO   0x80000000u
#define F_ONE        0x3f800000u    /*  1.0f */
#define F_NEG_ONE    0xbf800000u    /* -1.0f */
#define F_TWO        0x40000000u    /*  2.0f */
#define F_HALF       0x3f000000u    /*  0.5f */
#define F_TENTH      0x3dcccccdu    /*  0.1f (nearest single) */
#define F_FIFTH      0x3e4ccccdu    /*  0.2f */
#define F_THREE      0x40400000u    /*  3.0f */
#define F_POS_INF    0x7f800000u
#define F_NEG_INF    0xff800000u
#define F_QNAN       0x7fc00000u    /* canonical quiet NaN */
#define F_SNAN       0x7f800001u    /* a signaling NaN */
#define F_MIN_NORMAL 0x00800000u    /* smallest positive normal, 2^-126 */
#define F_MAX_NORMAL 0x7f7fffffu    /* largest finite */
#define F_MIN_SUBNRM 0x00000001u    /* smallest positive subnormal, 2^-149 */
#define F_TWO24P1    0x4b800001u    /* 16777217.0f? no: see I2F vectors */

static const struct f32_vec f32_vectors[] = {
    /* --- A. Inexact arithmetic (rounding-mode sensitive) --- */
    { OP_ADD, F_TENTH,     F_FIFTH,    "0.1 + 0.2" },
    { OP_DIV, F_ONE,       F_THREE,    "1.0 / 3.0" },
    { OP_DIV, F_TWO,       F_THREE,    "2.0 / 3.0" },
    { OP_MUL, F_TENTH,     F_TENTH,    "0.1 * 0.1" },
    { OP_DIV, F_ONE,       0x41200000u, "1.0 / 10.0" },
    { OP_ADD, 0x3f800001u, 0x33800000u, "1+ulp + tiny (round-to-even probe)" },
    { OP_MUL, 0x3fb504f3u, 0x3fb504f3u, "sqrt(2)~ squared" },
    { OP_SUB, F_ONE,       F_TENTH,    "1.0 - 0.1" },

    /* --- B. Signed zero (bit-pattern sensitive) --- */
    { OP_ADD, F_POS_ZERO,  F_POS_ZERO, "+0 + +0" },
    { OP_ADD, F_NEG_ZERO,  F_NEG_ZERO, "-0 + -0" },
    { OP_ADD, F_POS_ZERO,  F_NEG_ZERO, "+0 + -0 (RNE => +0)" },
    { OP_ADD, F_NEG_ZERO,  F_POS_ZERO, "-0 + +0" },
    { OP_SUB, F_POS_ZERO,  F_POS_ZERO, "+0 - +0" },
    { OP_MUL, F_NEG_ZERO,  F_ONE,      "-0 * 1" },
    { OP_MUL, F_POS_ZERO,  F_NEG_ONE,  "+0 * -1" },
    { OP_NEG, F_POS_ZERO,  0,          "neg(+0)" },
    { OP_NEG, F_NEG_ZERO,  0,          "neg(-0)" },

    /* --- C. Subnormals and underflow (the flush-to-zero trap) --- */
    { OP_DIV, F_MIN_NORMAL, F_TWO,     "min_normal / 2 => subnormal" },
    { OP_ADD, F_MIN_SUBNRM, F_MIN_SUBNRM, "subnormal + subnormal" },
    { OP_MUL, F_MIN_NORMAL, F_HALF,    "min_normal * 0.5 => subnormal" },
    { OP_SUB, F_MIN_NORMAL, F_MIN_SUBNRM, "normal - subnormal" },
    { OP_MUL, F_MIN_SUBNRM, F_HALF,    "subnormal * 0.5 => underflow to 0" },
    { OP_ADD, F_MIN_SUBNRM, F_MIN_NORMAL, "subnormal + min_normal" },

    /* --- D. NaN and infinity (bit-pattern sensitive) --- */
    { OP_DIV, F_POS_ZERO,  F_POS_ZERO, "0/0 => NaN" },
    { OP_SUB, F_POS_INF,   F_POS_INF,  "inf - inf => NaN" },
    { OP_MUL, F_POS_INF,   F_POS_ZERO, "inf * 0 => NaN" },
    { OP_ADD, F_MAX_NORMAL, F_MAX_NORMAL, "max + max => +inf" },
    { OP_MUL, F_MAX_NORMAL, F_TWO,     "max * 2 => +inf" },
    { OP_DIV, F_ONE,       F_POS_ZERO, "1/0 => +inf" },
    { OP_DIV, F_NEG_ONE,   F_POS_ZERO, "-1/0 => -inf" },
    { OP_ADD, F_QNAN,      F_ONE,      "qNaN + 1 (payload/quieting)" },
    { OP_ADD, F_SNAN,      F_ONE,      "sNaN + 1 (quieting)" },
    { OP_MUL, F_SNAN,      F_TWO,      "sNaN * 2" },
    { OP_NEG, F_QNAN,      0,          "neg(qNaN) (sign only)" },
    { OP_NEG, F_POS_INF,   0,          "neg(+inf)" },

    /* --- D'. NaN/inf in the compares (all NaN compares are false) --- */
    { OP_EQ,  F_QNAN,      F_QNAN,     "NaN == NaN => 0" },
    { OP_LT,  F_QNAN,      F_ONE,      "NaN < 1 => 0" },
    { OP_LE,  F_ONE,       F_QNAN,     "1 <= NaN => 0" },
    { OP_EQ,  F_POS_ZERO,  F_NEG_ZERO, "+0 == -0 => 1" },
    { OP_LT,  F_NEG_INF,   F_POS_INF,  "-inf < +inf => 1" },
    { OP_EQ,  F_POS_INF,   F_POS_INF,  "+inf == +inf => 1" },
    { OP_LT,  F_TENTH,     F_FIFTH,    "0.1 < 0.2 => 1" },
    { OP_LE,  F_THREE,     F_THREE,    "3 <= 3 => 1" },

    /* --- E. Conversions: the known semantic gaps --- */
    { OP_I2F, 16777217u,   0,          "i2f(2^24+1) rounds to even" },
    { OP_I2F, 16777219u,   0,          "i2f(2^24+3) rounds" },
    { OP_I2F, 2147483647u, 0,          "i2f(INT_MAX) rounds up" },
    { OP_I2F, 0xffffffffu, 0,          "i2f(-1)" },
    { OP_I2F, 0x80000000u, 0,          "i2f(INT_MIN)" },
    { OP_F2I, 0x40333333u, 0,          "f2i(2.8) truncates to 2" },
    { OP_F2I, 0xc0333333u, 0,          "f2i(-2.8) truncates to -2" },
    { OP_F2I, F_QNAN,      0,          "f2i(NaN): ISA-defined, RV saturates" },
    { OP_F2I, F_POS_INF,   0,          "f2i(+inf): saturates" },
    { OP_F2I, F_NEG_INF,   0,          "f2i(-inf): saturates" },
    { OP_F2I, 0x50000000u, 0,          "f2i(2^33): out of range, saturates" },
    { OP_F2I, F_MIN_SUBNRM, 0,         "f2i(smallest subnormal) => 0" },

    /* --- F. Identity / injection-integrity (x*1 and x+0 must be exact) --- */
    { OP_MUL, F_TENTH,     F_ONE,      "0.1 * 1 == 0.1" },
    { OP_ADD, F_MIN_SUBNRM, F_POS_ZERO, "subnormal + 0 (FTZ probe)" },
    { OP_MUL, F_MAX_NORMAL, F_ONE,     "max * 1 == max" }
};

#define F32_NVEC ((int)(sizeof(f32_vectors) / sizeof(f32_vectors[0])))

#endif /* F32_VECTORS_H */
