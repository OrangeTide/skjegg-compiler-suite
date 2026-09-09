/* test_rv_decode.c : frame sizes, and what must not decode at all
 *
 * Two things the rig had no check for, both found by mutation testing.
 *
 * The first is the Zcmp stack adjustment, which is a table lookup on the
 * register-list field. Moving one of its boundaries by one register goes
 * unnoticed by 73 dedicated Zcmp checks, because they use one list and
 * never compare the frame size against the architecture's table.
 *
 * The second is rejection. Every other test in the rig runs instructions
 * that are meant to work and checks that they do; nothing checks that a
 * malformed encoding traps. Two separate mutants widen a guard so that
 * encodings which must raise the illegal-instruction trap instead
 * execute as something, and both survive everything: a reserved field
 * left unchecked in fmv.w.x, and the gate on the Zcb expander, which
 * lets 208 otherwise-illegal encodings decode.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "rv32.h"

#include <stdio.h>
#include <string.h>

#define MEM_SIZE    0x8000
#define CODE_BASE   0x1000
#define HANDLER     0x2000

static uint8_t mem[MEM_SIZE];
static int failures;

static uint32_t
r8(void *c, uint32_t a)
{
    (void)c;
    return a < MEM_SIZE ? mem[a] : 0;
}

static uint32_t
r16(void *c, uint32_t a)
{
    (void)c;
    if (a + 1 >= MEM_SIZE)
        return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8);
}

static uint32_t
r32(void *c, uint32_t a)
{
    (void)c;
    if (a + 3 >= MEM_SIZE)
        return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8) |
           ((uint32_t)mem[a + 2] << 16) | ((uint32_t)mem[a + 3] << 24);
}

static void
w8(void *c, uint32_t a, uint32_t v)
{
    (void)c;
    if (a < MEM_SIZE)
        mem[a] = (uint8_t)v;
}

static void
w16(void *c, uint32_t a, uint32_t v)
{
    (void)c;
    if (a + 1 < MEM_SIZE) {
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
    }
}

static void
w32(void *c, uint32_t a, uint32_t v)
{
    (void)c;
    if (a + 3 < MEM_SIZE) {
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
        mem[a + 2] = (uint8_t)(v >> 16);
        mem[a + 3] = (uint8_t)(v >> 24);
    }
}

static void
check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want)
        return;
    printf("FAIL %s: got 0x%08x, want 0x%08x\n", what, got, want);
    failures++;
}

static void
boot(rv_cpu *cpu, const void *insn, int bytes)
{
    memset(mem, 0, sizeof mem);
    memcpy(mem + CODE_BASE, insn, (size_t)bytes);
    rv_init(cpu, r8, r16, r32, w8, w16, w32, NULL);
    rv_reset(cpu, CODE_BASE);
    cpu->mtvec = HANDLER;
}

/****************************************************************
 * The Zcmp stack adjustment
 *
 * cm.push builds a frame whose base size comes from the register list:
 * on RV32 lists 4-7 take 16 bytes, 8-11 take 32, 12-14 take 48 and 15
 * takes 64, plus 16 for each unit of the immediate.  A boundary off by
 * one register puts the saved registers in the wrong place.
 ****************************************************************/

static void
test_zcmp_frame(void)
{
    static const struct { unsigned rlist, base; } cases[] = {
        { 4, 16 }, { 5, 16 }, { 6, 16 }, { 7, 16 },
        { 8, 32 }, { 9, 32 }, { 10, 32 }, { 11, 32 },
        { 12, 48 }, { 13, 48 }, { 14, 48 },
        { 15, 64 },
    };
    unsigned i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        /* cm.push {ra, ...}, -base : 101 11000 rlist(4) spimm(2) 10 */
        uint16_t c = (uint16_t)(0xb800u | (cases[i].rlist << 4) | 2);
        rv_cpu cpu;
        char what[64];

        boot(&cpu, &c, 2);
        cpu.x[2] = 0x4000;
        rv_step(&cpu);
        sprintf(what, "cm.push rlist %u frame", cases[i].rlist);
        check(what, 0x4000u - cpu.x[2], cases[i].base);
    }
}

/****************************************************************
 * Encodings that must not decode
 ****************************************************************/

/** Run one encoding and report the trap cause, or 0 if none was taken. */
static uint32_t
cause_of(const void *insn, int bytes)
{
    rv_cpu cpu;

    boot(&cpu, insn, bytes);
    rv_step(&cpu);
    return cpu.in_trap ? cpu.mcause : 0;
}

