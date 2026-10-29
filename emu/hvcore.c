/* hvcore.c : the hardware-virtualization binding for the shared process sandbox
 * (sched.c), for x86-64 and aarch64.
 *
 * The guest runs natively in 64-bit supervisor mode under the host hypervisor.  It reaches the
 * sandbox syscall layer through one trap: an MMIO read at SKJ_HV_DOORBELL with
 * the syscall number and arguments in the platform's Linux syscall registers,
 * which the VMM backend reads and whose result it delivers as the load's data.  The two
 * architectures share everything here but the Linux syscall numbering and the
 * image's machine id; the per-arch register convention lives in the VMM backend. */

#if !defined(__APPLE__)
# define _DEFAULT_SOURCE                /* MAP_ANONYMOUS */
#endif

#include "elf64.h"
#include "guest.h"
#include "hvcore.h"
#include "vmm.h"
#include "sched.h"
#include "skj_hv_abi.h"

#ifdef SKJ_HV_GDB
#include "hv_gdb.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Linux/x86-64 syscall numbers. */
#define X64_NR_READ         0
#define X64_NR_WRITE        1
#define X64_NR_CLOSE        3
#define X64_NR_LSEEK        8
#define X64_NR_BRK          12
#define X64_NR_WRITEV       20
#define X64_NR_SCHED_YIELD  24
#define X64_NR_NANOSLEEP    35
#define X64_NR_EXIT         60
#define X64_NR_EXIT_GROUP   231
#define X64_NR_GETDENTS64   217
#define X64_NR_OPEN         2
#define X64_NR_OPENAT       257
#define X64_NR_MKDIRAT      258
#define X64_NR_UNLINKAT     263
#define X64_NR_RENAMEAT2    316
#define X64_NR_FCHMODAT     268
#define X64_NR_CLOCK_GETTIME 228
#define X64_NR_GETRANDOM    318
#define X64_NR_STATX        332

/* Linux/aarch64 syscall numbers (the 64-bit asm-generic table). */
#define A64_NR_MKDIRAT      34
#define A64_NR_UNLINKAT     35
#define A64_NR_FCHMODAT     53
#define A64_NR_OPENAT       56
#define A64_NR_CLOSE        57
#define A64_NR_GETDENTS64   61
#define A64_NR_LSEEK        62
#define A64_NR_READ         63
#define A64_NR_WRITE        64
#define A64_NR_WRITEV       66
#define A64_NR_EXIT         93
#define A64_NR_EXIT_GROUP   94
#define A64_NR_NANOSLEEP    101
#define A64_NR_CLOCK_GETTIME 113
#define A64_NR_SCHED_YIELD  124
#define A64_NR_BRK          214
#define A64_NR_RENAMEAT2    276
#define A64_NR_GETRANDOM    278
#define A64_NR_STATX        291

typedef struct hv_cpu {
    vmm *vm;
    vmm_vcpu *vcpu;
    uint8_t *ram;
    size_t ram_size;
    int halted;
    int yield;
    int trapped;
    int preempt_armed;                  /* the preemption timer is set up */
    uint64_t trap_pc;
    uint64_t fault_addr;
} hv_cpu;

static int
sys_of_x64(uint64_t nr)
{
    switch (nr) {
    case X64_NR_READ:           return GSYS_READ;
    case X64_NR_WRITE:          return GSYS_WRITE;
    case X64_NR_WRITEV:         return GSYS_WRITEV;
    case X64_NR_CLOSE:          return GSYS_CLOSE;
    case X64_NR_LSEEK:          return GSYS_LSEEK;
    case X64_NR_BRK:            return GSYS_BRK;
    case X64_NR_EXIT:           return GSYS_EXIT;
    case X64_NR_EXIT_GROUP:     return GSYS_EXIT_GROUP;
    case X64_NR_OPEN:           return GSYS_OPEN;
    case X64_NR_OPENAT:         return GSYS_OPENAT;
    case X64_NR_GETDENTS64:     return GSYS_GETDENTS64;
    case X64_NR_STATX:          return GSYS_STATX;
    case X64_NR_MKDIRAT:        return GSYS_MKDIRAT;
    case X64_NR_UNLINKAT:       return GSYS_UNLINKAT;
    case X64_NR_FCHMODAT:       return GSYS_FCHMODAT;
    case X64_NR_RENAMEAT2:      return GSYS_RENAMEAT2;
    case X64_NR_CLOCK_GETTIME:  return GSYS_CLOCK_GETTIME;
    case X64_NR_GETRANDOM:      return GSYS_GETRANDOM;
    case X64_NR_NANOSLEEP:      return GSYS_NANOSLEEP;
    case X64_NR_SCHED_YIELD:    return GSYS_SCHED_YIELD;
    default:                    return GSYS_UNKNOWN;
    }
}

