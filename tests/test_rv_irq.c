/* test_rv_irq.c : interrupt delivery in the RV32 core
 *
 * A host-side test: it drives emu/rv32.c directly, so it needs no cross
 * toolchain and no qemu.  The guest programs are a handful of encoded
 * instructions in a flat array, which keeps the whole machine (memory,
 * the interrupt lines, the CSRs) under the test's hand.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "rv32.h"

#include <stdio.h>
#include <string.h>

#define MEM_SIZE    0x4000
#define CODE_BASE   0x1000      /* the interrupted loop */
#define HANDLER     0x2000      /* the trap handler */

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
 * Guest programs
 *
 * Hand-encoded, since the point is to test the core rather than a
 * toolchain:
 *   addi x1, x1, 1   0x00108093    count laps of the loop
 *   addi x2, x2, 1   0x00110113    count handler entries
 *   jal  x0, -4      0xffdff06f    branch back one instruction
 *   mret             0x30200073
 *   wfi              0x10500073
 *   ebreak           0x00100073
 *   lw   x3, 0(x0)   0x00002183    a load, for the misaligned case
 ****************************************************************/

#define I_ADDI_X1   0x00108093u
#define I_ADDI_X2   0x00110113u
#define I_JAL_BACK  0xffdff06fu
#define I_MRET      0x30200073u
#define I_WFI       0x10500073u
#define I_EBREAK    0x00100073u

static void
poke(uint32_t addr, uint32_t insn)
{
    w32(NULL, addr, insn);
}

/* The interrupted program: increment x1 forever.  The handler:
 * increment x2 and return. */
static void
build(uint32_t loop_insn)
{
    memset(mem, 0, sizeof(mem));
    poke(CODE_BASE + 0, loop_insn);
    poke(CODE_BASE + 4, I_JAL_BACK);
    poke(HANDLER + 0, I_ADDI_X2);
    poke(HANDLER + 4, I_MRET);
}

static void
setup(rv_cpu *cpu, uint32_t mtvec, uint32_t mie, int global_enable)
{
    rv_init(cpu, r8, r16, r32, w8, w16, w32, NULL);
    rv_reset(cpu, CODE_BASE);
    cpu->mtvec = mtvec;
    cpu->mie = mie;
    if (global_enable)
        cpu->mstatus |= RV_MSTATUS_MIE;
    else
        cpu->mstatus &= ~RV_MSTATUS_MIE;
}

static void
run(rv_cpu *cpu, int n)
{
    int k;

    for (k = 0; k < n; k++)
        if (rv_step(cpu) < 0)
            return;
}

/****************************************************************
 * Checks
 ****************************************************************/

static void
check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want)
        return;
    printf("FAIL %s: got 0x%08x, want 0x%08x\n", what, got, want);
    failures++;
}

/* A raised, enabled timer interrupt is taken between instructions, and
 * mret resumes the interrupted instruction. */
static void
test_timer(void)
{
    rv_cpu cpu;

    build(I_ADDI_X1);
    setup(&cpu, HANDLER, RV_MIP_MTIP, 1);

    run(&cpu, 4);                       /* two laps of the loop */
    check("timer: no spurious entry", cpu.x[2], 0);

    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    check("timer: pending", rv_irq_pending(&cpu),
          RV_CAUSE_INTERRUPT | RV_IRQ_TIMER);

    rv_step(&cpu);                      /* takes the interrupt */
    check("timer: mcause", cpu.mcause, RV_CAUSE_INTERRUPT | RV_IRQ_TIMER);
    check("timer: mepc is the next instruction", cpu.mepc, CODE_BASE);
    check("timer: entered the handler", cpu.pc, HANDLER);
    check("timer: MIE cleared on entry", cpu.mstatus & RV_MSTATUS_MIE, 0);
    check("timer: MPIE records it was on",
          (cpu.mstatus & RV_MSTATUS_MPIE) != 0, 1);

    /* The line is still raised, so the handler has to lower it or be
     * re-entered.  Lower it the way a timer driver would. */
    rv_set_irq(&cpu, RV_IRQ_TIMER, 0);

    rv_step(&cpu);                      /* addi x2, x2, 1 */
    rv_step(&cpu);                      /* mret */
    check("timer: handler ran once", cpu.x[2], 1);
    check("timer: mret resumed the interrupted pc", cpu.pc, CODE_BASE);
    check("timer: mret restored MIE",
          (cpu.mstatus & RV_MSTATUS_MIE) != 0, 1);

    run(&cpu, 4);
    check("timer: no second entry", cpu.x[2], 1);
}

/* Either gate closed means nothing is taken. */
static void
test_masking(void)
{
    rv_cpu cpu;

    build(I_ADDI_X1);
    setup(&cpu, HANDLER, RV_MIP_MTIP, 0);       /* mstatus.MIE clear */
    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    check("masking: mstatus gate", rv_irq_pending(&cpu), 0);
    run(&cpu, 6);
    check("masking: mstatus gate held", cpu.x[2], 0);

    build(I_ADDI_X1);
    setup(&cpu, HANDLER, 0, 1);                 /* mie bit clear */
    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    check("masking: mie gate", rv_irq_pending(&cpu), 0);
    run(&cpu, 6);
    check("masking: mie gate held", cpu.x[2], 0);

    /* Enabling the bit later takes the interrupt that was already
     * raised: the line is a level, not an edge. */
    cpu.mie = RV_MIP_MTIP;
    rv_step(&cpu);
    check("masking: level survives until enabled", cpu.pc, HANDLER);
}

