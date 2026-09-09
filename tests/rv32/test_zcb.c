/* test_zcb.c : compressed byte and halfword operations */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* Zcb is the one extension here whose whole content is a mapping: every
 * encoding stands for exactly one 32-bit instruction. So the first half of
 * this file tests the mapping directly, comparing rv_expand_zcb() against
 * what an assembler produces for the uncompressed form, and the second half
 * executes the memory ones to check that the offsets and the sign extension
 * came out right.
 *
 * Both columns of the expansion table were produced by GNU as at
 * -march=rv32imc_zbb_zcb, assembling the compressed mnemonic for one and
 * the uncompressed equivalent for the other. Neither number came from this
 * emulator. */

#include <stdio.h>
#include <stdlib.h>
#include "rv32.h"
#include "machine.h"

#define CODE_ADDR   0x1000
#define DATA_ADDR   0x2000

static rv_cpu cpu;
static machine mach;
static int tests, failures;

/****************************************************************
 * The expansion table
 ****************************************************************/

struct expansion {
    const char *name;
    uint16_t compressed;
    uint32_t expanded;
};

static const struct expansion expansions[] = {
    { "c.lbu s0, 0(s1)",    0x8080, 0x0004c403 },
    { "c.lbu a5, 3(a0)",    0x817c, 0x00354783 },
    { "c.lhu s0, 0(s1)",    0x8480, 0x0004d403 },
    { "c.lhu a5, 2(a0)",    0x853c, 0x00255783 },
    { "c.lh  s0, 2(s1)",    0x84e0, 0x00249403 },
    { "c.sb  a1, 1(a2)",    0x8a4c, 0x00b600a3 },
    { "c.sh  a1, 2(a2)",    0x8e2c, 0x00b61123 },
    { "c.zext.b a3",        0x9ee1, 0x0ff6f693 },
    { "c.sext.b a3",        0x9ee5, 0x60469693 },
    { "c.zext.h a3",        0x9ee9, 0x0806c6b3 },
    { "c.sext.h a3",        0x9eed, 0x60569693 },
    { "c.not a4",           0x9f75, 0xfff74713 },
    { "c.mul a4, a5",       0x9f5d, 0x02f70733 },
};

/* Encodings inside Zcb's space that no instruction claims. c.zext.w is
 * RV64 only and the rest are marked reserved, so all of them have to be
 * refused rather than expanded into something plausible. */
static const struct expansion reserved[] = {
    { "c.zext.w (RV64 only)",   0x9ef1, 0 },
    { "quadrant 0, bit 12 set", 0x9080, 0 },
    { "c.sh with bit 6 set",    0x8e2c | 0x40, 0 },
    { "quadrant 1, funct2 00",  0x9e01, 0 },
    { "quadrant 1, funct2 01",  0x9e21, 0 },
};

static void
check_expansions(void)
{
    size_t i;

    printf("\nevery encoding expands to the instruction it stands for\n");
    for (i = 0; i < sizeof(expansions) / sizeof(expansions[0]); i++) {
        uint32_t got = rv_expand_zcb(expansions[i].compressed);

        tests++;
        if (got == expansions[i].expanded) {
            printf("  ok   %-18s %04x -> %08x\n", expansions[i].name,
                   expansions[i].compressed, got);
        } else {
            failures++;
            printf("  FAIL %-18s %04x -> %08x want %08x\n",
                   expansions[i].name, expansions[i].compressed, got,
                   expansions[i].expanded);
        }
    }

    printf("\nthe reserved encodings are refused\n");
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        uint32_t got = rv_expand_zcb(reserved[i].compressed);

        tests++;
        if (got == 0) {
            printf("  ok   %-24s %04x refused\n", reserved[i].name,
                   reserved[i].compressed);
        } else {
            failures++;
            printf("  FAIL %-24s %04x expanded to %08x\n", reserved[i].name,
                   reserved[i].compressed, got);
        }
    }

    /* The base compressed decoder must not claim any of this space, or the
     * order the two are tried in would change the machine's behaviour. */
    printf("\nthe base compressed decoder leaves this space alone\n");
    for (i = 0; i < sizeof(expansions) / sizeof(expansions[0]); i++) {
        uint32_t got = rv_expand_c(expansions[i].compressed);

        tests++;
        if (got == 0) {
            printf("  ok   %04x illegal without Zcb\n",
                   expansions[i].compressed);
        } else {
            failures++;
            printf("  FAIL %04x expanded to %08x without Zcb\n",
                   expansions[i].compressed, got);
        }
    }
}

