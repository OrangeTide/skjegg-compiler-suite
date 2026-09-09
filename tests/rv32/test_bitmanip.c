/* test_bitmanip.c : Zba, Zbb and Zbs directed tests */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* These cases go after the boundaries that a table-driven implementation
 * gets wrong: a shift amount of zero, where a rotate has no bits to move
 * across; a shift amount of 31, where a bit index reaches the sign bit; a
 * count of leading or trailing zeros in a word that has none; and the
 * signed comparisons at the extremes of the range. The expected values are
 * written as constants rather than computed, so a test agrees with the
 * emulator only when both agree with the specification. */

#include <stdio.h>
#include <stdlib.h>
#include "rv32.h"
#include "machine.h"

#define CODE_ADDR   0x1000

static rv_cpu cpu;
static machine mach;
static int tests, failures;

/****************************************************************
 * Encoding
 ****************************************************************/

/* Every case computes x3 = f(x1, x2) or x3 = f(x1, immediate). */
#define RD_N    3
#define RS1_N   1
#define RS2_N   2

/* Macros rather than functions because the case tables are static */
#define ENC_R(f7, rs2, f3) \
    (((uint32_t)(f7) << 25) | ((uint32_t)(rs2) << 20) | \
     ((uint32_t)RS1_N << 15) | ((uint32_t)(f3) << 12) | \
     ((uint32_t)RD_N << 7) | 0x33u)

#define ENC_I(f7, shamt, f3) \
    (((uint32_t)(f7) << 25) | ((uint32_t)(shamt) << 20) | \
     ((uint32_t)RS1_N << 15) | ((uint32_t)(f3) << 12) | \
     ((uint32_t)RD_N << 7) | 0x13u)

/* Register-register form, and register-immediate form with the shift
 * amount folded into the encoding. */
#define R(f7, f3)           ENC_R((f7), RS2_N, (f3))
#define I(f7, sh, f3)       ENC_I((f7), (sh), (f3))

/* The unary Zbb instructions reuse the shift-amount field as a selector */
#define UNARY(sel)          ENC_I(0x30, (sel), 1)

struct testcase {
    const char *name;
    uint32_t insn;
    uint32_t a;         /* x1 */
    uint32_t b;         /* x2, ignored by the immediate forms */
    uint32_t want;      /* expected x3 */
};

/****************************************************************
 * Execution
 ****************************************************************/

static void
run(const struct testcase *t)
{
    uint32_t got;

    rv_reset(&cpu, CODE_ADDR);
    mach.mem[CODE_ADDR + 0] = (uint8_t)t->insn;
    mach.mem[CODE_ADDR + 1] = (uint8_t)(t->insn >> 8);
    mach.mem[CODE_ADDR + 2] = (uint8_t)(t->insn >> 16);
    mach.mem[CODE_ADDR + 3] = (uint8_t)(t->insn >> 24);
    cpu.x[RS1_N] = t->a;
    cpu.x[RS2_N] = t->b;
    cpu.x[RD_N] = 0xdeadbeef;
    rv_step(&cpu);

    got = cpu.x[RD_N];
    tests++;
    if (cpu.in_trap) {
        failures++;
        printf("  FAIL %-34s trapped, want %08x\n", t->name, t->want);
    } else if (got == t->want) {
        printf("  ok   %-34s %08x\n", t->name, got);
    } else {
        failures++;
        printf("  FAIL %-34s got %08x want %08x\n", t->name, got, t->want);
    }
}

static void
run_all(const char *title, const struct testcase *tc, size_t n)
{
    size_t i;

    printf("\n%s\n", title);
    for (i = 0; i < n; i++)
        run(&tc[i]);
}

/****************************************************************
 * Zba: shifted add
 ****************************************************************/

