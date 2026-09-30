/* test_rv_run.c : the rv_run() batch contract in the RV32 core
 *
 * A host-side test: it drives emu/rv32.c directly, so it needs no cross
 * toolchain and no qemu. It pins the reason rv_run() returns and the
 * retired count it reports, including that the loop stops early on a trap,
 * a wfi park, or a halt instead of spending the whole budget.
 */

#include "rv32.h"

#include <stdio.h>
#include <string.h>

#define MEM_SIZE    0x4000
#define CODE_BASE   0x1000

static uint8_t mem[MEM_SIZE];
static int failures;

/****************************************************************
 * Bus
 ****************************************************************/

static uint32_t
r8(void *ctx, uint32_t a)
{
    (void)ctx;
    return a < MEM_SIZE ? mem[a] : 0;
}

static uint32_t
r16(void *ctx, uint32_t a)
{
    (void)ctx;
    if (a + 1 >= MEM_SIZE)
        return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8);
}

static uint32_t
r32(void *ctx, uint32_t a)
{
    (void)ctx;
    if (a + 3 >= MEM_SIZE)
        return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8) |
           ((uint32_t)mem[a + 2] << 16) | ((uint32_t)mem[a + 3] << 24);
}

static void
w8(void *ctx, uint32_t a, uint32_t v)
{
    (void)ctx;
    if (a < MEM_SIZE)
        mem[a] = (uint8_t)v;
}

static void
w16(void *ctx, uint32_t a, uint32_t v)
{
    (void)ctx;
    if (a + 1 < MEM_SIZE) {
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
    }
}

static void
w32(void *ctx, uint32_t a, uint32_t v)
{
    (void)ctx;
    if (a + 3 < MEM_SIZE) {
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
        mem[a + 2] = (uint8_t)(v >> 16);
        mem[a + 3] = (uint8_t)(v >> 24);
    }
}

/****************************************************************
 * Guest programs (hand-encoded)
 *   addi x1, x1, 1   0x00108093
 *   jal  x0, -4      0xffdff06f    back one instruction
 *   ebreak           0x00100073
 *   wfi              0x10500073
 *   ecall            0x00000073
 ****************************************************************/

#define I_ADDI_X1   0x00108093u
#define I_JAL_BACK  0xffdff06fu
#define I_EBREAK    0x00100073u
#define I_WFI       0x10500073u
#define I_ECALL     0x00000073u

static void
poke(uint32_t addr, uint32_t insn)
{
    w32(NULL, addr, insn);
}

static void
setup(rv_cpu *cpu)
{
    rv_init(cpu, r8, r16, r32, w8, w16, w32, NULL);
    rv_reset(cpu, CODE_BASE);
}

/****************************************************************
 * Checks
 ****************************************************************/

static const char *
reason_name(enum rv_run_reason r)
{
    switch (r) {
    case RV_RUN_BUDGET: return "BUDGET";
    case RV_RUN_HALT:   return "HALT";
    case RV_RUN_TRAP:   return "TRAP";
    case RV_RUN_YIELD:  return "YIELD";
    }
    return "?";
}

static void
check_reason(const char *what, enum rv_run_reason got, enum rv_run_reason want)
{
    if (got == want)
        return;
    printf("FAIL %s: reason %s, want %s\n", what, reason_name(got),
           reason_name(want));
    failures++;
}

static void
check_int(const char *what, int got, int want)
{
    if (got == want)
        return;
    printf("FAIL %s: got %d, want %d\n", what, got, want);
    failures++;
}

/****************************************************************
 * A runnable batch that does not stop early spends the whole budget
 * and reports BUDGET, with *retired equal to the budget.
 ****************************************************************/

static void
test_budget(void)
{
    rv_cpu cpu;
    enum rv_run_reason r;
    int retired = -1;

    memset(mem, 0, sizeof(mem));
    poke(CODE_BASE + 0, I_ADDI_X1);     /* an infinite two-instruction loop */
    poke(CODE_BASE + 4, I_JAL_BACK);
    setup(&cpu);

    r = rv_run(&cpu, 6, &retired);
    check_reason("budget", r, RV_RUN_BUDGET);
    check_int("budget: retired", retired, 6);
    check_int("budget: not halted", cpu.halted, 0);
    check_int("budget: not in_trap", cpu.in_trap, 0);
    check_int("budget: not waiting", cpu.waiting, 0);
    check_int("budget: three laps counted x1", (int)cpu.x[1], 3);
}

/****************************************************************
 * A trap stops the batch on the faulting instruction: reason TRAP,
 * mcause set, and the rest of the budget unspent.
 ****************************************************************/