/****************************************************************
 * Execution
 ****************************************************************/

static void
place16(uint32_t addr, uint16_t insn)
{
    mach.mem[addr] = (uint8_t)insn;
    mach.mem[addr + 1] = (uint8_t)(insn >> 8);
}

static void
check(const char *name, uint32_t got, uint32_t want)
{
    tests++;
    if (got == want) {
        printf("  ok   %-34s %08x\n", name, got);
        return;
    }
    failures++;
    printf("  FAIL %-34s got %08x want %08x\n", name, got, want);
}

/** Run one encoding with s1 (x9) pointing at the data area. */
static void
run(uint16_t insn, uint32_t a0_in)
{
    rv_reset(&cpu, CODE_ADDR);
    place16(CODE_ADDR, insn);
    cpu.x[9] = DATA_ADDR;       /* s1 */
    cpu.x[8] = 0xdeadbeef;      /* s0, a destination */
    cpu.x[10] = a0_in;          /* a0, a source for the stores */
    rv_step(&cpu);
}

static void
seed_data(void)
{
    mach.mem[DATA_ADDR + 0] = 0x11;
    mach.mem[DATA_ADDR + 1] = 0x82;
    mach.mem[DATA_ADDR + 2] = 0x33;
    mach.mem[DATA_ADDR + 3] = 0xf4;
}

/* c.lbu s0, imm(s1) for imm 0 to 3, then the halfword forms. The point of
 * the byte cases is the offset: Zcb encodes its two bits in the opposite
 * order to every other compressed immediate, so an implementation that
 * copies the c.lw pattern reads the wrong byte for offsets 1 and 2 and
 * passes for 0 and 3. */
static void
test_loads(void)
{
    printf("\nloads, including the reversed byte offset\n");
    seed_data();

    run(0x8080, 0);                             /* c.lbu s0, 0(s1) */
    check("c.lbu offset 0", cpu.x[8], 0x11);
    run(0x8080 | 0x40, 0);                      /* c.lbu s0, 1(s1) */
    check("c.lbu offset 1", cpu.x[8], 0x82);
    run(0x8080 | 0x20, 0);                      /* c.lbu s0, 2(s1) */
    check("c.lbu offset 2", cpu.x[8], 0x33);
    run(0x8080 | 0x60, 0);                      /* c.lbu s0, 3(s1) */
    check("c.lbu offset 3", cpu.x[8], 0xf4);

    run(0x8480, 0);                             /* c.lhu s0, 0(s1) */
    check("c.lhu offset 0", cpu.x[8], 0x8211);
    run(0x8480 | 0x20, 0);                      /* c.lhu s0, 2(s1) */
    check("c.lhu offset 2", cpu.x[8], 0xf433);

    /* c.lh is the only Zcb load that sign extends */
    run(0x84c0, 0);                             /* c.lh s0, 0(s1) */
    check("c.lh sign extends", cpu.x[8], 0xffff8211);
    run(0x84c0 | 0x20, 0);                      /* c.lh s0, 2(s1) */
    check("c.lh offset 2", cpu.x[8], 0xfffff433);
}

static void
test_stores(void)
{
    printf("\nstores write the right width at the right offset\n");

    seed_data();
    run(0x8888 | 0x40, 0x000000aa);             /* c.sb a0, 1(s1) */
    check("c.sb offset 1", (uint32_t)mach.mem[DATA_ADDR + 1], 0xaa);
    check("  neighbour untouched", (uint32_t)mach.mem[DATA_ADDR + 2], 0x33);

    seed_data();
    run(0x8c88 | 0x20, 0x0000bbcc);             /* c.sh a0, 2(s1) */
    check("c.sh offset 2 low byte",
          (uint32_t)mach.mem[DATA_ADDR + 2], 0xcc);
    check("c.sh offset 2 high byte",
          (uint32_t)mach.mem[DATA_ADDR + 3], 0xbb);
    check("  neighbour untouched",
          (uint32_t)mach.mem[DATA_ADDR + 1], 0x82);

    /* A store takes only the low bits of the source register */
    seed_data();
    run(0x8888, 0xffffff77);                    /* c.sb a0, 0(s1) */
    check("c.sb truncates", (uint32_t)mach.mem[DATA_ADDR + 0], 0x77);
}

