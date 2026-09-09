/* test_mem.c : memory access, alignment and sandbox tests */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The differential tools compare against qemu running compiled code, and
 * compiled code is always aligned and always inside its own memory. These
 * tests cover what that leaves out: misaligned access in both of the
 * emulator's modes, and the access-check callback an embedding uses to
 * sandbox a guest. */

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

/* Set by the probe to refuse everything at or above this address */
static uint32_t forbidden = 0xffffffff;
static int probe_calls;

static int
probe(void *ctx, uint32_t addr, int size, int is_write)
{
    (void)ctx;
    (void)size;
    (void)is_write;
    probe_calls++;
    return addr >= forbidden;
}

/****************************************************************
 * Encoding helpers
 ****************************************************************/

static uint32_t
enc_load(uint32_t f3, uint32_t rd, uint32_t rs1, int32_t imm)
{
    return (((uint32_t)imm & 0xfff) << 20) | (rs1 << 15) | (f3 << 12) |
           (rd << 7) | 0x03;
}

static uint32_t
enc_store(uint32_t f3, uint32_t rs2, uint32_t rs1, int32_t imm)
{
    uint32_t u = (uint32_t)imm;

    return (((u >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | ((u & 0x1f) << 7) | 0x23;
}

static void
place(uint32_t addr, uint32_t insn)
{
    mach.mem[addr + 0] = (uint8_t)insn;
    mach.mem[addr + 1] = (uint8_t)(insn >> 8);
    mach.mem[addr + 2] = (uint8_t)(insn >> 16);
    mach.mem[addr + 3] = (uint8_t)(insn >> 24);
}

/** Reset, install one instruction, point x1 at the data area and step. */
static void
run(uint32_t insn, uint32_t x2, int trap_misaligned)
{
    rv_reset(&cpu, CODE_ADDR);
    cpu.trap_misaligned = trap_misaligned;
    place(CODE_ADDR, insn);
    cpu.x[1] = DATA_ADDR;
    cpu.x[2] = x2;
    rv_step(&cpu);
}

static void
check(const char *name, uint32_t got, uint32_t want)
{
    tests++;
    if (got == want) {
        printf("  ok   %-42s %08x\n", name, got);
        return;
    }
    failures++;
    printf("  FAIL %-42s got %08x want %08x\n", name, got, want);
}

static void
check_trap(const char *name, uint32_t cause, uint32_t tval)
{
    tests++;
    if (cpu.in_trap && cpu.mcause == cause && cpu.mtval == tval) {
        printf("  ok   %-42s cause=%u tval=%08x\n", name, cause, tval);
        return;
    }
    failures++;
    printf("  FAIL %-42s in_trap=%d cause=%u tval=%08x, wanted cause=%u "
           "tval=%08x\n", name, cpu.in_trap, cpu.mcause, cpu.mtval, cause,
           tval);
}

static void
check_no_trap(const char *name)
{
    tests++;
    if (!cpu.in_trap) {
        printf("  ok   %-42s no trap\n", name);
        return;
    }
    failures++;
    printf("  FAIL %-42s trapped with cause=%u\n", name, cpu.mcause);
}

/****************************************************************
 * Tests
 ****************************************************************/

static void
fill_data(void)
{
    static const uint8_t pattern[8] = {
        0x11, 0x22, 0x33, 0xf4, 0x55, 0x66, 0x77, 0x88,
    };

    memcpy(mach.mem + DATA_ADDR, pattern, sizeof(pattern));
}

static void
test_aligned(void)
{
    printf("\naligned loads sign-extend or zero-extend as the width says\n");
    fill_data();

    run(enc_load(0, 5, 1, 3), 0, 0);        /* lb of 0xf4 */
    check("lb", cpu.x[5], 0xfffffff4);
    run(enc_load(4, 5, 1, 3), 0, 0);        /* lbu */
    check("lbu", cpu.x[5], 0x000000f4);
    run(enc_load(1, 5, 1, 2), 0, 0);        /* lh of 0xf433 */
    check("lh", cpu.x[5], 0xfffff433);
    run(enc_load(5, 5, 1, 2), 0, 0);        /* lhu */
    check("lhu", cpu.x[5], 0x0000f433);
    run(enc_load(2, 5, 1, 0), 0, 0);        /* lw */
    check("lw", cpu.x[5], 0xf4332211);

    printf("\nstores write exactly their own width\n");
    memset(mach.mem + DATA_ADDR, 0, 8);
    run(enc_store(0, 2, 1, 0), 0xaabbccdd, 0);
    check("sb writes one byte", mach.mem[DATA_ADDR], 0xdd);
    check("sb leaves the next byte", mach.mem[DATA_ADDR + 1], 0);
    memset(mach.mem + DATA_ADDR, 0, 8);
    run(enc_store(1, 2, 1, 0), 0xaabbccdd, 0);
    check("sh low byte", mach.mem[DATA_ADDR], 0xdd);
    check("sh high byte", mach.mem[DATA_ADDR + 1], 0xcc);
    check("sh leaves the next byte", mach.mem[DATA_ADDR + 2], 0);

    printf("\na load into x0 is discarded\n");
    run(enc_load(2, 0, 1, 0), 0, 0);
    check("lw x0", cpu.x[0], 0);
}

static void
test_misaligned_emulated(void)
{
    printf("\nmisaligned access is emulated by default\n");
    fill_data();

    /* 0x2001 holds 22 33 f4 55 little-endian */
    run(enc_load(2, 5, 1, 1), 0, 0);
    check("lw at offset 1", cpu.x[5], 0x55f43322);
    check_no_trap("lw at offset 1 does not trap");

    run(enc_load(2, 5, 1, 3), 0, 0);
    check("lw at offset 3", cpu.x[5], 0x776655f4);

    run(enc_load(1, 5, 1, 1), 0, 0);
    check("lh at offset 1", cpu.x[5], 0x00003322);

    memset(mach.mem + DATA_ADDR, 0, 8);
    run(enc_store(2, 2, 1, 1), 0xaabbccdd, 0);
    check("sw at offset 1, byte 0", mach.mem[DATA_ADDR + 1], 0xdd);
    check("sw at offset 1, byte 3", mach.mem[DATA_ADDR + 4], 0xaa);
    check("sw at offset 1 leaves byte 0", mach.mem[DATA_ADDR], 0);
}

static void
test_misaligned_trapping(void)
{
    printf("\nwith trap_misaligned set, the same accesses fault\n");
    fill_data();

    run(enc_load(2, 5, 1, 1), 0, 1);
    check_trap("lw at offset 1", RV_CAUSE_LOAD_MISALIGNED, DATA_ADDR + 1);

    run(enc_load(1, 5, 1, 1), 0, 1);
    check_trap("lh at offset 1", RV_CAUSE_LOAD_MISALIGNED, DATA_ADDR + 1);

    run(enc_store(2, 2, 1, 2), 0xaabbccdd, 1);
    check_trap("sw at offset 2", RV_CAUSE_STORE_MISALIGNED, DATA_ADDR + 2);

    run(enc_load(0, 5, 1, 1), 0, 1);
    check_no_trap("lb is never misaligned");

    run(enc_load(2, 5, 1, 0), 0, 1);
    check_no_trap("an aligned lw still works");
}

static void
test_sandbox(void)
{
    printf("\nthe access check refuses reads and writes outside the "
           "sandbox\n");
    fill_data();
    rv_set_probe(&cpu, probe);

    forbidden = DATA_ADDR + 4;
    run(enc_load(2, 5, 1, 0), 0, 0);
    check_no_trap("a load below the limit is allowed");
    check("and returns the right value", cpu.x[5], 0xf4332211);

    run(enc_load(2, 5, 1, 4), 0, 0);
    check_trap("a load at the limit faults", RV_CAUSE_LOAD_ACCESS_FAULT,
               DATA_ADDR + 4);

    run(enc_store(2, 2, 1, 4), 0xaabbccdd, 0);
    check_trap("a store at the limit faults", RV_CAUSE_STORE_ACCESS_FAULT,
               DATA_ADDR + 4);

    tests++;
    if (mach.mem[DATA_ADDR + 4] == 0x55) {
        printf("  ok   %-42s memory untouched\n",
               "a refused store writes nothing");
    } else {
        failures++;
        printf("  FAIL %-42s memory was modified\n",
               "a refused store writes nothing");
    }

    printf("\nthe check also covers instruction fetch\n");
    forbidden = CODE_ADDR;
    rv_reset(&cpu, CODE_ADDR);
    place(CODE_ADDR, enc_load(2, 5, 1, 0));
    rv_step(&cpu);
    check_trap("fetch from a refused page", RV_CAUSE_INSN_ACCESS_FAULT,
               CODE_ADDR);

    forbidden = 0xffffffff;
    rv_set_probe(&cpu, NULL);
}

static void
test_fetch_alignment(void)
{
    printf("\nan odd program counter is an instruction address fault\n");
    rv_reset(&cpu, CODE_ADDR + 1);
    rv_step(&cpu);
    check_trap("pc with bit 0 set", RV_CAUSE_INSN_MISALIGNED, CODE_ADDR + 1);
}

int
main(void)
{
    if (machine_init(&mach, &cpu))
        return 1;

    printf("memory access and sandbox tests\n");

    test_aligned();
    test_misaligned_emulated();
    test_misaligned_trapping();
    test_sandbox();
    test_fetch_alignment();

    printf("\n%d tests, %d failures (%d access checks performed)\n", tests,
           failures, probe_calls);
    machine_free(&mach);
    return failures ? 1 : 0;
}
