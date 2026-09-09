/* test_irq.c : interrupt delivery */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* Ported from the skjegg project, which adopted this interpreter and then
 * added the interrupt seam it was missing.
 *
 * This is the first part of the machine no reference model can check.
 * lockstep.c compares against qemu-riscv32, which runs in user mode and
 * has no interrupts at all; the compliance suite never raises one; and the
 * fuzzer generates arithmetic on a hart nothing can interrupt. So the
 * expected values here come from the specification and the tests are
 * checked by mutation instead: break the priority order, restore the old
 * vectoring, make a line edge-triggered, and each one has to fail.
 *
 * The guest programs are hand-encoded, because the point is to test the
 * core rather than a toolchain. */

#include <stdio.h>
#include <stdlib.h>
#include "rv32.h"
#include "machine.h"

#define CODE_BASE   0x1000      /* the interrupted loop */
#define HANDLER     0x2000      /* the trap handler */

static rv_cpu cpu;
static machine mach;
static int tests, failures;

/****************************************************************
 * Guest programs
 *
 *   addi x1, x1, 1   0x00108093    count laps of the loop
 *   addi x2, x2, 1   0x00110113    count handler entries
 *   jal  x0, -4      0xffdff06f    branch back one instruction
 *   mret             0x30200073
 *   wfi              0x10500073
 ****************************************************************/

#define I_ADDI_X1   0x00108093u
#define I_ADDI_X2   0x00110113u
#define I_JAL_BACK  0xffdff06fu
#define I_MRET      0x30200073u
#define I_WFI       0x10500073u

static void
poke(uint32_t addr, uint32_t insn)
{
    mach.mem[addr + 0] = (uint8_t)insn;
    mach.mem[addr + 1] = (uint8_t)(insn >> 8);
    mach.mem[addr + 2] = (uint8_t)(insn >> 16);
    mach.mem[addr + 3] = (uint8_t)(insn >> 24);
}

/* The interrupted program increments x1 forever. The handler increments
 * x2 and returns. */
static void
build(uint32_t loop_insn)
{
    poke(CODE_BASE + 0, loop_insn);
    poke(CODE_BASE + 4, I_JAL_BACK);
    poke(HANDLER + 0, I_ADDI_X2);
    poke(HANDLER + 4, I_MRET);
}

static void
setup(uint32_t mtvec, uint32_t mie, int global_enable)
{
    rv_reset(&cpu, CODE_BASE);
    cpu.mtvec = mtvec;
    cpu.mie = mie;
    if (global_enable)
        cpu.mstatus |= RV_MSTATUS_MIE;
    else
        cpu.mstatus &= ~RV_MSTATUS_MIE;
}

static void
run(int n)
{
    int k;

    for (k = 0; k < n; k++)
        if (rv_step(&cpu) < 0)
            return;
}

static void
check(const char *what, uint32_t got, uint32_t want)
{
    tests++;
    if (got == want) {
        printf("  ok   %-44s %08x\n", what, got);
        return;
    }
    failures++;
    printf("  FAIL %-44s got %08x want %08x\n", what, got, want);
}

/****************************************************************
 * Delivery
 ****************************************************************/

/* A raised, enabled timer interrupt is taken between instructions, and
 * mret resumes the instruction that had not run. */
static void
test_timer(void)
{
    printf("\na timer interrupt, taken and returned from\n");
    build(I_ADDI_X1);
    setup(HANDLER, RV_MIP_MTIP, 1);

    run(4);                             /* two laps of the loop */
    check("no entry while no line is raised", cpu.x[2], 0);

    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    check("pending reports the cause", rv_irq_pending(&cpu),
          RV_CAUSE_INTERRUPT | RV_IRQ_TIMER);

    rv_step(&cpu);                      /* takes the interrupt */
    check("mcause", cpu.mcause, RV_CAUSE_INTERRUPT | RV_IRQ_TIMER);
    check("mepc is the instruction that did not run", cpu.mepc, CODE_BASE);
    check("entered the handler", cpu.pc, HANDLER);
    check("MIE cleared on entry", cpu.mstatus & RV_MSTATUS_MIE, 0);
    check("MPIE records that it was on",
          (cpu.mstatus & RV_MSTATUS_MPIE) != 0, 1);

    /* The line is still raised, so the handler must lower it or be
     * re-entered. Lower it the way a timer driver would. */
    rv_set_irq(&cpu, RV_IRQ_TIMER, 0);

    rv_step(&cpu);                      /* addi x2, x2, 1 */
    rv_step(&cpu);                      /* mret */
    check("the handler ran once", cpu.x[2], 1);
    check("mret resumed the interrupted pc", cpu.pc, CODE_BASE);
    check("mret restored MIE", (cpu.mstatus & RV_MSTATUS_MIE) != 0, 1);

    run(4);
    check("no second entry once the line is low", cpu.x[2], 1);
}

/* Nothing is delivered unless both gates are open. */
static void
test_masking(void)
{
    printf("\nboth gates, and a level that waits for them\n");

    build(I_ADDI_X1);
    setup(HANDLER, RV_MIP_MTIP, 0);             /* mstatus.MIE clear */
    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    check("mstatus.MIE gates it", rv_irq_pending(&cpu), 0);
    run(6);
    check("  and keeps gating it", cpu.x[2], 0);

    build(I_ADDI_X1);
    setup(HANDLER, 0, 1);                       /* the mie bit is clear */
    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    check("the mie bit gates it", rv_irq_pending(&cpu), 0);
    run(6);
    check("  and keeps gating it", cpu.x[2], 0);

    /* Enabling the bit later takes the interrupt that was already
     * raised, because a line is a level and not an edge. */
    cpu.mie = RV_MIP_MTIP;
    rv_step(&cpu);
    check("the level survives until it is enabled", cpu.pc, HANDLER);
}