/* External beats software beats timer. */
static void
test_priority(void)
{
    rv_cpu cpu;

    build(I_ADDI_X1);
    setup(&cpu, HANDLER, RV_MIP_MTIP | RV_MIP_MSIP | RV_MIP_MEIP, 1);

    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    rv_set_irq(&cpu, RV_IRQ_SOFT, 1);
    rv_set_irq(&cpu, RV_IRQ_EXT, 1);
    check("priority: external first", rv_irq_pending(&cpu),
          RV_CAUSE_INTERRUPT | RV_IRQ_EXT);

    rv_set_irq(&cpu, RV_IRQ_EXT, 0);
    check("priority: then software", rv_irq_pending(&cpu),
          RV_CAUSE_INTERRUPT | RV_IRQ_SOFT);

    rv_set_irq(&cpu, RV_IRQ_SOFT, 0);
    check("priority: then timer", rv_irq_pending(&cpu),
          RV_CAUSE_INTERRUPT | RV_IRQ_TIMER);
}

/* Vectored mtvec spreads the interrupts and leaves exceptions at base. */
static void
test_vectored(void)
{
    rv_cpu cpu;

    build(I_ADDI_X1);
    setup(&cpu, HANDLER | 1, RV_MIP_MTIP, 1);
    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    rv_step(&cpu);
    check("vectored: interrupt lands at base + 4*cause", cpu.pc,
          HANDLER + 4 * RV_IRQ_TIMER);

    /* An exception in the same mode goes to base itself. */
    build(0);                           /* an all-zero word is illegal */
    setup(&cpu, HANDLER | 1, RV_MIP_MTIP, 1);
    rv_step(&cpu);
    check("vectored: exception lands at base", cpu.pc, HANDLER);
    check("vectored: exception cause", cpu.mcause, RV_CAUSE_ILLEGAL_INSN);
}

/* wfi is a nop that records the wait, and an interrupt ends it. */
static void
test_wfi(void)
{
    rv_cpu cpu;

    build(I_WFI);
    setup(&cpu, HANDLER, RV_MIP_MTIP, 1);

    rv_step(&cpu);
    check("wfi: waiting", cpu.waiting != 0, 1);
    check("wfi: ran on", cpu.pc, CODE_BASE + 4);

    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    rv_step(&cpu);
    check("wfi: the interrupt ends the wait", cpu.waiting, 0);
    check("wfi: entered the handler", cpu.pc, HANDLER);
}

/* mret sets MIE from MPIE, which means it can leave interrupts off.
 * Every other test here returns from a handler that was entered with
 * interrupts on, so MPIE is set and mret turns MIE back on; none of them
 * would notice an mret that simply always enabled it. A guest that traps
 * with interrupts already disabled and returns is the case that does.
 * (Found by mutation testing: `!(mstatus & MPIE)` mutated to `|` makes
 * the clearing branch unreachable and survived both this suite and the
 * imported one.) */
static void
test_mret_mpie_clear(void)
{
    rv_cpu cpu;

    build(I_ADDI_X1);
    poke(CODE_BASE, I_MRET);
    setup(&cpu, HANDLER, RV_MIP_MTIP, 0);       /* MIE clear */
    cpu.mstatus &= ~RV_MSTATUS_MPIE;            /* and MPIE clear */
    cpu.mepc = HANDLER;

    rv_step(&cpu);                              /* mret */
    check("mret: resumed at mepc", cpu.pc, HANDLER);
    check("mret: MIE stays clear when MPIE was clear",
          cpu.mstatus & RV_MSTATUS_MIE, 0);
    check("mret: MPIE is set on return",
          (cpu.mstatus & RV_MSTATUS_MPIE) != 0, 1);

    /* and with MPIE set it does turn interrupts back on */
    build(I_ADDI_X1);
    poke(CODE_BASE, I_MRET);
    setup(&cpu, HANDLER, RV_MIP_MTIP, 0);
    cpu.mstatus |= RV_MSTATUS_MPIE;
    cpu.mepc = HANDLER;

    rv_step(&cpu);
    check("mret: MIE restored when MPIE was set",
          (cpu.mstatus & RV_MSTATUS_MIE) != 0, 1);
}

/* A handler that returns with the line still raised is re-entered, the
 * way a real level-sensitive line behaves. */
static void
test_level_reentry(void)
{
    rv_cpu cpu;

    build(I_ADDI_X1);
    setup(&cpu, HANDLER, RV_MIP_MTIP, 1);
    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);

    run(&cpu, 9);       /* trap, addi, mret, trap, addi, mret, ... */
    check("level: re-entered while raised", cpu.x[2] >= 2, 1);
    check("level: the loop never advanced", cpu.x[1], 0);
}

int
main(void)
{
    test_timer();
    test_masking();
    test_priority();
    test_vectored();
    test_wfi();
    test_mret_mpie_clear();
    test_level_reentry();

    if (failures) {
        printf("FAIL: %d check%s failed\n", failures,
               failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASS: rv32 interrupt delivery\n");
    return 0;
}