static const struct testcase zba[] = {
    { "sh1add",             R(0x10, 2), 0x00000003, 0x00000010, 0x00000016 },
    { "sh2add",             R(0x10, 4), 0x00000003, 0x00000010, 0x0000001c },
    { "sh3add",             R(0x10, 6), 0x00000003, 0x00000010, 0x00000028 },
    /* The shift is modulo 2^32; the discarded bits do not come back */
    { "sh3add wraps",       R(0x10, 6), 0x20000001, 0x00000005, 0x0000000d },
    { "sh1add zero base",   R(0x10, 2), 0x7fffffff, 0x00000000, 0xfffffffe },
};

/****************************************************************
 * Zbb: logic with a complemented operand
 ****************************************************************/

static const struct testcase zbb_logic[] = {
    { "andn",               R(0x20, 7), 0xff00ff00, 0x0f0f0f0f, 0xf000f000 },
    { "orn",                R(0x20, 6), 0xff00ff00, 0x0f0f0f0f, 0xfff0fff0 },
    { "xnor",               R(0x20, 4), 0xff00ff00, 0x0f0f0f0f, 0x0ff00ff0 },
    { "andn all ones",      R(0x20, 7), 0xffffffff, 0xffffffff, 0x00000000 },
    { "orn all zeros",      R(0x20, 6), 0x00000000, 0x00000000, 0xffffffff },
};

/****************************************************************
 * Zbb: counting and sign handling
 ****************************************************************/

static const struct testcase zbb_count[] = {
    { "clz",                UNARY(0), 0x00ff0000, 0, 8 },
    { "clz of zero is 32",  UNARY(0), 0x00000000, 0, 32 },
    { "clz of sign bit",    UNARY(0), 0x80000000, 0, 0 },
    { "ctz",                UNARY(1), 0x00ff0000, 0, 16 },
    { "ctz of zero is 32",  UNARY(1), 0x00000000, 0, 32 },
    { "ctz of odd is 0",    UNARY(1), 0xffffffff, 0, 0 },
    { "cpop",               UNARY(2), 0x00ff0f00, 0, 12 },
    { "cpop of zero",       UNARY(2), 0x00000000, 0, 0 },
    { "cpop of all ones",   UNARY(2), 0xffffffff, 0, 32 },
    { "sext.b positive",    UNARY(4), 0x00000042, 0, 0x00000042 },
    { "sext.b negative",    UNARY(4), 0xffffff80, 0, 0xffffff80 },
    { "sext.b ignores high",UNARY(4), 0x12345680, 0, 0xffffff80 },
    { "sext.h positive",    UNARY(5), 0x00007fff, 0, 0x00007fff },
    { "sext.h negative",    UNARY(5), 0x00008000, 0, 0xffff8000 },
    { "zext.h",             ENC_R(0x04, 0, 4), 0x12348765, 0, 0x00008765 },
};

/****************************************************************
 * Zbb: minimum and maximum
 ****************************************************************/

static const struct testcase zbb_minmax[] = {
    { "min signed",         R(0x05, 4), 0xffffffff, 0x00000001, 0xffffffff },
    { "max signed",         R(0x05, 6), 0xffffffff, 0x00000001, 0x00000001 },
    { "minu unsigned",      R(0x05, 5), 0xffffffff, 0x00000001, 0x00000001 },
    { "maxu unsigned",      R(0x05, 7), 0xffffffff, 0x00000001, 0xffffffff },
    { "min at INT_MIN",     R(0x05, 4), 0x80000000, 0x7fffffff, 0x80000000 },
    { "max at INT_MIN",     R(0x05, 6), 0x80000000, 0x7fffffff, 0x7fffffff },
    { "min of equals",      R(0x05, 4), 0x00001234, 0x00001234, 0x00001234 },
};

/****************************************************************
 * Zbb: rotates and byte-wise operations
 ****************************************************************/

