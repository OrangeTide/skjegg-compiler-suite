/* test_zcmp.c : whole-frame push and pop tests */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The expected frame layout here was taken from a reference
 * implementation rather than from prose: qemu-riscv32 was run with
 * -cpu rv32,c=false,zca=true,zcf=true,zcmp=true on a program that pushes
 * known values and writes the resulting frame out, and these tests encode
 * what came back. lockstep.c re-checks the same thing dynamically. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rv32.h"
#include "machine.h"

#define CODE_ADDR   0x1000
#define STACK_TOP   0x8000

static rv_cpu cpu;
static machine mach;
static int tests, failures;

/* s0 and s1 are x8 and x9; s2 upwards continue at x18 */
static const int sreg[12] = { 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 };

/****************************************************************
 * Encoding
 ****************************************************************/

#define CM_PUSH     0x18
#define CM_POP      0x1a
#define CM_POPRETZ  0x1c
#define CM_POPRET   0x1e

static uint16_t
enc_frame(uint32_t funct5, uint32_t rlist, uint32_t spimm)
{
    return (uint16_t)(0xa002 | (funct5 << 8) | (rlist << 4) | (spimm << 2));
}

static uint16_t
enc_move(int is_mva01s, uint32_t r1, uint32_t r2)
{
    return (uint16_t)(0xa002 | (3u << 10) | (r1 << 7) |
                      ((is_mva01s ? 3u : 1u) << 5) | (r2 << 2));
}

static void
place(uint32_t addr, uint16_t insn)
{
    mach.mem[addr] = (uint8_t)insn;
    mach.mem[addr + 1] = (uint8_t)(insn >> 8);
}

static uint32_t
peek(uint32_t addr)
{
    return (uint32_t)mach.mem[addr] | ((uint32_t)mach.mem[addr + 1] << 8) |
           ((uint32_t)mach.mem[addr + 2] << 16) |
           ((uint32_t)mach.mem[addr + 3] << 24);
}

/** Load every register the list can name with a recognisable value. */
static void
seed_registers(void)
{
    int i;

    cpu.x[1] = 0xaaaa0001;
    for (i = 0; i < 12; i++)
        cpu.x[sreg[i]] = 0xaaaa0002 + (uint32_t)i;
}

static void
start(uint16_t insn)
{
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, insn);
    /* Clear the frame area so that "this byte was not written" is a
     * meaningful check rather than a leftover from an earlier case */
    memset(mach.mem + STACK_TOP - 256, 0, 256);
    cpu.x[2] = STACK_TOP;
    seed_registers();
}

/****************************************************************
 * Checks
 ****************************************************************/

static void
check(const char *name, uint32_t got, uint32_t want)
{
    tests++;
    if (got == want) {
        printf("  ok   %-44s %08x\n", name, got);
        return;
    }
    failures++;
    printf("  FAIL %-44s got %08x want %08x\n", name, got, want);
}

/** Expected register count and stack adjustment for a list */
static int
list_regs(uint32_t rlist)
{
    return rlist == 15 ? 13 : (int)rlist - 3;
}

static uint32_t
list_adj(uint32_t rlist, uint32_t spimm)
{
    uint32_t base = rlist <= 7 ? 16 : rlist <= 11 ? 32 : rlist <= 14 ? 48 : 64;

    return base + spimm * 16;
}

static void
test_push(uint32_t rlist, uint32_t spimm)
{
    char name[80];
    uint32_t adj = list_adj(rlist, spimm);
    int n = list_regs(rlist), i;
    uint32_t block;

    start(enc_frame(CM_PUSH, rlist, spimm));
    rv_step(&cpu);

    snprintf(name, sizeof(name), "cm.push rlist=%u spimm=%u moves sp",
             rlist, spimm);
    check(name, STACK_TOP - cpu.x[2], adj);

    /* Registers sit at the top of the frame, ra lowest, list ascending */
    block = STACK_TOP - (uint32_t)n * 4;
    for (i = 0; i < n; i++) {
        uint32_t want = i == 0 ? 0xaaaa0001 : 0xaaaa0002 + (uint32_t)(i - 1);

        snprintf(name, sizeof(name), "  slot %d holds %s%d", i,
                 i == 0 ? "ra" : "s", i == 0 ? 0 : i - 1);
        check(name, peek(block + (uint32_t)i * 4), want);
    }

    /* Anything below the register block is untouched padding */
    if (adj > (uint32_t)n * 4) {
        snprintf(name, sizeof(name), "  padding below the block is untouched");
        check(name, peek(cpu.x[2]), 0);
    }
}

static void
test_pop_family(uint32_t funct5, const char *what, uint32_t rlist)
{
    char name[80];
    uint32_t adj = list_adj(rlist, 0);
    int n = list_regs(rlist), i;
    uint32_t block, sp_before;

    /* Build a frame by hand, then pop it */
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, enc_frame(funct5, rlist, 0));
    sp_before = STACK_TOP - adj;
    cpu.x[2] = sp_before;
    block = STACK_TOP - (uint32_t)n * 4;
    for (i = 0; i < n; i++) {
        uint32_t v = 0xbbbb0000 + (uint32_t)i;

        mach.mem[block + i * 4 + 0] = (uint8_t)v;
        mach.mem[block + i * 4 + 1] = (uint8_t)(v >> 8);
        mach.mem[block + i * 4 + 2] = (uint8_t)(v >> 16);
        mach.mem[block + i * 4 + 3] = (uint8_t)(v >> 24);
    }
    cpu.x[10] = 0x12345678;
    rv_step(&cpu);

    snprintf(name, sizeof(name), "%s rlist=%u restores sp", what, rlist);
    check(name, cpu.x[2], sp_before + adj);

    for (i = 0; i < n; i++) {
        int reg = i == 0 ? 1 : sreg[i - 1];

        snprintf(name, sizeof(name), "  x%d restored", reg);
        check(name, cpu.x[reg], 0xbbbb0000 + (uint32_t)i);
    }

    if (funct5 == CM_POP) {
        snprintf(name, sizeof(name), "  falls through to the next "
                 "instruction");
        check(name, cpu.pc, CODE_ADDR + 2);
    } else {
        snprintf(name, sizeof(name), "  returns to the restored ra");
        check(name, cpu.pc, 0xbbbb0000);
    }
    if (funct5 == CM_POPRETZ) {
        snprintf(name, sizeof(name), "  clears a0");
        check(name, cpu.x[10], 0);
    } else if (funct5 == CM_POPRET) {
        snprintf(name, sizeof(name), "  leaves a0 alone");
        check(name, cpu.x[10], 0x12345678);
    }
}