static void
test_illegal_encodings(void)
{
    /* fmv.w.x with rs2 nonzero.  The field is reserved and must be zero;
     * an implementation that ignores it executes a malformed
     * instruction. (0xf00500d3 is the well-formed fmv.w.x ft1, a0.) */
    uint32_t good = 0xf00500d3u;
    uint32_t bad_rs2 = good | (1u << 20);
    uint32_t bad_f3 = good | (1u << 12);

    check("fmv.w.x well-formed decodes", cause_of(&good, 4), 0);
    check("fmv.w.x with rs2 set traps", cause_of(&bad_rs2, 4),
          RV_CAUSE_ILLEGAL_INSN);
    check("fmv.w.x with funct3 set traps", cause_of(&bad_f3, 4),
          RV_CAUSE_ILLEGAL_INSN);

    /* A compressed encoding in quadrant 1 that neither the base set nor
     * Zcb defines.  The Zcb expander is reached only when the base set
     * rejects an encoding, so a gate that is too generous there turns an
     * illegal instruction into whatever Zcb makes of it. */
    {
        uint16_t c = 0x9041u;
        check("undefined quadrant-1 encoding traps", cause_of(&c, 2),
              RV_CAUSE_ILLEGAL_INSN);
    }

    /* The all-zero halfword is the architecture's canonical illegal
     * instruction, and stays illegal whatever else is enabled. */
    {
        uint16_t c = 0x0000u;
        check("the zero halfword traps", cause_of(&c, 2),
              RV_CAUSE_ILLEGAL_INSN);
    }

    /* rev8 is one encoding in its group, fixed by both its funct3 and a
     * shift amount of 24; the rest of the group is reserved. A guard
     * that accepts the group on either half of that test alone turns 31
     * reserved encodings into rev8, and nothing else in the rig runs a
     * reserved encoding to see it trap.
     *
     *   0x6985d513 is rev8 a0, a1; these are the same group at shift
     *   amounts 0, 9 and 31. */
    {
        uint32_t sh0 = 0x6805d513u, sh9 = 0x6935d513u, sh31 = 0x69f5d513u;

        check("rev8 group, shift 0, traps", cause_of(&sh0, 4),
              RV_CAUSE_ILLEGAL_INSN);
        check("rev8 group, shift 9, traps", cause_of(&sh9, 4),
              RV_CAUSE_ILLEGAL_INSN);
        check("rev8 group, shift 31, traps", cause_of(&sh31, 4),
              RV_CAUSE_ILLEGAL_INSN);
    }
}

/* c.ebreak and c.jalr share an encoding, told apart only by whether the
 * register field is zero. Swapping that test exchanges the two, and
 * nothing notices, because instruction coverage reports c.ebreak as
 * executed by no method in the whole rig. This is the first thing to
 * run it.
 *
 *   c.ebreak  0x9002 expands to ebreak       0x00100073
 *   c.jalr ra 0x9082 expands to jalr ra,0(ra) 0x000080e7
 */
static void
test_ebreak_against_jalr(void)
{
    uint16_t ebreak = 0x9002u;
    rv_cpu cpu;

    check("c.ebreak expands to ebreak", rv_expand_c(0x9002u), 0x00100073u);
    check("c.jalr expands to jalr", rv_expand_c(0x9082u), 0x000080e7u);

    /* and it really does break: the trap, not just the expansion */
    boot(&cpu, &ebreak, 2);
    rv_step(&cpu);
    check("c.ebreak raises the breakpoint trap", cpu.mcause,
          RV_CAUSE_BREAKPOINT);
    check("c.ebreak reports its own address", cpu.mepc, CODE_BASE);
}

/* rev8 lives in the immediate-shift space and is told apart by both its
 * funct3 and its shift amount. A guard that accepts either alone claims
 * ordinary shifts: srli by anything other than 24, and any shift by 24.
 *
 *   rev8 a0, a1      0x6985d513
 *   srli a0, a1, 1   0x0015d513
 *   slli a0, a1, 24  0x01859513
 *   rori a0, a1, 24  0x6185d513
 */
static void
test_rev8_neighbours(void)
{
    static const struct { uint32_t insn; uint32_t want; const char *what; }
    cases[] = {
        { 0x6985d513u, 0xefcdab89u, "rev8 reverses the bytes" },
        { 0x0015d513u, 0x44d5e6f7u, "srli by 1 is a shift, not rev8" },
        { 0x01859513u, 0xef000000u, "slli by 24 is a shift, not rev8" },
        { 0x6185d513u, 0xabcdef89u, "rori by 24 is a rotate, not rev8" },
    };
    unsigned i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rv_cpu cpu;

        boot(&cpu, &cases[i].insn, 4);
        cpu.x[11] = 0x89abcdefu;
        rv_step(&cpu);
        check(cases[i].what, cpu.x[10], cases[i].want);
    }
}

int
main(void)
{
    test_zcmp_frame();
    test_illegal_encodings();
    test_ebreak_against_jalr();
    test_rev8_neighbours();

    if (failures) {
        printf("FAIL: %d check%s failed\n", failures,
               failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASS: rv32 frame sizes and illegal encodings\n");
    return 0;
}