static const struct testcase zbb_rotate[] = {
    { "rol by 4",           R(0x30, 1), 0x12345678, 4, 0x23456781 },
    { "ror by 4",           R(0x30, 5), 0x12345678, 4, 0x81234567 },
    /* A rotate by zero must not shift by 32, which is undefined in C */
    { "rol by 0",           R(0x30, 1), 0x12345678, 0, 0x12345678 },
    { "ror by 0",           R(0x30, 5), 0x12345678, 0, 0x12345678 },
    /* Only the low five bits of the register count */
    { "rol masks to 5 bits",R(0x30, 1), 0x12345678, 36, 0x23456781 },
    { "rol by 31",          R(0x30, 1), 0x00000001, 31, 0x80000000 },
    { "rori by 8",          I(0x30, 8, 5), 0x12345678, 0, 0x78123456 },
    { "rori by 0",          I(0x30, 0, 5), 0x12345678, 0, 0x12345678 },
    { "rev8",               I(0x34, 0x18, 5), 0x12345678, 0, 0x78563412 },
    { "orc.b",              I(0x14, 0x07, 5), 0x00010200, 0, 0x00ffff00 },
    { "orc.b of zero",      I(0x14, 0x07, 5), 0x00000000, 0, 0x00000000 },
    { "orc.b of high byte", I(0x14, 0x07, 5), 0x80000000, 0, 0xff000000 },
};

/****************************************************************
 * Zbs: single-bit operations
 ****************************************************************/

static const struct testcase zbs[] = {
    { "bset",               R(0x14, 1), 0x00000000, 5, 0x00000020 },
    { "bclr",               R(0x24, 1), 0xffffffff, 5, 0xffffffdf },
    { "binv sets",          R(0x34, 1), 0x00000000, 5, 0x00000020 },
    { "binv clears",        R(0x34, 1), 0x00000020, 5, 0x00000000 },
    { "bext of a set bit",  R(0x24, 5), 0x00000020, 5, 0x00000001 },
    { "bext of a clear bit",R(0x24, 5), 0x00000000, 5, 0x00000000 },
    /* Bit index 31 is the sign bit, and bit 32 wraps back to bit 0 */
    { "bset bit 31",        R(0x14, 1), 0x00000000, 31, 0x80000000 },
    { "bext bit 31",        R(0x24, 5), 0x80000000, 31, 0x00000001 },
    { "bset index wraps",   R(0x14, 1), 0x00000000, 32, 0x00000001 },
    { "bseti",              I(0x14, 5, 1), 0x00000000, 0, 0x00000020 },
    { "bclri",              I(0x24, 5, 1), 0xffffffff, 0, 0xffffffdf },
    { "binvi",              I(0x34, 5, 1), 0x00000020, 0, 0x00000000 },
    { "bexti",              I(0x24, 5, 5), 0x00000020, 0, 0x00000001 },
    { "bseti bit 31",       I(0x14, 31, 1), 0x00000000, 0, 0x80000000 },
    { "bexti bit 31",       I(0x24, 31, 5), 0x80000000, 0, 0x00000001 },
};

/****************************************************************
 * The switch
 ****************************************************************/

/* With the extensions disabled every one of these encodings has to raise an
 * illegal-instruction trap, because the base integer set leaves all of them
 * unassigned. This is what lets a host offer a plain RV32IMAFC machine. */
static void
test_switch_off(void)
{
    static const uint32_t insns[] = {
        R(0x10, 2), R(0x20, 7), R(0x05, 4), R(0x30, 1), R(0x14, 1),
        R(0x24, 5), R(0x34, 1), ENC_R(0x04, 0, 4),
        UNARY(0), UNARY(2), I(0x30, 8, 5), I(0x34, 0x18, 5),
        I(0x14, 0x07, 5), I(0x14, 5, 1), I(0x24, 5, 5),
    };
    size_t i;

    printf("\nevery encoding traps when the extensions are off\n");
    cpu.bitmanip = 0;
    for (i = 0; i < sizeof(insns) / sizeof(insns[0]); i++) {
        struct testcase t = { "", insns[i], 0x12345678, 4, 0 };

        rv_reset(&cpu, CODE_ADDR);
        mach.mem[CODE_ADDR + 0] = (uint8_t)t.insn;
        mach.mem[CODE_ADDR + 1] = (uint8_t)(t.insn >> 8);
        mach.mem[CODE_ADDR + 2] = (uint8_t)(t.insn >> 16);
        mach.mem[CODE_ADDR + 3] = (uint8_t)(t.insn >> 24);
        cpu.x[RS1_N] = t.a;
        cpu.x[RS2_N] = t.b;
        rv_step(&cpu);

        tests++;
        if (cpu.mcause == RV_CAUSE_ILLEGAL_INSN && cpu.in_trap) {
            printf("  ok   %08x traps\n", t.insn);
        } else {
            failures++;
            printf("  FAIL %08x did not trap (mcause %u)\n",
                   t.insn, cpu.mcause);
        }
    }
    cpu.bitmanip = 1;
}

