/* test_mips_sys.c : the MIPS system (privileged) tier
 *
 * A host-side test: it drives emu/mips32.c directly with system_mode set, so
 * it needs no cross toolchain and no qemu.  It is the minimal privileged
 * runner: a flat memory, a handful of hand-encoded instructions, a guest
 * exception handler, and the external interrupt lines, all under the test's
 * hand.  The sibling of tests/test_rv_irq.c for the RV32 core.
 *
 * The bus folds the kseg virtual addresses down to physical ones (addr &
 * 0x1FFFFFFF) the way a real MIPS board does, which is why the 0x80000080
 * exception vector lands in this flat array: the core leaves address mapping
 * to the bus, so the vector is just another bus address.
 */

#include "mips32.h"

#include <stdio.h>
#include <string.h>

#define MEM_SIZE    0x4000
#define CODE_BASE   0x1000          /* the interrupted / faulting program */
#define VEC         0x80000080u     /* general exception vector (BEV = 0) */

static uint8_t mem[MEM_SIZE];
static int failures;

/****************************************************************
 * Bus: physical = virtual & 0x1FFFFFFF, then index the flat array
 ****************************************************************/

static uint32_t
phys(uint32_t a)
{
    return a & 0x1FFFFFFFu;
}

static uint32_t
r8(void *ctx, uint32_t a)
{
    (void)ctx;
    a = phys(a);
    return a < MEM_SIZE ? mem[a] : 0;
}

static uint32_t
r16(void *ctx, uint32_t a)
{
    (void)ctx;
    a = phys(a);
    if (a + 1 >= MEM_SIZE)
        return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8);
}

static uint32_t
r32(void *ctx, uint32_t a)
{
    (void)ctx;
    a = phys(a);
    if (a + 3 >= MEM_SIZE)
        return 0;
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8) |
           ((uint32_t)mem[a + 2] << 16) | ((uint32_t)mem[a + 3] << 24);
}

static void
w8(void *ctx, uint32_t a, uint32_t v)
{
    (void)ctx;
    a = phys(a);
    if (a < MEM_SIZE)
        mem[a] = (uint8_t)v;
}

static void
w16(void *ctx, uint32_t a, uint32_t v)
{
    (void)ctx;
    a = phys(a);
    if (a + 1 < MEM_SIZE) {
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
    }
}

static void
w32(void *ctx, uint32_t a, uint32_t v)
{
    (void)ctx;
    a = phys(a);
    if (a + 3 < MEM_SIZE) {
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
        mem[a + 2] = (uint8_t)(v >> 16);
        mem[a + 3] = (uint8_t)(v >> 24);
    }
}

/****************************************************************
 * Hand-encoded instructions
 ****************************************************************/

#define I_NOP          0x00000000u   /* sll $0,$0,0 */
#define I_ADDIU_1_1_1  0x24210001u   /* addiu $1,$1,1  : count loop laps  */
#define I_ADDIU_2_2_1  0x24420001u   /* addiu $2,$2,1  : count handler runs */
#define I_J_CODE       0x08000400u   /* j CODE_BASE (0x1000 >> 2 = 0x400) */
#define I_SYSCALL      0x0000000Cu   /* syscall */
#define I_BREAK        0x0000000Du   /* break */
#define I_ADD_4_3_3    0x00632020u   /* add $4,$3,$3   : overflow when $3 big */
#define I_LW_5_1_0     0x8C050001u   /* lw $5,1($0)    : misaligned load */
#define I_RESERVED     0xFC000000u   /* opcode 0x3F, unused */

/* Exception-handler building blocks. */
#define I_MFC0_K0_EPC  0x401A7000u   /* mfc0 $k0,$14 (EPC) */
#define I_ADDIU_K0_4   0x275A0004u   /* addiu $k0,$k0,4 */
#define I_JR_K0        0x03400008u   /* jr $k0 */
#define I_RFE          0x42000010u   /* rfe */

static void
poke(uint32_t vaddr, uint32_t insn)
{
    w32(NULL, vaddr, insn);
}

