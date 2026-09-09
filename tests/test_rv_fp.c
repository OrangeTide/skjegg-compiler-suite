/* test_rv_fp.c : the sign of a zero sum
 *
 * Adding two zeros is the one case in floating-point addition where the
 * result's sign is not implied by its value, and the rule is not
 * symmetric: like signs keep the sign, opposite signs give +0 in every
 * rounding mode except round-down, where they give -0.
 *
 * Swapping the two branches survives the whole rig. The unit suites
 * never add two zeros; the compliance suite does not distinguish the
 * sign of a zero sum; and the fuzzer can see it but needs an fadd.s
 * whose operands are both exactly -0, roughly one in 1600, so it takes
 * hundreds of rounds to reach one. Measured: the mutant survived 120
 * rounds and died at 600.
 *
 * The expected signs here were taken from qemu-riscv32 running the same
 * eight additions, so this checks the core against another
 * implementation rather than against its author's reading.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "rv32.h"

#include <stdio.h>
#include <string.h>

#define MEM_SIZE    0x4000
#define CODE_BASE   0x1000

#define POS_ZERO    0x00000000u
#define NEG_ZERO    0x80000000u

/* fadd.s f3, f1, f2, rne   0x002081d3
 * fadd.s f3, f1, f2, rdn   0x0020a1d3 */
#define I_FADD_RNE  0x002081d3u
#define I_FADD_RDN  0x0020a1d3u

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
    printf("FAIL %s: got %08x, want %08x\n", what, got, want);
    failures++;
}

/** f1 + f2 under the given encoding's rounding mode. */
static uint32_t
add(uint32_t insn, uint32_t a, uint32_t b)
{
    rv_cpu cpu;

    memset(mem, 0, sizeof mem);
    mem[CODE_BASE] = (uint8_t)insn;
    mem[CODE_BASE + 1] = (uint8_t)(insn >> 8);
    mem[CODE_BASE + 2] = (uint8_t)(insn >> 16);
    mem[CODE_BASE + 3] = (uint8_t)(insn >> 24);

    rv_init(&cpu, r8, r16, r32, w8, w16, w32, NULL);
    rv_reset(&cpu, CODE_BASE);
    cpu.f[1] = a;
    cpu.f[2] = b;
    rv_step(&cpu);
    return cpu.f[3];
}

/* Like signs keep the sign, in every mode. */
static void
test_like_signs(void)
{
    check("rne: +0 + +0 is +0", add(I_FADD_RNE, POS_ZERO, POS_ZERO),
          POS_ZERO);
    check("rne: -0 + -0 is -0", add(I_FADD_RNE, NEG_ZERO, NEG_ZERO),
          NEG_ZERO);
    check("rdn: +0 + +0 is +0", add(I_FADD_RDN, POS_ZERO, POS_ZERO),
          POS_ZERO);
    check("rdn: -0 + -0 is -0", add(I_FADD_RDN, NEG_ZERO, NEG_ZERO),
          NEG_ZERO);
}

/* Opposite signs give +0, except rounding down, which gives -0. The two
 * orders are checked because the rule is about the pair, not about which
 * operand carried the sign. */
static void
test_opposite_signs(void)
{
    check("rne: +0 + -0 is +0", add(I_FADD_RNE, POS_ZERO, NEG_ZERO),
          POS_ZERO);
    check("rne: -0 + +0 is +0", add(I_FADD_RNE, NEG_ZERO, POS_ZERO),
          POS_ZERO);
    check("rdn: +0 + -0 is -0", add(I_FADD_RDN, POS_ZERO, NEG_ZERO),
          NEG_ZERO);
    check("rdn: -0 + +0 is -0", add(I_FADD_RDN, NEG_ZERO, POS_ZERO),
          NEG_ZERO);
}

int
main(void)
{
    test_like_signs();
    test_opposite_signs();

    if (failures) {
        printf("FAIL: %d check%s failed\n", failures,
               failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASS: rv32 sign of a zero sum\n");
    return 0;
}
