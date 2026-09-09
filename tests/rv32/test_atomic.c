/* test_atomic.c : tests for the A extension */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The A extension carries a weaker test story than the rest of this
 * emulator. The compliance suite's atomic tests live on a branch that the
 * available assembler cannot build, and the instruction fuzzer excludes
 * memory access by design, so these directed tests and lockstep against
 * qemu are the whole of the evidence.
 *
 * That is defensible only because the semantics are narrow here. This is a
 * single hart with no interrupts, so no other agent can interleave with a
 * read-modify-write, and the acquire and release bits order accesses that
 * nothing else can observe. What remains to get right is the arithmetic of
 * each operation, the value returned, the reservation that lr.w and sc.w
 * carry, and the alignment rule. Those are what these tests cover. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rv32.h"
#include "machine.h"

#define CODE_ADDR   0x1000
#define DATA_ADDR   0x2000

static rv_cpu cpu;
static machine mach;
static int tests, failures;

/****************************************************************
 * Encoding
 ****************************************************************/

#define AMOADD      0x00
#define AMOSWAP     0x01
#define LR          0x02
#define SC          0x03
#define AMOXOR      0x04
#define AMOOR       0x08
#define AMOAND      0x0c
#define AMOMIN      0x10
#define AMOMAX      0x14
#define AMOMINU     0x18
#define AMOMAXU     0x1c

/* rs1 holds the address, rs2 the operand, rd the result */
#define RS1 1
#define RS2 2
#define RD  10

static uint32_t
enc_amo_rs2(uint32_t funct5, uint32_t aq, uint32_t rl, uint32_t rs2)
{
    return (funct5 << 27) | (aq << 26) | (rl << 25) | (rs2 << 20) |
           (RS1 << 15) | (2u << 12) | (RD << 7) | 0x2f;
}

static uint32_t
enc_amo(uint32_t funct5, uint32_t aq, uint32_t rl)
{
    return enc_amo_rs2(funct5, aq, rl, RS2);
}

/* lr.w has no second operand: the field must be zero */
static uint32_t
enc_lr(void)
{
    return enc_amo_rs2(LR, 0, 0, 0);
}

static void
place(uint32_t addr, uint32_t insn)
{
    mach.mem[addr + 0] = (uint8_t)insn;
    mach.mem[addr + 1] = (uint8_t)(insn >> 8);
    mach.mem[addr + 2] = (uint8_t)(insn >> 16);
    mach.mem[addr + 3] = (uint8_t)(insn >> 24);
}

static uint32_t
peek(uint32_t addr)
{
    return (uint32_t)mach.mem[addr] | ((uint32_t)mach.mem[addr + 1] << 8) |
           ((uint32_t)mach.mem[addr + 2] << 16) |
           ((uint32_t)mach.mem[addr + 3] << 24);
}

static void
poke(uint32_t addr, uint32_t v)
{
    mach.mem[addr + 0] = (uint8_t)v;
    mach.mem[addr + 1] = (uint8_t)(v >> 8);
    mach.mem[addr + 2] = (uint8_t)(v >> 16);
    mach.mem[addr + 3] = (uint8_t)(v >> 24);
}

/** Reset, install one atomic, and run it against the given memory word. */
static void
run(uint32_t insn, uint32_t mem_value, uint32_t operand, uint32_t addr)
{
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, insn);
    poke(DATA_ADDR, mem_value);
    cpu.x[RS1] = addr;
    cpu.x[RS2] = operand;
    rv_step(&cpu);
}

/****************************************************************
 * Checks
 ****************************************************************/

static void
check(const char *name, uint32_t got, uint32_t want)
{
    tests++;
    if (got == want) {
        printf("  ok   %-40s %08x\n", name, got);
        return;
    }
    failures++;
    printf("  FAIL %-40s got %08x want %08x\n", name, got, want);
}

/** An atomic returns the value from before the update and leaves the
 * updated value behind. */
static void
expect_amo(const char *name, uint32_t funct5, uint32_t before,
           uint32_t operand, uint32_t after)
{
    char buf[64];

    run(enc_amo(funct5, 0, 0), before, operand, DATA_ADDR);
    snprintf(buf, sizeof(buf), "%s returns the old value", name);
    check(buf, cpu.x[RD], before);
    snprintf(buf, sizeof(buf), "%s stores the new value", name);
    check(buf, peek(DATA_ADDR), after);
}

static void
check_trap(const char *name, uint32_t cause, uint32_t tval)
{
    tests++;
    if (cpu.in_trap && cpu.mcause == cause && cpu.mtval == tval) {
        printf("  ok   %-40s cause=%u tval=%08x\n", name, cause, tval);
        return;
    }
    failures++;
    printf("  FAIL %-40s in_trap=%d cause=%u tval=%08x, wanted cause=%u\n",
           name, cpu.in_trap, cpu.mcause, cpu.mtval, cause);
}

/****************************************************************
 * Tests
 ****************************************************************/

