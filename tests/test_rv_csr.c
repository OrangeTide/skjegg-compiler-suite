/* test_rv_csr.c : the zero-source rules of the CSR instructions
 *
 * The CSR instructions carry a rule that is easy to state and easy to
 * get subtly wrong: whether the write happens at all depends on the
 * form and on whether the source field is zero.
 *
 *   csrrw / csrrwi   always write, including when the source is x0 or
 *                    an immediate of 0, which write a zero
 *   csrrs / csrrc    with rs1 = x0, and their immediate forms with a
 *   csrrsi / csrrci  zero immediate, perform no write at all, which is
 *                    what makes them safe to use on a read-only CSR
 *
 * Confusing the two directions survives the whole rig, because the
 * only difference is whether a register nobody looks at afterwards was
 * written. Mutation testing found it: turning `(f3 & 3) != 1` into
 * `(f3 | 3) != 1` makes the no-write case apply to every form, so
 * `csrrw csr, x0` silently stops writing.
 *
 * Instruction coverage reports csrrc, csrrci and csrrsi as executed by
 * no method in the rig at all, so this is also the first thing to run
 * them.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "rv32.h"

#include <stdio.h>
#include <string.h>

#define MEM_SIZE    0x4000
#define CODE_BASE   0x1000

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

/****************************************************************
 * Guest instructions, as an assembler encodes them.  fcsr is the CSR
 * throughout: it is writable, it is eight bits wide, and it exists on
 * a machine with F.
 *
 *   csrrw  a0, fcsr, zero   0x00301573
 *   csrrs  a0, fcsr, zero   0x00302573
 *   csrrc  a0, fcsr, zero   0x00303573
 *   csrrwi a0, fcsr, 0      0x00305573
 *   csrrsi a0, fcsr, 0      0x00306573
 *   csrrci a0, fcsr, 0      0x00307573
 *   csrrw  a0, fcsr, a1     0x00359573
 *   csrrs  a0, fcsr, a1     0x0035a573
 *   csrrci a0, fcsr, 3      0x0031f573
 ****************************************************************/

#define I_CSRRW_X0      0x00301573u
#define I_CSRRS_X0      0x00302573u
#define I_CSRRC_X0      0x00303573u
#define I_CSRRWI_0      0x00305573u
#define I_CSRRSI_0      0x00306573u
#define I_CSRRCI_0      0x00307573u
#define I_CSRRW_A1      0x00359573u
#define I_CSRRS_A1      0x0035a573u
#define I_CSRRCI_3      0x0031f573u

static void
check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want)
        return;
    printf("FAIL %s: got 0x%08x, want 0x%08x\n", what, got, want);
    failures++;
}

/** Run one CSR instruction with fcsr preloaded, and a1 as its source.
 * mcycle is seeded too, so the counter tests have a known value in both
 * halves to check against. */
static void
run_one(rv_cpu *cpu, uint32_t insn, uint32_t fcsr, uint32_t a1)
{
    memset(mem, 0, sizeof mem);
    w32(NULL, CODE_BASE, insn);

    rv_init(cpu, r8, r16, r32, w8, w16, w32, NULL);
    rv_reset(cpu, CODE_BASE);
    cpu->fcsr = fcsr;
    cpu->x[11] = a1;
    cpu->mcycle = 0x1122334455667788ull;
    rv_step(cpu);
}

/* The write forms write even when the source is zero. */
static void
test_write_forms_always_write(void)
{
    rv_cpu cpu;

    run_one(&cpu, I_CSRRW_X0, 0xff, 0);
    check("csrrw x0: read the old value", cpu.x[10], 0xff);
    check("csrrw x0: wrote zero", cpu.fcsr, 0);

    run_one(&cpu, I_CSRRWI_0, 0xff, 0);
    check("csrrwi 0: read the old value", cpu.x[10], 0xff);
    check("csrrwi 0: wrote zero", cpu.fcsr, 0);

    run_one(&cpu, I_CSRRW_A1, 0x0f, 0x21);
    check("csrrw a1: read the old value", cpu.x[10], 0x0f);
    check("csrrw a1: wrote the new one", cpu.fcsr, 0x21);
}

/* The set and clear forms with a zero source perform no write. */
static void
test_zero_source_does_not_write(void)
{
    rv_cpu cpu;

    run_one(&cpu, I_CSRRS_X0, 0x1f, 0);
    check("csrrs x0: read", cpu.x[10], 0x1f);
    check("csrrs x0: left the csr alone", cpu.fcsr, 0x1f);

    run_one(&cpu, I_CSRRC_X0, 0x1f, 0);
    check("csrrc x0: read", cpu.x[10], 0x1f);
    check("csrrc x0: left the csr alone", cpu.fcsr, 0x1f);

    run_one(&cpu, I_CSRRSI_0, 0x1f, 0);
    check("csrrsi 0: read", cpu.x[10], 0x1f);
    check("csrrsi 0: left the csr alone", cpu.fcsr, 0x1f);

    run_one(&cpu, I_CSRRCI_0, 0x1f, 0);
    check("csrrci 0: read", cpu.x[10], 0x1f);
    check("csrrci 0: left the csr alone", cpu.fcsr, 0x1f);
}