static void
test_moves(void)
{
    printf("\nthe register-pair moves\n");

    start(enc_move(0, 2, 5));           /* cm.mvsa01 s2, s5 */
    cpu.x[10] = 0x11112222;
    cpu.x[11] = 0x33334444;
    rv_step(&cpu);
    check("cm.mvsa01 writes the first from a0", cpu.x[sreg[2]], 0x11112222);
    check("cm.mvsa01 writes the second from a1", cpu.x[sreg[5]], 0x33334444);

    start(enc_move(1, 1, 6));           /* cm.mva01s s1, s6 */
    cpu.x[sreg[1]] = 0x55556666;
    cpu.x[sreg[6]] = 0x77778888;
    rv_step(&cpu);
    check("cm.mva01s reads the first into a0", cpu.x[10], 0x55556666);
    check("cm.mva01s reads the second into a1", cpu.x[11], 0x77778888);

    printf("\nreserved and disabled encodings raise illegal instruction\n");
    start(enc_move(0, 3, 3));           /* the two registers must differ */
    rv_step(&cpu);
    tests++;
    if (cpu.in_trap && cpu.mcause == RV_CAUSE_ILLEGAL_INSN)
        printf("  ok   %-44s illegal\n", "cm.mvsa01 with equal registers");
    else {
        failures++;
        printf("  FAIL %-44s in_trap=%d cause=%u\n",
               "cm.mvsa01 with equal registers", cpu.in_trap, cpu.mcause);
    }

    start(enc_frame(CM_PUSH, 2, 0));    /* rlist below 4 is reserved */
    rv_step(&cpu);
    tests++;
    if (cpu.in_trap && cpu.mcause == RV_CAUSE_ILLEGAL_INSN)
        printf("  ok   %-44s illegal\n", "cm.push with a reserved rlist");
    else {
        failures++;
        printf("  FAIL %-44s in_trap=%d cause=%u\n",
               "cm.push with a reserved rlist", cpu.in_trap, cpu.mcause);
    }

    start(enc_frame(CM_PUSH, 9, 0));
    cpu.zcmp = 0;                       /* a plain RV32IMFC machine */
    rv_step(&cpu);
    tests++;
    if (cpu.in_trap && cpu.mcause == RV_CAUSE_ILLEGAL_INSN)
        printf("  ok   %-44s illegal\n", "cm.push with Zcmp disabled");
    else {
        failures++;
        printf("  FAIL %-44s in_trap=%d cause=%u\n",
               "cm.push with Zcmp disabled", cpu.in_trap, cpu.mcause);
    }
    cpu.zcmp = 1;
}

static void
test_round_trip(void)
{
    uint32_t rlist;

    printf("\na push followed by a pop restores every register\n");
    for (rlist = 4; rlist <= 15; rlist++) {
        char name[80];
        int n = list_regs(rlist), i, ok = 1;

        rv_reset(&cpu, CODE_ADDR);
        place(CODE_ADDR, enc_frame(CM_PUSH, rlist, 1));
        place(CODE_ADDR + 2, enc_frame(CM_POP, rlist, 1));
        cpu.x[2] = STACK_TOP;
        seed_registers();
        rv_step(&cpu);

        /* Scribble over the registers so the pop has to do real work */
        for (i = 0; i < 12; i++)
            cpu.x[sreg[i]] = 0xdeadbeef;
        cpu.x[1] = 0xdeadbeef;
        rv_step(&cpu);

        if (cpu.x[1] != 0xaaaa0001)
            ok = 0;
        for (i = 0; i < n - 1; i++) {
            if (cpu.x[sreg[i]] != 0xaaaa0002 + (uint32_t)i)
                ok = 0;
        }
        if (cpu.x[2] != STACK_TOP)
            ok = 0;

        snprintf(name, sizeof(name), "rlist=%u round trip (%d registers)",
                 rlist, n);
        check(name, ok ? 1 : 0, 1);
    }
}

int
main(void)
{
    if (machine_init(&mach, &cpu))
        return 1;

    printf("Zcmp push and pop tests\n");

    printf("\ncm.push places the register block at the top of the frame\n");
    test_push(4, 0);
    test_push(9, 0);
    test_push(15, 0);
    test_push(6, 1);

    printf("\ncm.pop and the returning forms\n");
    test_pop_family(CM_POP, "cm.pop", 9);
    test_pop_family(CM_POPRET, "cm.popret", 9);
    test_pop_family(CM_POPRETZ, "cm.popretz", 6);

    test_moves();
    test_round_trip();

    printf("\n%d tests, %d failures\n", tests, failures);
    machine_free(&mach);
    return failures ? 1 : 0;
}