/* Nothing here may disturb the base instructions that share the same funct3
 * values. sub and sra are the two that sit next to a bit-manipulation
 * encoding in the same opcode. */
static void
test_base_intact(void)
{
    static const struct testcase tc[] = {
        { "sub still subtracts", ENC_R(0x20, RS2_N, 0), 100, 40, 60 },
        { "sra still shifts",    ENC_R(0x20, RS2_N, 5), 0xff000000, 4,
          0xfff00000 },
        { "srai still shifts",   ENC_I(0x20, 4, 5), 0xff000000, 0,
          0xfff00000 },
        { "slli still shifts",   ENC_I(0x00, 4, 1), 0x0000000f, 0,
          0x000000f0 },
    };

    run_all("the base instructions in the same encoding space still work",
            tc, sizeof(tc) / sizeof(tc[0]));
}

/** Read misa the way a guest would, with csrrs rd, misa, x0. */
static uint32_t
read_misa(void)
{
    uint32_t insn = (RV_CSR_MISA << 20) | (2u << 12) | (RD_N << 7) | 0x73;

    /* The extension switches are part of the machine, so rv_reset leaves
     * them alone and the caller's setting survives this call. */
    rv_reset(&cpu, CODE_ADDR);
    mach.mem[CODE_ADDR + 0] = (uint8_t)insn;
    mach.mem[CODE_ADDR + 1] = (uint8_t)(insn >> 8);
    mach.mem[CODE_ADDR + 2] = (uint8_t)(insn >> 16);
    mach.mem[CODE_ADDR + 3] = (uint8_t)(insn >> 24);
    rv_step(&cpu);
    return cpu.x[RD_N];
}

int
main(void)
{
    uint32_t misa;

    if (machine_init(&mach, &cpu))
        return 1;

    printf("Zba, Zbb and Zbs tests\n");

    run_all("Zba: shifted add", zba, sizeof(zba) / sizeof(zba[0]));
    run_all("Zbb: complemented logic", zbb_logic,
            sizeof(zbb_logic) / sizeof(zbb_logic[0]));
    run_all("Zbb: counting and sign extension", zbb_count,
            sizeof(zbb_count) / sizeof(zbb_count[0]));
    run_all("Zbb: minimum and maximum", zbb_minmax,
            sizeof(zbb_minmax) / sizeof(zbb_minmax[0]));
    run_all("Zbb: rotates and byte operations", zbb_rotate,
            sizeof(zbb_rotate) / sizeof(zbb_rotate[0]));
    run_all("Zbs: single-bit operations", zbs, sizeof(zbs) / sizeof(zbs[0]));

    test_base_intact();
    test_switch_off();

    /* misa advertises B, which by definition means all three present */
    printf("\nmisa reports the B extension\n");
    cpu.bitmanip = 1;
    misa = read_misa();
    tests++;
    if (misa & (1u << 1)) {
        printf("  ok   misa B bit set            %08x\n", misa);
    } else {
        failures++;
        printf("  FAIL misa B bit clear          %08x\n", misa);
    }
    cpu.bitmanip = 0;
    misa = read_misa();
    tests++;
    if (!(misa & (1u << 1))) {
        printf("  ok   misa B bit clear when off %08x\n", misa);
    } else {
        failures++;
        printf("  FAIL misa B bit still set      %08x\n", misa);
    }
    cpu.bitmanip = 1;

    printf("\n%d tests, %d failures\n", tests, failures);
    machine_free(&mach);
    return failures ? 1 : 0;
}