/* With a nonzero source they do write, so the tests above are about the
 * zero case and not about the instructions being inert. */
static void
test_nonzero_source_writes(void)
{
    rv_cpu cpu;

    run_one(&cpu, I_CSRRS_A1, 0x01, 0x20);
    check("csrrs a1: read the old value", cpu.x[10], 0x01);
    check("csrrs a1: set the bits", cpu.fcsr, 0x21);

    run_one(&cpu, I_CSRRCI_3, 0x1f, 0);
    check("csrrci 3: read the old value", cpu.x[10], 0x1f);
    check("csrrci 3: cleared the low two bits", cpu.fcsr, 0x1c);
}

/* fflags and frm are windows onto fcsr rather than registers of their
 * own, so each has to select its own field out of it.  Reading the
 * wrong field, or failing to mask, survives the rig too: nothing else
 * reads either one.
 *
 *   csrrs a0, frm, zero      0x00202573
 *   csrrs a0, fflags, zero   0x00102573
 */
static void
test_fcsr_windows(void)
{
    rv_cpu cpu;

    /* frm = 1 (round toward zero), fflags = 0x0f */
    run_one(&cpu, 0x00202573u, 0x2f, 0);
    check("frm: the rounding-mode field only", cpu.x[10], 1);

    run_one(&cpu, 0x00102573u, 0x2f, 0);
    check("fflags: the exception field only", cpu.x[10], 0x0f);

    /* frm = 7, the reserved dynamic encoding, still reads back whole */
    run_one(&cpu, 0x00202573u, 0xe0, 0);
    check("frm: reads all three bits", cpu.x[10], 7);

    run_one(&cpu, 0x00102573u, 0xe0, 0);
    check("fflags: zero when no exception is set", cpu.x[10], 0);
}

/* Which forms read is a separate rule from which forms write, and the
 * checks above pin only the writes because every one of them uses a
 * real destination register. A csrrs whose destination is x0 must still
 * read: the value it sets bits into is the register's own. Only a
 * csrrw, which discards the old value anyway, may skip the read.
 *
 *   csrrs zero, fcsr, a1    0x0035a073
 *   csrrc zero, fcsr, a1    0x0035b073
 *   csrrw zero, fcsr, a1    0x00359073
 */
static void
test_x0_destination_still_reads(void)
{
    rv_cpu cpu;

    run_one(&cpu, 0x0035a073u, 0x40, 0x21);
    check("csrrs to x0 sets bits in the old value", cpu.fcsr, 0x61);

    run_one(&cpu, 0x0035b073u, 0x61, 0x21);
    check("csrrc to x0 clears bits from the old value", cpu.fcsr, 0x40);

    run_one(&cpu, 0x00359073u, 0x40, 0x21);
    check("csrrw to x0 replaces it", cpu.fcsr, 0x21);
}

/* mcycle is a 64-bit counter reached through two 32-bit windows, so
 * each write has to leave the other half alone. The counter advances as
 * the writing instruction retires, which is why the half that is not
 * under test is allowed to be one larger.
 *
 *   csrrw zero, mcycle,  a1   0xb0059073
 *   csrrw zero, mcycleh, a1   0xb8059073
 */
static void
test_mcycle_halves(void)
{
    rv_cpu cpu;
    uint32_t lo;

    run_one(&cpu, 0xb0059073u, 0, 0xdeadbeefu);
    check("mcycle write leaves the high half", (uint32_t)(cpu.mcycle >> 32),
          0x11223344u);

    run_one(&cpu, 0xb8059073u, 0, 0xdeadbeefu);
    check("mcycleh write sets the high half", (uint32_t)(cpu.mcycle >> 32),
          0xdeadbeefu);
    lo = (uint32_t)cpu.mcycle;
    check("mcycleh write leaves the low half", lo | 1u, 0x55667789u);
}

int
main(void)
{
    test_write_forms_always_write();
    test_zero_source_does_not_write();
    test_nonzero_source_writes();
    test_fcsr_windows();
    test_x0_destination_still_reads();
    test_mcycle_halves();

    if (failures) {
        printf("FAIL: %d check%s failed\n", failures,
               failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASS: rv32 csr zero-source rules\n");
    return 0;
}
