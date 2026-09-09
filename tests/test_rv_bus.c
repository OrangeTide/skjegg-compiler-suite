/* test_rv_bus.c : how a guest access reaches the memory bus
 *
 * Every other test in the rig checks the *value* a load produces, and
 * the interpreter has two ways of producing it: one wide callback when
 * the address is aligned, and a byte-at-a-time assembly when it is not.
 * Over plain RAM the two agree exactly, so nothing that reads only the
 * value can tell them apart, and inverting the alignment test survives
 * the whole rig.
 *
 * It matters to an embedder rather than to a program. The callbacks are
 * where a host puts its devices, and a device register read once as a
 * word is not the same transaction as the same register read four times
 * a byte at a time: side effects fire a different number of times, and
 * a register narrower than the access sees a different pattern
 * entirely. That is exactly what skj-run's own arch-test board has, in
 * its 16550 transmitter and its CLINT.
 *
 * So this test counts callbacks rather than checking values, which is
 * the only way the distinction is visible.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "rv32.h"

#include <stdio.h>
#include <string.h>

#define MEM_SIZE    0x4000
#define CODE_BASE   0x1000
#define DATA        0x200       /* 8-aligned, room either side */

static uint8_t mem[MEM_SIZE];
static int n_r8, n_r16, n_r32, n_w8, n_w16, n_w32;
static int failures;

/****************************************************************
 * A bus that records how it was asked, not just what for
 ****************************************************************/

/* Only the guest's own access is interesting.  Fetching the instruction
 * reaches the same bus, as two 16-bit reads, and would otherwise be
 * counted as if the program had made it. */
static int
is_data(uint32_t a)
{
    return a >= DATA - 16 && a < DATA + 64;
}

static uint32_t
r8(void *c, uint32_t a)
{
    (void)c;
    if (is_data(a))
        n_r8++;
    return a < MEM_SIZE ? mem[a] : 0;
}

static uint32_t
r16(void *c, uint32_t a)
{
    (void)c;
    if (is_data(a))
        n_r16++;
    if (a + 1 >= MEM_SIZE)
        return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8);
}

static uint32_t
r32(void *c, uint32_t a)
{
    (void)c;
    if (is_data(a))
        n_r32++;
    if (a + 3 >= MEM_SIZE)
        return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8) |
           ((uint32_t)mem[a + 2] << 16) | ((uint32_t)mem[a + 3] << 24);
}

static void
w8(void *c, uint32_t a, uint32_t v)
{
    (void)c;
    if (is_data(a))
        n_w8++;
    if (a < MEM_SIZE)
        mem[a] = (uint8_t)v;
}

static void
w16(void *c, uint32_t a, uint32_t v)
{
    (void)c;
    if (is_data(a))
        n_w16++;
    if (a + 1 < MEM_SIZE) {
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
    }
}

static void
w32(void *c, uint32_t a, uint32_t v)
{
    (void)c;
    if (is_data(a))
        n_w32++;
    if (a + 3 < MEM_SIZE) {
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
        mem[a + 2] = (uint8_t)(v >> 16);
        mem[a + 3] = (uint8_t)(v >> 24);
    }
}

/****************************************************************
 * Guest instructions, as an assembler encodes them
 *
 *   lw   a0, 0(a1)   0x0005a503
 *   sw   a0, 0(a1)   0x00a5a023
 *   lh   a0, 0(a1)   0x00059503
 *   sh   a0, 0(a1)   0x00a59023
 *
 * a1 carries the address, so one encoding covers every alignment.
 ****************************************************************/

#define I_LW    0x0005a503u
#define I_SW    0x00a5a023u
#define I_LH    0x00059503u
#define I_SH    0x00a59023u

static void
poke32(uint32_t addr, uint32_t v)
{
    mem[addr] = (uint8_t)v;
    mem[addr + 1] = (uint8_t)(v >> 8);
    mem[addr + 2] = (uint8_t)(v >> 16);
    mem[addr + 3] = (uint8_t)(v >> 24);
}

/** Run one instruction against `addr`, with a0 preloaded for stores. */
static void
run_one(rv_cpu *cpu, uint32_t insn, uint32_t addr, uint32_t store_val)
{
    memset(mem, 0, sizeof mem);
    poke32(CODE_BASE, insn);
    poke32(DATA, 0x44332211u);
    poke32(DATA + 4, 0x88776655u);

    rv_init(cpu, r8, r16, r32, w8, w16, w32, NULL);
    rv_reset(cpu, CODE_BASE);
    cpu->x[11] = addr;                  /* a1 */
    cpu->x[10] = store_val;             /* a0 */

    n_r8 = n_r16 = n_r32 = n_w8 = n_w16 = n_w32 = 0;
    rv_step(cpu);
}

static void
check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want)
        return;
    printf("FAIL %s: got %u (0x%08x), want %u (0x%08x)\n", what, got, got,
           want, want);
    failures++;
}