static int
sys_of_arm64(uint64_t nr)
{
    switch (nr) {
    case A64_NR_READ:           return GSYS_READ;
    case A64_NR_WRITE:          return GSYS_WRITE;
    case A64_NR_WRITEV:         return GSYS_WRITEV;
    case A64_NR_CLOSE:          return GSYS_CLOSE;
    case A64_NR_LSEEK:          return GSYS_LSEEK;
    case A64_NR_BRK:            return GSYS_BRK;
    case A64_NR_EXIT:           return GSYS_EXIT;
    case A64_NR_EXIT_GROUP:     return GSYS_EXIT_GROUP;
    case A64_NR_OPENAT:         return GSYS_OPENAT;
    case A64_NR_GETDENTS64:     return GSYS_GETDENTS64;
    case A64_NR_STATX:          return GSYS_STATX;
    case A64_NR_MKDIRAT:        return GSYS_MKDIRAT;
    case A64_NR_UNLINKAT:       return GSYS_UNLINKAT;
    case A64_NR_FCHMODAT:       return GSYS_FCHMODAT;
    case A64_NR_RENAMEAT2:      return GSYS_RENAMEAT2;
    case A64_NR_CLOCK_GETTIME:  return GSYS_CLOCK_GETTIME;
    case A64_NR_GETRANDOM:      return GSYS_GETRANDOM;
    case A64_NR_NANOSLEEP:      return GSYS_NANOSLEEP;
    case A64_NR_SCHED_YIELD:    return GSYS_SCHED_YIELD;
    default:                    return GSYS_UNKNOWN;
    }
}

/* Service one doorbell: read the syscall registers, run the emulator-private
 * selectors or the normal guest syscall, and deliver the result as the load's
 * MMIO data.  Exposed so the GDB stub services doorbells while single-stepping. */
void
hv_service_doorbell(proc *p, struct vmm_exit *ex)
{
    hv_cpu *cpu = p->cpu;
    guest *g = &p->g;
    uint64_t nr, a[6];
    uint32_t result;
    int sys;
    int32_t r;

    vmm_vcpu_syscall(cpu->vcpu, &nr, a);

    switch (sched_private(p, (uint32_t)nr, (uint32_t)a[0], (uint32_t)a[1],
                          (uint32_t)vmm_vcpu_pc(cpu->vcpu), &result)) {
    case SKJ_PRIV_OK:
        vmm_vcpu_syscall_ret(ex, result);
        return;
    case SKJ_PRIV_FATAL:
        cpu->halted = 1;
        vmm_vcpu_syscall_ret(ex, 0);
        return;
    case SKJ_PRIV_YIELD:
        cpu->yield = 1;
        vmm_vcpu_syscall_ret(ex, 0);
        return;
    case SKJ_PRIV_NO:
        break;
    }

    sys = (g->arch == GUEST_ARCH_AARCH64) ? sys_of_arm64(nr) : sys_of_x64(nr);
    r = guest_syscall(g, sys, (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2],
                      (uint32_t)a[3], (uint32_t)a[4], (uint32_t)a[5]);
    vmm_vcpu_syscall_ret(ex, (uint64_t)(int64_t)r);
    if (g->exited)
        cpu->halted = 1;
}

/****************************************************************
 * Guest memory: one contiguous buffer shared with the vcpu
 ****************************************************************/

static int
hv_mem_init(proc *p, const run_opts *o)
{
    hv_cpu *cpu = p->cpu;
    uint8_t *ram;

    (void)o;
    if (!vmm_available()) {
        fprintf(stderr, "skj-run: hardware virtualization is unavailable "
                "(no hypervisor, or the guest arch differs from the host)\n");
        return -1;
    }
    ram = mmap(NULL, SKJ_HV_RAM_SIZE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) {
        fprintf(stderr, "skj-run: cannot allocate %u bytes of guest RAM\n",
                (unsigned)SKJ_HV_RAM_SIZE);
        return -1;
    }
    cpu->ram = ram;
    cpu->ram_size = SKJ_HV_RAM_SIZE;
    gm_set_extern(&p->g.mem, ram, SKJ_HV_RAM_SIZE);
    return 0;
}