/* External beats software beats timer. */
static void
test_priority(void)
{
    printf("\npriority\n");
    build(I_ADDI_X1);
    setup(HANDLER, RV_MIP_MTIP | RV_MIP_MSIP | RV_MIP_MEIP, 1);

    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    rv_set_irq(&cpu, RV_IRQ_SOFT, 1);
    rv_set_irq(&cpu, RV_IRQ_EXT, 1);
    check("external first", rv_irq_pending(&cpu),
          RV_CAUSE_INTERRUPT | RV_IRQ_EXT);

    rv_set_irq(&cpu, RV_IRQ_EXT, 0);
    check("then software", rv_irq_pending(&cpu),
          RV_CAUSE_INTERRUPT | RV_IRQ_SOFT);

    rv_set_irq(&cpu, RV_IRQ_SOFT, 0);
    check("then timer", rv_irq_pending(&cpu),
          RV_CAUSE_INTERRUPT | RV_IRQ_TIMER);
}

/* Vectored mtvec spreads the interrupts and leaves exceptions at base.
 * The emulator vectored both for as long as it has existed, and every
 * method used to verify it missed that, because none of them ever set
 * mtvec to vectored mode. */
static void
test_vectored(void)
{
    printf("\nvectored mode applies to interrupts only\n");

    build(I_ADDI_X1);
    setup(HANDLER | 1, RV_MIP_MTIP, 1);
    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    rv_step(&cpu);
    check("an interrupt lands at base + 4*cause", cpu.pc,
          HANDLER + 4 * RV_IRQ_TIMER);

    /* An all-zero word is a defined illegal instruction */
    build(0);
    setup(HANDLER | 1, RV_MIP_MTIP, 1);
    rv_step(&cpu);
    check("an exception lands at base itself", cpu.pc, HANDLER);
    check("  with the exception's cause", cpu.mcause, RV_CAUSE_ILLEGAL_INSN);
}

/* wfi is a nop that records the wait, and an interrupt ends it. */
static void
test_wfi(void)
{
    printf("\nwfi\n");
    build(I_WFI);
    setup(HANDLER, RV_MIP_MTIP, 1);

    rv_step(&cpu);
    check("records that the guest is waiting", cpu.waiting != 0, 1);
    check("but runs on, since a nop is legal", cpu.pc, CODE_BASE + 4);

    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    rv_step(&cpu);
    check("an interrupt ends the wait", cpu.waiting, 0);
    check("and enters the handler", cpu.pc, HANDLER);
}

/* A handler that returns with the line still raised is re-entered, the
 * way a real level-sensitive line behaves. A guest that forgets to
 * acknowledge livelocks in its handler exactly as it would on silicon. */
static void
test_level_reentry(void)
{
    printf("\na line left raised re-enters the handler\n");
    build(I_ADDI_X1);
    setup(HANDLER, RV_MIP_MTIP, 1);
    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);

    run(9);                     /* trap, addi, mret, trap, addi, mret, ... */
    check("re-entered while raised", cpu.x[2] >= 2, 1);
    check("the interrupted loop never advanced", cpu.x[1], 0);
}

/* Taking an interrupt drops any outstanding reservation, so a store
 * conditional that straddles one fails and the guest retries. The rule
 * was already in rv_trap, but until there were interrupts the only way to
 * reach it was an exception, which a guest between lr.w and sc.w does not
 * normally take. */
static void
test_reservation_dropped(void)
{
    /* lr.w x3, (x4)   0x1002a1af   reserve
     * sc.w x5, x6, (x4) 0x1862a2af  store conditional
     * The handler and the loop are as before. */
    static const uint32_t I_LR = 0x100221afu;
    static const uint32_t I_SC = 0x186222afu;

    printf("\nan interrupt between lr.w and sc.w drops the reservation\n");

    /* Without an interrupt the pair succeeds */
    build(I_ADDI_X1);
    poke(CODE_BASE + 0, I_LR);
    poke(CODE_BASE + 4, I_SC);
    setup(HANDLER, RV_MIP_MTIP, 1);
    cpu.x[4] = 0x3000;
    cpu.x[6] = 0xabcd;
    rv_step(&cpu);
    rv_step(&cpu);
    check("the pair succeeds when nothing interrupts it", cpu.x[5], 0);

    /* With one taken in between, the store conditional fails */
    build(I_ADDI_X1);
    poke(CODE_BASE + 0, I_LR);
    poke(CODE_BASE + 4, I_SC);
    setup(HANDLER, RV_MIP_MTIP, 1);
    cpu.x[4] = 0x3000;
    cpu.x[6] = 0xabcd;
    rv_step(&cpu);                      /* lr.w reserves */
    rv_set_irq(&cpu, RV_IRQ_TIMER, 1);
    rv_step(&cpu);                      /* the interrupt is taken */
    check("the interrupt was taken", cpu.pc, HANDLER);
    rv_set_irq(&cpu, RV_IRQ_TIMER, 0);
    rv_step(&cpu);                      /* addi x2, x2, 1 */
    rv_step(&cpu);                      /* mret, back to the sc.w */
    check("mret resumed at the store conditional", cpu.pc, CODE_BASE + 4);
    rv_step(&cpu);
    check("the store conditional failed", cpu.x[5], 1);
}

int
main(void)
{
    if (machine_init(&mach, &cpu))
        return 1;
    mach.quiet = 1;

    printf("interrupt delivery tests\n");

    test_timer();
    test_masking();
    test_priority();
    test_vectored();
    test_wfi();
    test_level_reentry();
    test_reservation_dropped();

    printf("\n%d tests, %d failures\n", tests, failures);
    machine_free(&mach);
    return failures ? 1 : 0;
}