static void
test_trap(void)
{
    rv_cpu cpu;
    enum rv_run_reason r;
    int retired = -1;

    memset(mem, 0, sizeof(mem));
    poke(CODE_BASE + 0, I_ADDI_X1);
    poke(CODE_BASE + 4, I_EBREAK);      /* traps on the second instruction */
    poke(CODE_BASE + 8, I_ADDI_X1);     /* must not run */
    setup(&cpu);

    r = rv_run(&cpu, 10, &retired);
    check_reason("trap", r, RV_RUN_TRAP);
    check_int("trap: stopped early", retired, 2);
    check_int("trap: in_trap set", cpu.in_trap, 1);
    check_int("trap: mcause is breakpoint", (int)cpu.mcause,
              RV_CAUSE_BREAKPOINT);
    check_int("trap: the following insn did not run", (int)cpu.x[1], 1);
}

/****************************************************************
 * wfi parks the hart: reason YIELD, waiting set, batch unspent. The
 * hart is still runnable, so this is a yield and not a halt.
 ****************************************************************/

static void
test_yield(void)
{
    rv_cpu cpu;
    enum rv_run_reason r;
    int retired = -1;

    memset(mem, 0, sizeof(mem));
    poke(CODE_BASE + 0, I_ADDI_X1);
    poke(CODE_BASE + 4, I_WFI);         /* parks on the second instruction */
    poke(CODE_BASE + 8, I_ADDI_X1);     /* must not run */
    setup(&cpu);

    r = rv_run(&cpu, 10, &retired);
    check_reason("yield", r, RV_RUN_YIELD);
    check_int("yield: stopped early", retired, 2);
    check_int("yield: waiting set", cpu.waiting, 1);
    check_int("yield: not halted", cpu.halted, 0);
    check_int("yield: not in_trap", cpu.in_trap, 0);
    check_int("yield: the following insn did not run", (int)cpu.x[1], 1);
}

/****************************************************************
 * A hart halted before the call returns HALT at once, retiring nothing.
 ****************************************************************/

static void
test_halt_at_entry(void)
{
    rv_cpu cpu;
    enum rv_run_reason r;
    int retired = -1;

    memset(mem, 0, sizeof(mem));
    poke(CODE_BASE + 0, I_ADDI_X1);
    poke(CODE_BASE + 4, I_JAL_BACK);
    setup(&cpu);
    rv_halt(&cpu);

    r = rv_run(&cpu, 5, &retired);
    check_reason("halt-at-entry", r, RV_RUN_HALT);
    check_int("halt-at-entry: retired nothing", retired, 0);
}

/* An ecall handler that halts the hart, the way an exit syscall would. */
static int
ecall_halt(rv_cpu *cpu, void *ctx)
{
    (void)ctx;
    rv_halt(cpu);
    return 0;                           /* handled: the instruction retires */
}

/****************************************************************
 * A halt raised mid-batch (an exit syscall through the ecall handler)
 * stops the loop: reason HALT, the ecall counted, the rest unspent.
 ****************************************************************/

static void
test_halt_midbatch(void)
{
    rv_cpu cpu;
    enum rv_run_reason r;
    int retired = -1;

    memset(mem, 0, sizeof(mem));
    poke(CODE_BASE + 0, I_ADDI_X1);
    poke(CODE_BASE + 4, I_ADDI_X1);
    poke(CODE_BASE + 8, I_ECALL);       /* the handler halts here */
    poke(CODE_BASE + 12, I_ADDI_X1);    /* must not run */
    setup(&cpu);
    rv_set_ecall(&cpu, ecall_halt, NULL);

    r = rv_run(&cpu, 10, &retired);
    check_reason("halt-midbatch", r, RV_RUN_HALT);
    check_int("halt-midbatch: stopped after the ecall", retired, 3);
    check_int("halt-midbatch: halted", cpu.halted, 1);
    check_int("halt-midbatch: the following insn did not run", (int)cpu.x[1], 2);
}

/****************************************************************
 * A NULL retired pointer is accepted (the reason still comes back).
 ****************************************************************/

static void
test_null_retired(void)
{
    rv_cpu cpu;
    enum rv_run_reason r;

    memset(mem, 0, sizeof(mem));
    poke(CODE_BASE + 0, I_ADDI_X1);
    poke(CODE_BASE + 4, I_JAL_BACK);
    setup(&cpu);

    r = rv_run(&cpu, 3, NULL);
    check_reason("null-retired", r, RV_RUN_BUDGET);
}

int
main(void)
{
    test_budget();
    test_trap();
    test_yield();
    test_halt_at_entry();
    test_halt_midbatch();
    test_null_retired();

    if (failures) {
        printf("test_rv_run: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_rv_run: all rv_run() batch-contract checks passed\n");
    return 0;
}