/****************************************************************
 * The vtable
 ****************************************************************/

static void *
hv_alloc(void)
{
    return calloc(1, sizeof(hv_cpu));
}

static void
hv_free(void *vcpu)
{
    hv_cpu *cpu = vcpu;

    if (!cpu)
        return;
    if (cpu->vcpu)
        vmm_vcpu_free(cpu->vcpu);
    if (cpu->vm)
        vmm_free(cpu->vm);
    if (cpu->ram && cpu->ram != MAP_FAILED)
        munmap(cpu->ram, cpu->ram_size);
    free(cpu);
}

static void
hv_ready(proc *p, uint32_t entry, uint32_t sp)
{
    hv_cpu *cpu = p->cpu;

    cpu->vm = vmm_new();
    if (!cpu->vm || vmm_map(cpu->vm, cpu->ram, 0, cpu->ram_size) != 0) {
        fprintf(stderr, "skj-run: VMM setup failed\n");
        cpu->halted = 1;
        return;
    }
    cpu->vcpu = vmm_vcpu_new(cpu->vm);
    if (!cpu->vcpu) {
        fprintf(stderr, "skj-run: VMM vcpu setup failed\n");
        cpu->halted = 1;
        return;
    }
    vmm_vcpu_set_pc(cpu->vcpu, entry);
    vmm_vcpu_set_sp(cpu->vcpu, sp);
}

/* The HV timeslice.  KVM cannot count retired instructions, so the slice
 * currency is wall-clock time: a preemption timer breaks into the guest every
 * SKJ_HV_SLICE_US microseconds (the timer signal interrupts KVM_RUN only, never
 * host-side work), and the core treats that VMM_X_INTR as a full slice spent.
 * A guest that traps to a doorbell first yields earlier, as on the emulated
 * cores.  10 ms is a classic round-robin quantum: fine enough for fairness
 * between processes, coarse enough not to tax a syscall-heavy guest. */
#define SKJ_HV_SLICE_US 10000u

static enum skj_run_reason
hv_run(proc *p, int budget, int *ran)
{
    hv_cpu *cpu = p->cpu;
    guest *g = &p->g;
    int steps = 0;

    /* Arm the preemption timer on first use.  It is a property of the scheduler
     * run loop; the gdb stub drives its own loop and never calls hv_run, so
     * single-stepping is left undisturbed. */
    if (!cpu->preempt_armed) {
        vmm_vcpu_arm_preempt(cpu->vcpu, SKJ_HV_SLICE_US);
        cpu->preempt_armed = 1;
    }

    for (;;) {
        struct vmm_exit ex;

        if (cpu->halted) {
            if (ran) *ran = steps;
            return SKJ_RUN_HALT;
        }
        if (steps >= budget) {          /* a coarse slice: doorbells, not insns */
            if (ran) *ran = steps;
            return SKJ_RUN_BUDGET;
        }

        vmm_vcpu_run(cpu->vcpu, &ex);
        steps++;

        switch (ex.kind) {
        case VMM_X_DOORBELL:
            hv_service_doorbell(p, &ex);
            if (g->exited || cpu->halted) {
                if (ran) *ran = steps;
                return SKJ_RUN_HALT;
            }
            if (cpu->yield) {
                cpu->yield = 0;
                if (ran) *ran = steps;
                return SKJ_RUN_YIELD;
            }
            break;
        case VMM_X_INTR:
            /* The preemption timer fired (or a spurious signal woke the run):
             * a signal context switch is a budget checkpoint.  Charge the whole
             * slice and hand the CPU back so the scheduler can rotate. */
            if (ran) *ran = budget;
            return SKJ_RUN_BUDGET;
        case VMM_X_HALT:
            cpu->halted = 1;
            if (ran) *ran = steps;
            return SKJ_RUN_HALT;
        case VMM_X_MMIO:
        case VMM_X_ERROR:
        default:
            cpu->trapped = 1;
            cpu->trap_pc = vmm_vcpu_pc(cpu->vcpu);
            cpu->fault_addr = ex.mmio_addr;
            if (ran) *ran = steps;
            return SKJ_RUN_TRAP;
        }
    }
}