static void
test_arithmetic(void)
{
    printf("\nevery atomic returns the value it replaced\n");
    expect_amo("amoswap.w", AMOSWAP, 0x11112222, 0xaaaabbbb, 0xaaaabbbb);
    expect_amo("amoadd.w", AMOADD, 100, 23, 123);
    expect_amo("amoxor.w", AMOXOR, 0xff00ff00, 0x0f0f0f0f, 0xf00ff00f);
    expect_amo("amoor.w", AMOOR, 0xff000000, 0x0000ffff, 0xff00ffff);
    expect_amo("amoand.w", AMOAND, 0xff00ff00, 0x0f0f0f0f, 0x0f000f00);

    printf("\naddition wraps rather than saturating\n");
    expect_amo("amoadd.w overflow", AMOADD, 0xffffffff, 2, 1);

    printf("\nthe minimum and maximum forms differ in signedness\n");
    expect_amo("amomin.w picks the signed smaller", AMOMIN,
               0xffffffff, 1, 0xffffffff);
    expect_amo("amomax.w picks the signed larger", AMOMAX,
               0xffffffff, 1, 1);
    expect_amo("amominu.w picks the unsigned smaller", AMOMINU,
               0xffffffff, 1, 1);
    expect_amo("amomaxu.w picks the unsigned larger", AMOMAXU,
               0xffffffff, 1, 0xffffffff);
    expect_amo("amomin.w with equal operands", AMOMIN, 7, 7, 7);
    expect_amo("amomax.w at the signed boundary", AMOMAX,
               0x80000000, 0x7fffffff, 0x7fffffff);
}

static void
test_reservation(void)
{
    uint32_t lr = enc_lr(), sc = enc_amo(SC, 0, 0);

    printf("\na store conditional after its load reserved succeeds\n");
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, lr);
    place(CODE_ADDR + 4, sc);
    poke(DATA_ADDR, 0x1234abcd);
    cpu.x[RS1] = DATA_ADDR;
    cpu.x[RS2] = 0xfeedface;
    rv_step(&cpu);
    check("lr.w reads the word", cpu.x[RD], 0x1234abcd);
    rv_step(&cpu);
    check("sc.w reports success", cpu.x[RD], 0);
    check("sc.w stored the value", peek(DATA_ADDR), 0xfeedface);

    printf("\na store conditional without a reservation fails\n");
    run(sc, 0x1234abcd, 0xfeedface, DATA_ADDR);
    check("sc.w reports failure", cpu.x[RD], 1);
    check("sc.w wrote nothing", peek(DATA_ADDR), 0x1234abcd);

    printf("\na reservation covers one address and one use\n");
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, lr);
    place(CODE_ADDR + 4, sc);
    poke(DATA_ADDR, 1);
    poke(DATA_ADDR + 4, 2);
    cpu.x[RS1] = DATA_ADDR;
    cpu.x[RS2] = 0x99999999;
    rv_step(&cpu);
    cpu.x[RS1] = DATA_ADDR + 4;         /* a different address */
    rv_step(&cpu);
    check("sc.w to another address fails", cpu.x[RD], 1);
    check("and writes nothing", peek(DATA_ADDR + 4), 2);

    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, lr);
    place(CODE_ADDR + 4, sc);
    place(CODE_ADDR + 8, sc);
    poke(DATA_ADDR, 0);
    cpu.x[RS1] = DATA_ADDR;
    cpu.x[RS2] = 0x55555555;
    rv_step(&cpu);
    rv_step(&cpu);
    check("the first sc.w succeeds", cpu.x[RD], 0);
    cpu.x[RS2] = 0x66666666;
    rv_step(&cpu);
    check("a second sc.w fails", cpu.x[RD], 1);
    check("and leaves the first value", peek(DATA_ADDR), 0x55555555);

    printf("\na trap clears any outstanding reservation\n");
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, lr);
    poke(DATA_ADDR, 0);
    cpu.x[RS1] = DATA_ADDR;
    rv_step(&cpu);
    tests++;
    if (cpu.res_valid) {
        printf("  ok   %-40s reservation held\n", "lr.w takes a reservation");
    } else {
        failures++;
        printf("  FAIL %-40s no reservation\n", "lr.w takes a reservation");
    }
    rv_trap(&cpu, RV_CAUSE_BREAKPOINT, 0);
    tests++;
    if (!cpu.res_valid) {
        printf("  ok   %-40s reservation dropped\n", "a trap clears it");
    } else {
        failures++;
        printf("  FAIL %-40s reservation survived\n", "a trap clears it");
    }
}