/****************************************************************
 * Setup
 ****************************************************************/

static void
setup(mips_cpu *cpu)
{
    memset(mem, 0, sizeof(mem));
    mips_init(cpu, r8, r16, r32, w8, w16, w32, NULL);
    cpu->system_mode = 1;
    mips_reset(cpu, CODE_BASE);
}

/* A handler that bumps $2 and returns to EPC (interrupts, where EPC already
 * names the instruction to resume). */
static void
handler_resume_epc(void)
{
    poke(VEC + 0, I_ADDIU_2_2_1);
    poke(VEC + 4, I_MFC0_K0_EPC);
    poke(VEC + 8, I_JR_K0);
    poke(VEC + 12, I_RFE);           /* delay slot of jr */
}

/* A handler that returns to EPC + 4 (synchronous traps like syscall, where EPC
 * names the trapping instruction and re-running it would loop). */
static void
handler_skip_epc(void)
{
    poke(VEC + 0, I_MFC0_K0_EPC);
    poke(VEC + 4, I_ADDIU_K0_4);
    poke(VEC + 8, I_JR_K0);
    poke(VEC + 12, I_RFE);           /* delay slot of jr */
}

static void
step(mips_cpu *cpu, int n)
{
    int k;

    for (k = 0; k < n; k++)
        mips_step(cpu);
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

static uint32_t
exc_code(const mips_cpu *cpu)
{
    return (cpu->cp0[MIPS_CP0_CAUSE] & MIPS_CAUSE_EXC_MASK)
           >> MIPS_CAUSE_EXC_SHIFT;
}

/****************************************************************
 * Tests
 ****************************************************************/

/* SYSCALL vectors through CP0: EPC names the syscall, Cause carries the code,
 * the SR stack is pushed, and the PC enters the handler.  RFE pops the stack
 * and the handler returns past the syscall. */
static void
test_syscall(void)
{
    mips_cpu cpu;

    setup(&cpu);
    handler_skip_epc();
    cpu.cp0[MIPS_CP0_SR] = MIPS_SR_IEc | MIPS_SR_KUc;   /* user, ints on */
    poke(CODE_BASE + 0, I_SYSCALL);
    poke(CODE_BASE + 4, I_ADDIU_1_1_1);                 /* runs after return */

    mips_step(&cpu);                                    /* the syscall */
    check("syscall: ExcCode", exc_code(&cpu), MIPS_EXC_SYSCALL);
    check("syscall: EPC is the syscall", cpu.cp0[MIPS_CP0_EPC], CODE_BASE);
    check("syscall: no BD", cpu.cp0[MIPS_CP0_CAUSE] & MIPS_CAUSE_BD, 0);
    check("syscall: vectored", cpu.pc, VEC);
    /* stack pushed: current pair cleared, previous = the old current */
    check("syscall: IEc cleared on entry", cpu.cp0[MIPS_CP0_SR] & MIPS_SR_IEc,
          0);
    check("syscall: KUc cleared (kernel)", cpu.cp0[MIPS_CP0_SR] & MIPS_SR_KUc,
          0);
    check("syscall: old current is now previous",
          (cpu.cp0[MIPS_CP0_SR] & (MIPS_SR_IEp | MIPS_SR_KUp)),
          MIPS_SR_IEp | MIPS_SR_KUp);

    step(&cpu, 4);              /* mfc0, addiu k0, jr, rfe(slot) */
    check("syscall: returned past the syscall", cpu.pc, CODE_BASE + 4);
    check("syscall: RFE restored IEc", cpu.cp0[MIPS_CP0_SR] & MIPS_SR_IEc,
          MIPS_SR_IEc);
    check("syscall: RFE restored KUc", cpu.cp0[MIPS_CP0_SR] & MIPS_SR_KUc,
          MIPS_SR_KUc);

    mips_step(&cpu);            /* the instruction after the syscall */
    check("syscall: resumed the program", cpu.r[1], 1);
}

/* Arithmetic overflow vectors and leaves the destination unchanged. */
static void
test_overflow(void)
{
    mips_cpu cpu;

    setup(&cpu);
    handler_skip_epc();
    cpu.r[3] = 0x7FFFFFFFu;
    cpu.r[4] = 0xDEADBEEFu;     /* must survive the trapping add */
    poke(CODE_BASE, I_ADD_4_3_3);

    mips_step(&cpu);
    check("overflow: ExcCode", exc_code(&cpu), MIPS_EXC_OVERFLOW);
    check("overflow: EPC", cpu.cp0[MIPS_CP0_EPC], CODE_BASE);
    check("overflow: destination untouched", cpu.r[4], 0xDEADBEEFu);
    check("overflow: vectored", cpu.pc, VEC);
}

/* A misaligned load raises an address error and records BadVADDR. */
static void
test_address_error(void)
{
    mips_cpu cpu;

    setup(&cpu);
    handler_skip_epc();
    poke(CODE_BASE, I_LW_5_1_0);        /* lw from address 1 */

    mips_step(&cpu);
    check("adel: ExcCode", exc_code(&cpu), MIPS_EXC_ADEL);
    check("adel: BadVADDR", cpu.cp0[MIPS_CP0_BADVADDR], 1);
    check("adel: EPC", cpu.cp0[MIPS_CP0_EPC], CODE_BASE);
}

/* A reserved opcode raises the reserved-instruction exception. */
static void
test_reserved(void)
{
    mips_cpu cpu;

    setup(&cpu);
    handler_skip_epc();
    poke(CODE_BASE, I_RESERVED);

    mips_step(&cpu);
    check("ri: ExcCode", exc_code(&cpu), MIPS_EXC_RI);
    check("ri: vectored", cpu.pc, VEC);
}

/* BREAK vectors by default; with halt_on_break it stops the CPU instead. */
static void
test_break(void)
{
    mips_cpu cpu;

    setup(&cpu);
    handler_skip_epc();
    poke(CODE_BASE, I_BREAK);
    mips_step(&cpu);
    check("break: ExcCode", exc_code(&cpu), MIPS_EXC_BREAK);
    check("break: vectored", cpu.pc, VEC);

    setup(&cpu);
    cpu.halt_on_break = 1;
    poke(CODE_BASE, I_BREAK);
    mips_step(&cpu);
    check("break: halt_on_break stops", cpu.halted != 0, 1);
    check("break: no vector when halting", cpu.pc, CODE_BASE + 4);
}

/* A fault in a branch delay slot sets BD and points EPC at the branch, so the
 * branch re-executes on return. */
static void
test_branch_delay(void)
{
    mips_cpu cpu;

    setup(&cpu);
    handler_skip_epc();
    poke(CODE_BASE + 0, I_J_CODE);      /* the branch */
    poke(CODE_BASE + 4, I_SYSCALL);     /* its delay slot faults */

    mips_step(&cpu);                    /* the branch */
    mips_step(&cpu);                    /* the syscall in the slot */
    check("bd: EPC is the branch", cpu.cp0[MIPS_CP0_EPC], CODE_BASE);
    check("bd: BD set", (cpu.cp0[MIPS_CP0_CAUSE] & MIPS_CAUSE_BD) != 0, 1);
}

/* An enabled external line is taken between instructions; EPC names the
 * instruction that has not run, the handler runs, and RFE resumes it. */
static void
test_interrupt(void)
{
    mips_cpu cpu;

    setup(&cpu);
    handler_resume_epc();
    cpu.cp0[MIPS_CP0_SR] = MIPS_SR_IEc | (1u << (8 + 2));  /* line 2 unmasked */
    poke(CODE_BASE + 0, I_ADDIU_1_1_1);
    poke(CODE_BASE + 4, I_J_CODE);

    step(&cpu, 4);                      /* two laps of the loop */
    check("irq: no spurious entry", cpu.r[2], 0);

    mips_set_irq(&cpu, 2, 1);
    mips_step(&cpu);                    /* takes the interrupt at the boundary */
    check("irq: ExcCode INT", exc_code(&cpu), MIPS_EXC_INT);
    check("irq: entered handler", cpu.pc, VEC);
    check("irq: IEc cleared on entry", cpu.cp0[MIPS_CP0_SR] & MIPS_SR_IEc, 0);
    uint32_t resume = cpu.cp0[MIPS_CP0_EPC];

    mips_set_irq(&cpu, 2, 0);           /* the driver lowers the line */
    step(&cpu, 4);                      /* addiu $2, mfc0, jr, rfe */
    check("irq: handler ran once", cpu.r[2], 1);
    check("irq: resumed interrupted pc", cpu.pc, resume);
    check("irq: RFE restored IEc", cpu.cp0[MIPS_CP0_SR] & MIPS_SR_IEc,
          MIPS_SR_IEc);
}

/* Either gate shut (the global IEc, or the per-line IM mask) blocks the line;
 * the line is a level, so enabling the gate later still takes it. */
static void
test_masking(void)
{
    mips_cpu cpu;

    /* IEc clear */
    setup(&cpu);
    handler_resume_epc();
    cpu.cp0[MIPS_CP0_SR] = (1u << (8 + 2));     /* unmasked but IEc off */
    poke(CODE_BASE + 0, I_ADDIU_1_1_1);
    poke(CODE_BASE + 4, I_J_CODE);
    mips_set_irq(&cpu, 2, 1);
    step(&cpu, 6);
    check("mask: IEc gate holds", cpu.r[2], 0);

    /* IM bit clear */
    setup(&cpu);
    handler_resume_epc();
    cpu.cp0[MIPS_CP0_SR] = MIPS_SR_IEc;         /* enabled but line masked */
    poke(CODE_BASE + 0, I_ADDIU_1_1_1);
    poke(CODE_BASE + 4, I_J_CODE);
    mips_set_irq(&cpu, 2, 1);
    step(&cpu, 6);
    check("mask: IM gate holds", cpu.r[2], 0);

    /* unmask: the level that was already raised is now taken */
    cpu.cp0[MIPS_CP0_SR] |= (1u << (8 + 2));
    mips_step(&cpu);
    check("mask: level survives until unmasked", cpu.pc, VEC);
}

/* A handler that returns with the line still raised is re-entered, the way a
 * level-sensitive line behaves. */
static void
test_level_reentry(void)
{
    mips_cpu cpu;

    setup(&cpu);
    handler_resume_epc();
    cpu.cp0[MIPS_CP0_SR] = MIPS_SR_IEc | (1u << (8 + 2));
    poke(CODE_BASE + 0, I_ADDIU_1_1_1);
    poke(CODE_BASE + 4, I_J_CODE);
    mips_set_irq(&cpu, 2, 1);               /* never lowered */

    step(&cpu, 12);
    check("level: re-entered while raised", cpu.r[2] >= 2, 1);
    check("level: loop never advanced", cpu.r[1], 0);
}

/* With system_mode clear the same events do not vector: the flag is the gate.
 * SYSCALL falls to the user hook (none installed here, so it stops), and the
 * CP0 registers stay untouched. */
static void
test_user_mode_gated(void)
{
    mips_cpu cpu;

    setup(&cpu);
    cpu.system_mode = 0;
    poke(CODE_BASE, I_SYSCALL);

    mips_step(&cpu);
    check("user: did not vector", cpu.pc != VEC, 1);
    check("user: CP0 EPC untouched", cpu.cp0[MIPS_CP0_EPC], 0);
    check("user: stopped for the harness", cpu.trapped != 0, 1);
}

int
main(void)
{
    test_syscall();
    test_overflow();
    test_address_error();
    test_reserved();
    test_break();
    test_branch_delay();
    test_interrupt();
    test_masking();
    test_level_reentry();
    test_user_mode_gated();

    if (failures) {
        printf("FAIL: %d check%s failed\n", failures,
               failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASS: mips32 system tier (exceptions, interrupts)\n");
    return 0;
}