static void     hv_halt(void *cpu)        { ((hv_cpu *)cpu)->halted = 1; }
static void     hv_clear_yield(void *cpu) { ((hv_cpu *)cpu)->yield = 0; }
static uint32_t hv_fault_pc(void *cpu)    { return (uint32_t)vmm_vcpu_pc(((hv_cpu *)cpu)->vcpu); }
static uint32_t hv_trap_pc(void *cpu)     { return (uint32_t)((hv_cpu *)cpu)->trap_pc; }
static uint32_t hv_bt_fp(void *cpu)       { (void)cpu; return 0; }   /* no backtrace */
static int      hv_trap_signal(void *cpu) { (void)cpu; return 11; }  /* SIGSEGV */

static void
hv_report_trap(FILE *out, int slot, void *vcpu)
{
    const hv_cpu *cpu = vcpu;

    fprintf(out, "skj-run: [proc %d] unhandled guest fault at pc=0x%llx "
            "(mmio 0x%llx)\n", slot + 1,
            (unsigned long long)cpu->trap_pc,
            (unsigned long long)cpu->fault_addr);
}

static void
hv_dump_regs(FILE *out, int slot, void *vcpu)
{
    hv_cpu *cpu = vcpu;
    uint64_t regs[VMM_NGPR];
    const char *const *names;
    int n, i, nn;

    if (!cpu->vcpu)
        return;
    n = vmm_vcpu_gpregs(cpu->vcpu, regs);
    names = vmm_gpreg_names(&nn);
    fprintf(out, "skj-run: [proc %d] registers:\n", slot + 1);
    for (i = 0; i < n; i++)
        fprintf(out, "  %4s=0x%016llx%s", i < nn ? names[i] : "?",
                (unsigned long long)regs[i], (i % 2) == 1 ? "\n" : "");
    if (n % 2)
        fprintf(out, "\n");
}

static void
hv_dump_trace(FILE *out, void *vcpu)
{
    (void)out; (void)vcpu;              /* no instruction trace under the VMM */
}

vmm_vcpu *
hv_vcpu_of(proc *p)
{
    return ((hv_cpu *)p->cpu)->vcpu;
}

int
hv_is_halted(proc *p)
{
    return ((hv_cpu *)p->cpu)->halted;
}

#ifdef SKJ_HV_GDB
static void
hv_gdb(proc *root, int port, int *ec)
{
    hv_gdb_serve(root, port, ec);
}
#define HV_GDB hv_gdb
#else
#define HV_GDB NULL
#endif

#define HV_OPS(ident, nm, mach, ar)             \
    static const skj_cpu_ops ident = {          \
        .name = nm,                             \
        .machine = mach,                        \
        .arch = ar,                             \
        .big_endian = 0,                        \
        .word64 = 1,                            \
        .alloc = hv_alloc,                      \
        .free = hv_free,                        \
        .mem_init = hv_mem_init,                \
        .ready = hv_ready,                      \
        .run = hv_run,                          \
        .halt = hv_halt,                        \
        .clear_yield = hv_clear_yield,          \
        .fault_pc = hv_fault_pc,                \
        .trap_pc = hv_trap_pc,                  \
        .bt_fp = hv_bt_fp,                      \
        .bt_bigendian = 0,                      \
        .report_trap = hv_report_trap,          \
        .trap_signal = hv_trap_signal,          \
        .dump_regs = hv_dump_regs,              \
        .dump_trace = hv_dump_trace,            \
        .gdb = HV_GDB,                          \
    }

HV_OPS(hv_x86_ops, "x86-64", EM_X86_64, GUEST_ARCH_X86_64);
HV_OPS(hv_arm64_ops, "aarch64", EM_AARCH64, GUEST_ARCH_AARCH64);

int
run_x86_64hv(const run_opts *o, int *status)
{
    return run_sandbox(o, &hv_x86_ops, status);
}

int
run_arm64hv(const run_opts *o, int *status)
{
    return run_sandbox(o, &hv_arm64_ops, status);
}