static void
test_alignment(void)
{
    printf("\nan atomic must be naturally aligned even when ordinary\n"
           "misaligned access is emulated\n");

    cpu.trap_misaligned = 0;            /* the permissive default */

    run(enc_amo(AMOADD, 0, 0), 0, 1, DATA_ADDR + 1);
    check_trap("amoadd.w at offset 1", RV_CAUSE_STORE_MISALIGNED,
               DATA_ADDR + 1);
    run(enc_amo(AMOADD, 0, 0), 0, 1, DATA_ADDR + 2);
    check_trap("amoadd.w at offset 2", RV_CAUSE_STORE_MISALIGNED,
               DATA_ADDR + 2);
    run(enc_lr(), 0, 0, DATA_ADDR + 2);
    check_trap("lr.w at offset 2", RV_CAUSE_LOAD_MISALIGNED, DATA_ADDR + 2);
    run(enc_amo(SC, 0, 0), 0, 1, DATA_ADDR + 3);
    check_trap("sc.w at offset 3", RV_CAUSE_STORE_MISALIGNED, DATA_ADDR + 3);

    run(enc_amo(AMOADD, 0, 0), 5, 1, DATA_ADDR);
    tests++;
    if (!cpu.in_trap && peek(DATA_ADDR) == 6) {
        printf("  ok   %-40s no trap\n", "an aligned atomic still works");
    } else {
        failures++;
        printf("  FAIL %-40s in_trap=%d value=%08x\n",
               "an aligned atomic still works", cpu.in_trap,
               peek(DATA_ADDR));
    }
}

static void
test_encodings(void)
{
    printf("\nordering bits do not change the result on a single hart\n");
    expect_amo("amoadd.w aq", AMOADD, 10, 5, 15);
    run(enc_amo(AMOADD, 1, 0), 10, 5, DATA_ADDR);
    check("amoadd.w.aq stores the same", peek(DATA_ADDR), 15);
    run(enc_amo(AMOADD, 0, 1), 10, 5, DATA_ADDR);
    check("amoadd.w.rl stores the same", peek(DATA_ADDR), 15);
    run(enc_amo(AMOADD, 1, 1), 10, 5, DATA_ADDR);
    check("amoadd.w.aqrl stores the same", peek(DATA_ADDR), 15);

    printf("\nreserved and disabled encodings raise illegal instruction\n");
    run(enc_amo(0x05, 0, 0), 0, 0, DATA_ADDR);      /* no such operation */
    check_trap("an unassigned funct5", RV_CAUSE_ILLEGAL_INSN,
               enc_amo(0x05, 0, 0));

    /* lr.w takes no second operand */
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, enc_lr() | (1u << 20));
    cpu.x[RS1] = DATA_ADDR;
    rv_step(&cpu);
    check_trap("lr.w with rs2 set", RV_CAUSE_ILLEGAL_INSN,
               enc_lr() | (1u << 20));

    /* RV32 has only the word-sized forms */
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, (enc_amo(AMOADD, 0, 0) & ~(7u << 12)) | (3u << 12));
    cpu.x[RS1] = DATA_ADDR;
    rv_step(&cpu);
    tests++;
    if (cpu.in_trap && cpu.mcause == RV_CAUSE_ILLEGAL_INSN)
        printf("  ok   %-40s illegal\n", "the doubleword form on RV32");
    else {
        failures++;
        printf("  FAIL %-40s in_trap=%d cause=%u\n",
               "the doubleword form on RV32", cpu.in_trap, cpu.mcause);
    }

    run(enc_amo(AMOADD, 0, 0), 0, 1, DATA_ADDR);
    cpu.atomics = 0;
    rv_reset(&cpu, CODE_ADDR);
    cpu.atomics = 0;
    place(CODE_ADDR, enc_amo(AMOADD, 0, 0));
    cpu.x[RS1] = DATA_ADDR;
    rv_step(&cpu);
    tests++;
    if (cpu.in_trap && cpu.mcause == RV_CAUSE_ILLEGAL_INSN)
        printf("  ok   %-40s illegal\n", "an atomic with A disabled");
    else {
        failures++;
        printf("  FAIL %-40s in_trap=%d cause=%u\n",
               "an atomic with A disabled", cpu.in_trap, cpu.mcause);
    }
    cpu.atomics = 1;
}

static void
test_misa(void)
{
    uint32_t out = 0;

    printf("\nmisa advertises what is implemented\n");
    tests++;
    /* csrr rd, misa */
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, (0x301u << 20) | (0u << 15) | (2u << 12) |
                     (RD << 7) | 0x73);
    rv_step(&cpu);
    out = cpu.x[RD];
    if ((out & 1) && (out & (1u << 8)) && (out & (1u << 12)) &&
        (out & (1u << 5)) && (out & (1u << 2))) {
        printf("  ok   %-40s %08x\n", "misa reports I, M, A, F and C", out);
    } else {
        failures++;
        printf("  FAIL %-40s %08x\n", "misa reports I, M, A, F and C", out);
    }
}

int
main(void)
{
    if (machine_init(&mach, &cpu))
        return 1;

    printf("A extension tests\n");

    test_arithmetic();
    test_reservation();
    test_alignment();
    test_encodings();
    test_misa();

    printf("\n%d tests, %d failures\n", tests, failures);
    machine_free(&mach);
    return failures ? 1 : 0;
}
