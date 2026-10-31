/* cf_user.c : the ColdFire binding for the shared process sandbox (sched.c)
 *
 * ColdFire user mode is built rather than special-cased: the CPU takes
 * exceptions through a vector table in a page of guest memory, and every entry
 * points into an unmapped "magic window".  When the PC lands there the core's
 * batched run stops (the host-reserved trap window), and this binding's run
 * wrapper decodes the vector: a TRAP #0 is a syscall it services in place and
 * resumes; anything else is a fault it reports to the scheduler.  Servicing the
 * syscall inside the wrapper is what lets ColdFire present the same contract as
 * the rv/mips cores, whose syscalls are a step callback.
 */

#include "coldfire.h"
#include "elf32.h"
#include "guest.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CF_MAGIC_BASE   0xFFFF0000u
#define CF_MAGIC_END    (CF_MAGIC_BASE + 64 * 4)
#define CF_VECTOR_PAGE  0x00000000u

/* Linux/m68k syscall numbers. */
#define M68K_NR_EXIT        1
#define M68K_NR_READ        3
#define M68K_NR_WRITE       4
#define M68K_NR_OPEN        5
#define M68K_NR_CLOSE       6
#define M68K_NR_LSEEK       19
#define M68K_NR_BRK         45
#define M68K_NR_STATX       379
#define M68K_NR_MKDIRAT     289
#define M68K_NR_UNLINKAT    294
#define M68K_NR_FCHMODAT    299
#define M68K_NR_RENAMEAT2   351
#define M68K_NR_GETDENTS64  220
#define M68K_NR_WRITEV      146
#define M68K_NR_EXIT_GROUP  247
#define M68K_NR_OPENAT      288
#define M68K_NR_SCHED_YIELD 158
#define M68K_NR_NANOSLEEP   162
#define M68K_NR_CLOCK_GETTIME 263
#define M68K_NR_GETRANDOM   352

static int
sys_of_m68k(uint32_t nr)
{
    switch (nr) {
    case M68K_NR_EXIT:          return GSYS_EXIT;
    case M68K_NR_READ:          return GSYS_READ;
    case M68K_NR_WRITE:         return GSYS_WRITE;
    case M68K_NR_OPEN:          return GSYS_OPEN;
    case M68K_NR_OPENAT:        return GSYS_OPENAT;
    case M68K_NR_CLOSE:         return GSYS_CLOSE;
    case M68K_NR_LSEEK:         return GSYS_LSEEK;
    case M68K_NR_STATX:         return GSYS_STATX;
    case M68K_NR_MKDIRAT:       return GSYS_MKDIRAT;
    case M68K_NR_UNLINKAT:      return GSYS_UNLINKAT;
    case M68K_NR_FCHMODAT:      return GSYS_FCHMODAT;
    case M68K_NR_RENAMEAT2:     return GSYS_RENAMEAT2;
    case M68K_NR_GETDENTS64:    return GSYS_GETDENTS64;
    case M68K_NR_BRK:           return GSYS_BRK;
    case M68K_NR_WRITEV:        return GSYS_WRITEV;
    case M68K_NR_EXIT_GROUP:    return GSYS_EXIT_GROUP;
    case M68K_NR_CLOCK_GETTIME: return GSYS_CLOCK_GETTIME;
    case M68K_NR_GETRANDOM:     return GSYS_GETRANDOM;
    case M68K_NR_NANOSLEEP:     return GSYS_NANOSLEEP;
    case M68K_NR_SCHED_YIELD:   return GSYS_SCHED_YIELD;
    default:                    return GSYS_UNKNOWN;
    }
}

/* Service a TRAP #0.  The exception frame (format/vector word, sr, pc) is on
 * the active stack; pop it, resume at the saved pc, and read the o32-style
 * ColdFire syscall convention out of d0 (number) / d1-d5 + a0 (args), writing
 * the result back to d0.  The emulator-private selectors go to the shared
 * handler; everything else is a Linux syscall. */