/****************************************************************
 * An aligned access is one wide callback
 ****************************************************************/

static void
test_aligned_is_wide(void)
{
    rv_cpu cpu;

    run_one(&cpu, I_LW, DATA, 0);
    check("aligned lw: value", cpu.x[10], 0x44332211u);
    check("aligned lw: one read32", (uint32_t)n_r32, 1);
    check("aligned lw: no byte reads", (uint32_t)n_r8, 0);

    run_one(&cpu, I_SW, DATA, 0xdeadbeefu);
    check("aligned sw: one write32", (uint32_t)n_w32, 1);
    check("aligned sw: no byte writes", (uint32_t)n_w8, 0);
    check("aligned sw: landed", (uint32_t)mem[DATA] |
          ((uint32_t)mem[DATA + 1] << 8) | ((uint32_t)mem[DATA + 2] << 16) |
          ((uint32_t)mem[DATA + 3] << 24), 0xdeadbeefu);

    run_one(&cpu, I_LH, DATA + 2, 0);
    check("aligned lh: one read16", (uint32_t)n_r16, 1);
    check("aligned lh: no byte reads", (uint32_t)n_r8, 0);

    run_one(&cpu, I_SH, DATA + 2, 0x1234u);
    check("aligned sh: one write16", (uint32_t)n_w16, 1);
    check("aligned sh: no byte writes", (uint32_t)n_w8, 0);
}

/****************************************************************
 * A misaligned one is assembled from bytes, and only from bytes
 ****************************************************************/

static void
test_misaligned_is_bytes(void)
{
    rv_cpu cpu;

    run_one(&cpu, I_LW, DATA + 1, 0);
    check("misaligned lw: value", cpu.x[10], 0x55443322u);
    check("misaligned lw: four byte reads", (uint32_t)n_r8, 4);
    check("misaligned lw: no read32", (uint32_t)n_r32, 0);
    check("misaligned lw: no read16", (uint32_t)n_r16, 0);

    run_one(&cpu, I_SW, DATA + 1, 0xdeadbeefu);
    check("misaligned sw: four byte writes", (uint32_t)n_w8, 4);
    check("misaligned sw: no write32", (uint32_t)n_w32, 0);
    check("misaligned sw: landed", (uint32_t)mem[DATA + 1] |
          ((uint32_t)mem[DATA + 2] << 8) | ((uint32_t)mem[DATA + 3] << 16) |
          ((uint32_t)mem[DATA + 4] << 24), 0xdeadbeefu);

    run_one(&cpu, I_LH, DATA + 1, 0);
    check("misaligned lh: two byte reads", (uint32_t)n_r8, 2);
    check("misaligned lh: no read16", (uint32_t)n_r16, 0);

    run_one(&cpu, I_SH, DATA + 1, 0x1234u);
    check("misaligned sh: two byte writes", (uint32_t)n_w8, 2);
    check("misaligned sh: no write16", (uint32_t)n_w16, 0);
}

/****************************************************************
 * A byte access is a byte callback whatever the address
 ****************************************************************/

static void
test_byte_access(void)
{
    rv_cpu cpu;

    /* lbu a0, 0(a1) : 0x0005c503 */
    run_one(&cpu, 0x0005c503u, DATA, 0);
    check("lbu aligned: one byte read", (uint32_t)n_r8, 1);
    check("lbu aligned: no wide read", (uint32_t)(n_r16 + n_r32), 0);

    run_one(&cpu, 0x0005c503u, DATA + 1, 0);
    check("lbu odd: one byte read", (uint32_t)n_r8, 1);
    check("lbu odd: no wide read", (uint32_t)(n_r16 + n_r32), 0);
}

/****************************************************************
 * With trapping turned on, a misaligned access takes a trap instead
 * of reaching the bus at all
 ****************************************************************/

static void
test_trap_misaligned(void)
{
    rv_cpu cpu;

    run_one(&cpu, I_LW, DATA, 0);       /* init the machine, then re-run */
    cpu.trap_misaligned = 1;
    cpu.mtvec = 0x3000;
    cpu.pc = CODE_BASE;
    cpu.x[11] = DATA + 1;
    n_r8 = n_r16 = n_r32 = 0;
    rv_step(&cpu);
    check("trapping: no bus access at all",
          (uint32_t)(n_r8 + n_r16 + n_r32), 0);
    check("trapping: cause is a misaligned load", cpu.mcause,
          RV_CAUSE_LOAD_MISALIGNED);
    check("trapping: mtval is the address", cpu.mtval, DATA + 1);
}

int
main(void)
{
    test_aligned_is_wide();
    test_misaligned_is_bytes();
    test_byte_access();
    test_trap_misaligned();

    if (failures) {
        printf("FAIL: %d check%s failed\n", failures,
               failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASS: rv32 bus access widths\n");
    return 0;
}