/* The one-operand forms all read and write the same register. */
static void
test_unary(void)
{
    printf("\nthe one-operand arithmetic forms\n");

    rv_reset(&cpu, CODE_ADDR);
    place16(CODE_ADDR, 0x9ee1);                 /* c.zext.b a3 */
    cpu.x[13] = 0x12345678;
    rv_step(&cpu);
    check("c.zext.b", cpu.x[13], 0x00000078);

    rv_reset(&cpu, CODE_ADDR);
    place16(CODE_ADDR, 0x9ee5);                 /* c.sext.b a3 */
    cpu.x[13] = 0x12345688;
    rv_step(&cpu);
    check("c.sext.b", cpu.x[13], 0xffffff88);

    rv_reset(&cpu, CODE_ADDR);
    place16(CODE_ADDR, 0x9ee9);                 /* c.zext.h a3 */
    cpu.x[13] = 0x12348765;
    rv_step(&cpu);
    check("c.zext.h", cpu.x[13], 0x00008765);

    rv_reset(&cpu, CODE_ADDR);
    place16(CODE_ADDR, 0x9eed);                 /* c.sext.h a3 */
    cpu.x[13] = 0x12348765;
    rv_step(&cpu);
    check("c.sext.h", cpu.x[13], 0xffff8765);

    rv_reset(&cpu, CODE_ADDR);
    place16(CODE_ADDR, 0x9f75);                 /* c.not a4 */
    cpu.x[14] = 0x0f0f0f0f;
    rv_step(&cpu);
    check("c.not", cpu.x[14], 0xf0f0f0f0);

    rv_reset(&cpu, CODE_ADDR);
    place16(CODE_ADDR, 0x9f5d);                 /* c.mul a4, a5 */
    cpu.x[14] = 7;
    cpu.x[15] = 6;
    rv_step(&cpu);
    check("c.mul", cpu.x[14], 42);

    /* Multiplication keeps the low word, like mul */
    rv_reset(&cpu, CODE_ADDR);
    place16(CODE_ADDR, 0x9f5d);                 /* c.mul a4, a5 */
    cpu.x[14] = 0x00010000;
    cpu.x[15] = 0x00010000;
    rv_step(&cpu);
    check("c.mul keeps the low word", cpu.x[14], 0);
}

/****************************************************************
 * The switches
 ****************************************************************/

static int
traps(uint16_t insn)
{
    rv_reset(&cpu, CODE_ADDR);
    place16(CODE_ADDR, insn);
    cpu.x[9] = DATA_ADDR;
    rv_step(&cpu);
    return cpu.in_trap && cpu.mcause == RV_CAUSE_ILLEGAL_INSN;
}

static void
test_switches(void)
{
    size_t i;

    printf("\nevery encoding traps when Zcb is off\n");
    cpu.zcb = 0;
    for (i = 0; i < sizeof(expansions) / sizeof(expansions[0]); i++) {
        uint16_t insn = expansions[i].compressed;

        tests++;
        if (traps(insn)) {
            printf("  ok   %04x traps\n", insn);
        } else {
            failures++;
            printf("  FAIL %04x did not trap\n", insn);
        }
    }
    cpu.zcb = 1;

    /* Three forms expand into Zbb and one into M. The specification makes
     * those a requirement of Zcb rather than an option, and here the
     * expanded instruction enforces it without a separate check. */
    printf("\nthe forms that expand into Zbb trap when Zbb is off\n");
    cpu.bitmanip = 0;
    {
        static const struct { const char *name; uint16_t insn; } dep[] = {
            { "c.sext.b", 0x9ee5 },
            { "c.zext.h", 0x9ee9 },
            { "c.sext.h", 0x9eed },
        };

        for (i = 0; i < sizeof(dep) / sizeof(dep[0]); i++) {
            tests++;
            if (traps(dep[i].insn)) {
                printf("  ok   %s traps\n", dep[i].name);
            } else {
                failures++;
                printf("  FAIL %s did not trap\n", dep[i].name);
            }
        }

        /* The forms that need nothing but the base set still work */
        rv_reset(&cpu, CODE_ADDR);
        place16(CODE_ADDR, 0x9ee1);             /* c.zext.b a3 */
        cpu.x[13] = 0x12345678;
        rv_step(&cpu);
        check("c.zext.b still works", cpu.x[13], 0x00000078);
    }
    cpu.bitmanip = 1;
}

int
main(void)
{
    if (machine_init(&mach, &cpu))
        return 1;

    printf("Zcb compressed byte and halfword tests\n");

    check_expansions();
    test_loads();
    test_stores();
    test_unary();
    test_switches();

    printf("\n%d tests, %d failures\n", tests, failures);
    machine_free(&mach);
    return failures ? 1 : 0;
}