static void
cf_service(proc *p)
{
    cf_cpu *cpu = p->cpu;
    guest *g = &p->g;
    uint32_t sp = cpu->a[7];
    uint32_t sr = gm_load(&g->mem, sp + 2, 2, 1);
    uint32_t ret = gm_load(&g->mem, sp + 4, 4, 1);
    uint32_t result;
    int32_t r;

    cpu->a[7] = sp + 8;
    cpu->sr = (cpu->sr & ~0xFFFFu) | sr;
    cpu->pc = ret;

    switch (sched_private(p, cpu->d[0], cpu->d[1], cpu->d[2], ret, &result)) {
    case SKJ_PRIV_OK:    cpu->d[0] = result; return;
    case SKJ_PRIV_FATAL: cpu->halted = 1; return;   /* exit + code set by the handler */
    case SKJ_PRIV_YIELD: cpu->yielding = 1; return;
    case SKJ_PRIV_NO:    break;
    }

    r = guest_syscall(g, sys_of_m68k(cpu->d[0]), cpu->d[1], cpu->d[2], cpu->d[3],
                      cpu->d[4], cpu->d[5], cpu->a[0]);
    if (g->park == GPARK_RETRY) {
        /* The syscall did nothing: rewind to the 2-byte TRAP #0 so it re-traps
         * and re-issues next slice; commit no result, yield now.  The stack
         * pointer stays at the popped (pre-trap) value a[7] already holds, so
         * re-executing TRAP pushes an identical frame rather than leaking one. */
        cpu->pc = ret - 2;
        cpu->yielding = 1;
        return;
    }
    cpu->d[0] = (uint32_t)r;
    if (g->park == GPARK_YIELD)         /* completed, but end the slice */
        cpu->yielding = 1;
    if (g->exited)
        cpu->halted = 1;
}

/****************************************************************
 * Diagnostics
 ****************************************************************/

static const char *
trace_name(int type)
{
    switch (type) {
    case CF_TR_ILLEGAL:         return "illegal instruction";
    case CF_TR_ACCESS_ERROR:    return "access error";
    case CF_TR_ADDRESS_ERROR:   return "address error";
    case CF_TR_ZERO_DIVIDE:     return "divide by zero";
    case CF_TR_PRIVILEGE:       return "privilege violation";
    case CF_TR_FORMAT_ERROR:    return "format error";
    case CF_TR_LINE_A:          return "line A";
    case CF_TR_LINE_F:          return "line F";
    case CF_TR_TRAP:            return "trap";
    case CF_TR_DOUBLE_FAULT:    return "double fault";
    default:                    return "event";
    }
}

static void
cf_dump_trace(FILE *out, void *vcpu)
{
    const cf_cpu *cpu = vcpu;
    uint32_t n = cf_trace_count(&cpu->trace);
    uint32_t i;

    if (cf_trace_overflowed(&cpu->trace))
        fprintf(out, "  (earlier events dropped)\n");
    for (i = 0; i < n; i++) {
        const cf_trace_event_t *e = cf_trace_peek(&cpu->trace, i);
        fprintf(out, "  pc=0x%08x op=0x%04x %s%s%s\n", e->pc, e->opword,
                trace_name(e->type), e->note[0] ? ": " : "", e->note);
    }
}

static void
cf_dump_regs(FILE *out, int slot, void *vcpu)
{
    const cf_cpu *cpu = vcpu;
    int i;

    fprintf(out, "skj-run: [proc %d] registers:\n", slot + 1);
    for (i = 0; i < 8; i++)
        fprintf(out, "  d%d=0x%08x a%d=0x%08x\n", i, cpu->d[i], i, cpu->a[i]);
    fprintf(out, "  pc=0x%08x sr=0x%04x\n", cpu->pc, cpu->sr & 0xFFFF);
}

static const char *
vector_name(int vec)
{
    switch (vec) {
    case CF_VEC_ACCESS_ERROR:   return "access error";
    case CF_VEC_ADDRESS_ERROR:  return "address error";
    case CF_VEC_ILLEGAL:        return "illegal instruction";
    case CF_VEC_ZERO_DIVIDE:    return "divide by zero";
    case CF_VEC_PRIVILEGE:      return "privilege violation";
    case CF_VEC_LINE_A:         return "unimplemented line A instruction";
    case CF_VEC_LINE_F:         return "unimplemented line F instruction";
    case CF_VEC_FORMAT_ERROR:   return "format error";
    default:                    return "unhandled exception";
    }
}

static int
signal_of_vector(int vec)
{
    switch (vec) {
    case CF_VEC_ACCESS_ERROR:
    case CF_VEC_ADDRESS_ERROR:  return 11;      /* SIGSEGV */
    case CF_VEC_ZERO_DIVIDE:    return 8;        /* SIGFPE */
    case CF_VEC_ILLEGAL:
    case CF_VEC_PRIVILEGE:
    case CF_VEC_LINE_A:
    case CF_VEC_LINE_F:         return 4;        /* SIGILL */
    default:                    return 6;        /* SIGABRT */
    }
}

static void
cf_report_trap(FILE *out, int slot, void *vcpu)
{
    const cf_cpu *cpu = vcpu;
    int vec = (int)((cpu->pc - CF_MAGIC_BASE) / 4);

    fprintf(out, "skj-run: [proc %d] %s (vector %d)\n", slot + 1,
            vector_name(vec), vec);
}

static int
cf_trap_signal(void *vcpu)
{
    const cf_cpu *cpu = vcpu;

    return signal_of_vector((int)((cpu->pc - CF_MAGIC_BASE) / 4));
}

/****************************************************************
 * The vtable
 ****************************************************************/

static void *
cf_alloc(void)
{
    return calloc(1, sizeof(cf_cpu));
}

static void
cf_free(void *cpu)
{
    free(cpu);
}

static void
cf_ready(proc *p, uint32_t entry, uint32_t sp)
{
    cf_cpu *cpu = p->cpu;
    guest *g = &p->g;
    int i;

    /* Vector table: reset SP and PC, then every exception into the magic window
     * so the run wrapper sees it.  cf_reset reads the SP and PC back out. */
    gm_map(&g->mem, CF_VECTOR_PAGE, GM_PAGE_SIZE, GM_R, "vectors");
    g->mem.loading = 1;
    gm_store(&g->mem, CF_VECTOR_PAGE + 0, 4, 1, sp);
    gm_store(&g->mem, CF_VECTOR_PAGE + 4, 4, 1, entry);
    for (i = 2; i < 64; i++)
        gm_store(&g->mem, CF_VECTOR_PAGE + (uint32_t)i * 4, 4, 1,
                 CF_MAGIC_BASE + (uint32_t)i * 4);
    g->mem.loading = 0;

    cf_init(cpu, skj_bus_r8, skj_bus_r16, skj_bus_r32,
            skj_bus_w8, skj_bus_w16, skj_bus_w32, p);
    cf_reset(cpu);              /* loads SP and PC from the vector table */
    cpu->trap_window_base = CF_MAGIC_BASE;
    cpu->trap_window_end = CF_MAGIC_END;
    g->can_park = 1;            /* cooperative blocking-syscall yields (see guest.h) */
}

/* Drive cf_run, servicing TRAP #0 syscalls in place so only a terminal event
 * (exit/halt, a real fault vector, a bus fault, a yield, or a spent budget)
 * returns to the scheduler. */
static enum skj_run_reason
cf_run_adapter(proc *p, int budget, int *ran)
{
    cf_cpu *cpu = p->cpu;
    enum skj_run_reason reason = SKJ_RUN_BUDGET;
    int total = 0;

    while (total < budget) {
        int sub = 0;
        enum skj_run_reason r = cf_run(cpu, budget - total, &sub);

        total += sub;
        if (p->g.mem.fault) {           /* a bus/address error */
            reason = SKJ_RUN_TRAP;
            break;
        }
        if (r == SKJ_RUN_TRAP) {        /* pc reached the exception-vector window */
            int vec = (int)((cpu->pc - CF_MAGIC_BASE) / 4);

            if (vec == CF_VEC_TRAP(0)) {
                cf_service(p);
                if (p->g.mem.fault) { reason = SKJ_RUN_TRAP; break; }
                if (p->g.exited || cpu->halted) { reason = SKJ_RUN_HALT; break; }
                if (cpu->yielding) { reason = SKJ_RUN_YIELD; break; }
                continue;               /* serviced; keep running */
            }
            reason = SKJ_RUN_TRAP;      /* a real fault vector */
            break;
        }
        reason = r;                     /* HALT / YIELD / BUDGET */
        break;
    }
    *ran = total;
    return reason;
}

static void     cf_halt(void *cpu)        { ((cf_cpu *)cpu)->halted = 1; }
static void     cf_clear_yield(void *cpu) { ((cf_cpu *)cpu)->yielding = 0; }
static uint32_t cf_fault_pc(void *cpu)    { return ((cf_cpu *)cpu)->pc; }
static uint32_t cf_trap_pc(void *cpu)     { return ((cf_cpu *)cpu)->except_pc; }
static uint32_t cf_bt_fp(void *cpu)       { return ((cf_cpu *)cpu)->a[6]; }

static const skj_cpu_ops cf_ops = {
    .name = "m68k/ColdFire",
    .machine = EM_68K,
    .arch = GUEST_ARCH_COLDFIRE,
    .big_endian = 1,
    .alloc = cf_alloc,
    .free = cf_free,
    .ready = cf_ready,
    .run = cf_run_adapter,
    .halt = cf_halt,
    .clear_yield = cf_clear_yield,
    .fault_pc = cf_fault_pc,
    .trap_pc = cf_trap_pc,
    .bt_fp = cf_bt_fp,
    .bt_bigendian = 1,
    .report_trap = cf_report_trap,
    .trap_signal = cf_trap_signal,
    .dump_regs = cf_dump_regs,
    .dump_trace = cf_dump_trace,
    .gdb = NULL,
};

int
run_coldfire(const run_opts *o, int *status)
{
    return run_sandbox(o, &cf_ops, status);
}
