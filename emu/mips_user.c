/* mips_user.c : the MIPS I binding for the shared process sandbox (sched.c) */

#include "elf32.h"
#include "guest.h"
#include "mips32.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The GDB remote-protocol stub (--gdb PORT); the in-tree skj-run build defines
 * SKJ_MIPS_GDB and links emu/mips_gdb.c.  Independent of the backtrace add-on. */
#ifdef SKJ_MIPS_GDB
#include "mips_gdb.h"
#endif

/* Linux/MIPS o32 syscall numbers (__NR_Linux base is 4000). */
#define MIPS_NR_EXIT        4001
#define MIPS_NR_READ        4003
#define MIPS_NR_WRITE       4004
#define MIPS_NR_OPEN        4005
#define MIPS_NR_CLOSE       4006
#define MIPS_NR_LSEEK       4019
#define MIPS_NR_BRK         4045
#define MIPS_NR_WRITEV      4146
#define MIPS_NR_SCHED_YIELD 4162
#define MIPS_NR_NANOSLEEP   4166
#define MIPS_NR_GETDENTS64  4219
#define MIPS_NR_EXIT_GROUP  4246
#define MIPS_NR_CLOCK_GETTIME 4263
#define MIPS_NR_OPENAT      4288
#define MIPS_NR_MKDIRAT     4289
#define MIPS_NR_UNLINKAT    4294
#define MIPS_NR_FCHMODAT    4333
#define MIPS_NR_RENAMEAT2   4351
#define MIPS_NR_GETRANDOM   4353
#define MIPS_NR_STATX       4366

static int
sys_of_mips(uint32_t nr)
{
    switch (nr) {
    case MIPS_NR_EXIT:          return GSYS_EXIT;
    case MIPS_NR_READ:          return GSYS_READ;
    case MIPS_NR_WRITE:         return GSYS_WRITE;
    case MIPS_NR_OPEN:          return GSYS_OPEN;
    case MIPS_NR_OPENAT:        return GSYS_OPENAT;
    case MIPS_NR_CLOSE:         return GSYS_CLOSE;
    case MIPS_NR_LSEEK:         return GSYS_LSEEK;
    case MIPS_NR_BRK:           return GSYS_BRK;
    case MIPS_NR_WRITEV:        return GSYS_WRITEV;
    case MIPS_NR_SCHED_YIELD:   return GSYS_SCHED_YIELD;
    case MIPS_NR_NANOSLEEP:     return GSYS_NANOSLEEP;
    case MIPS_NR_GETDENTS64:    return GSYS_GETDENTS64;
    case MIPS_NR_EXIT_GROUP:    return GSYS_EXIT_GROUP;
    case MIPS_NR_CLOCK_GETTIME: return GSYS_CLOCK_GETTIME;
    case MIPS_NR_MKDIRAT:       return GSYS_MKDIRAT;
    case MIPS_NR_UNLINKAT:      return GSYS_UNLINKAT;
    case MIPS_NR_FCHMODAT:      return GSYS_FCHMODAT;
    case MIPS_NR_RENAMEAT2:     return GSYS_RENAMEAT2;
    case MIPS_NR_GETRANDOM:     return GSYS_GETRANDOM;
    case MIPS_NR_STATX:         return GSYS_STATX;
    default:                    return GSYS_UNKNOWN;
    }
}

/* MIPS Linux predates the asm-generic uapi and keeps the older open() flag bit
 * values, which differ from every other target (and from the host kernel this
 * emulator forwards to).  qemu-mipsel translates them; so must skj-run.  The
 * access mode (low two bits) and the modern flags at 0x10000 and above agree
 * across the two ABIs; the classic bits between differ and are remapped here.
 * O_RDONLY is 0 on both, which is why read paths worked before this. */
static uint32_t
mips_oflags_to_host(uint32_t f)
{
    uint32_t h = f & 3;                 /* access mode: identical */

    if (f & 0x0008) h |= 0x0400;        /* O_APPEND   */
    if (f & 0x0010) h |= 0x1000;        /* O_DSYNC    */
    if (f & 0x0080) h |= 0x0800;        /* O_NONBLOCK */
    if (f & 0x0100) h |= 0x0040;        /* O_CREAT    */
    if (f & 0x0200) h |= 0x0200;        /* O_TRUNC (same bit) */
    if (f & 0x0400) h |= 0x0080;        /* O_EXCL     */
    if (f & 0x0800) h |= 0x0100;        /* O_NOCTTY   */
    if (f & 0x1000) h |= 0x2000;        /* FASYNC / O_ASYNC */
    if (f & 0x2000) h |= 0x8000;        /* O_LARGEFILE */
    if (f & 0x4000) h |= 0x101000;      /* __O_SYNC (carries O_DSYNC) */
    if (f & 0x8000) h |= 0x4000;        /* O_DIRECT   */
    h |= (f & 0xFFFF0000u);             /* O_DIRECTORY/NOFOLLOW/NOATIME/CLOEXEC/PATH/TMPFILE agree */
    return h;
}

/* Service a SYSCALL.  o32: the number is in $v0 (r2), args in $a0-$a3 (r4-r7)
 * with the fifth and sixth on the stack at sp+16 / sp+20.  The kernel return
 * convention is $v0 = result (or positive errno) and $a3 = error flag; the
 * runtime's SYSRET turns an error into the negative-errno value the libc wants.
 * The emulator-private selectors return their result directly in $v0 (and clear
 * $a3), since their hand-written stubs do not run SYSRET. */
static int
mips_syscall(mips_cpu *cpu, void *ctx)
{
    proc *p = ctx;
    guest *g = &p->g;
    uint32_t nr = cpu->r[2];
    uint32_t a0, a1, a2, a3, a4, a5, result;
    int sys;
    int32_t r;

    switch (sched_private(p, nr, cpu->r[4], cpu->r[5], cpu->cur_pc, &result)) {
    case SKJ_PRIV_OK:    cpu->r[2] = result; cpu->r[7] = 0; return 0;
    case SKJ_PRIV_FATAL: mips_halt(cpu); return 0;
    case SKJ_PRIV_YIELD: cpu->yielding = 1; return 0;
    case SKJ_PRIV_NO:    break;
    }

    sys = sys_of_mips(nr);
    a0 = cpu->r[4]; a1 = cpu->r[5]; a2 = cpu->r[6]; a3 = cpu->r[7];
    a4 = gm_load(&g->mem, cpu->r[29] + 16, 4, 0);
    a5 = gm_load(&g->mem, cpu->r[29] + 20, 4, 0);

    /* Translate the o32 open-flag encoding to the host ABI: open(path, flags,
     * mode) carries flags in a1; openat(dirfd, path, flags, mode) in a2. */
    if (sys == GSYS_OPEN)
        a1 = mips_oflags_to_host(a1);
    else if (sys == GSYS_OPENAT)
        a2 = mips_oflags_to_host(a2);

    r = guest_syscall(g, sys, a0, a1, a2, a3, a4, a5);
    if (r < 0) {
        cpu->r[2] = (uint32_t)(-r);     /* $v0 = positive errno */
        cpu->r[7] = 1;                  /* $a3 = error */
    } else {
        cpu->r[2] = (uint32_t)r;        /* $v0 = result */
        cpu->r[7] = 0;                  /* $a3 = ok */
    }
    if (g->exited)
        mips_halt(cpu);
    return 0;
}

/****************************************************************
 * Diagnostics
 ****************************************************************/

static const char *
trace_name(int type)
{
    switch (type) {
    case MIPS_TR_ILLEGAL:        return "illegal instruction";
    case MIPS_TR_FETCH_FAULT:    return "instruction fetch fault";
    case MIPS_TR_LOAD_FAULT:     return "load fault";
    case MIPS_TR_STORE_FAULT:    return "store fault";
    case MIPS_TR_LOAD_MISALIGN:  return "misaligned load";
    case MIPS_TR_STORE_MISALIGN: return "misaligned store";
    case MIPS_TR_SYSCALL:        return "syscall";
    case MIPS_TR_BREAK:          return "break";
    case MIPS_TR_OVERFLOW:       return "arithmetic overflow";
    case MIPS_TR_TRAP:           return "trap";
    default:                     return "event";
    }
}

static void
mips_dump_trace(FILE *out, void *vcpu)
{
    const mips_cpu *cpu = vcpu;
    uint32_t n = mips_trace_count(&cpu->trace);
    uint32_t i;

    if (mips_trace_overflowed(&cpu->trace))
        fprintf(out, "  (earlier events dropped)\n");
    for (i = 0; i < n; i++) {
        const mips_trace_event_t *e = mips_trace_peek(&cpu->trace, i);
        fprintf(out, "  pc=0x%08x insn=0x%08x %s%s%s\n", e->pc, e->insn,
                trace_name(e->type), e->note[0] ? ": " : "", e->note);
    }
}

/* The integer register file at a fault, four per line, in register order. */
static void
mips_dump_regs(FILE *out, int slot, void *vcpu)
{
    const mips_cpu *cpu = vcpu;
    int i;

    fprintf(out, "skj-run: [proc %d] registers:\n", slot + 1);
    for (i = 0; i < 32; i++)
        fprintf(out, "  %4s=0x%08x%s", mips_r_names[i], cpu->r[i],
                (i % 4) == 3 ? "\n" : "");
    fprintf(out, "    hi=0x%08x   lo=0x%08x   pc=0x%08x\n",
            cpu->hi, cpu->lo, cpu->cur_pc);
}

static void
mips_report_trap(FILE *out, int slot, void *vcpu)
{
    const mips_cpu *cpu = vcpu;

    fprintf(out, "skj-run: [proc %d] %s at 0x%08x\n", slot + 1,
            trace_name(cpu->trap_type), cpu->cur_pc);
}

/****************************************************************
 * The vtable
 ****************************************************************/

static void *
mips_alloc(void)
{
    return calloc(1, sizeof(mips_cpu));
}

static void
mips_free(void *cpu)
{
    free(cpu);
}

static void
mips_ready(proc *p, uint32_t entry, uint32_t sp)
{
    mips_cpu *cpu = p->cpu;

    mips_init(cpu, skj_bus_r8, skj_bus_r16, skj_bus_r32,
              skj_bus_w8, skj_bus_w16, skj_bus_w32, p);
    mips_reset(cpu, entry);
    mips_set_syscall(cpu, mips_syscall, p);
    cpu->r[29] = sp;            /* $sp */
}

static enum skj_run_reason
mips_run_adapter(proc *p, int budget, int *ran)
{
    return mips_run(p->cpu, budget, ran);
}

static void     mips_halt_adapter(void *cpu) { mips_halt(cpu); }
static void     mips_clear_yield(void *cpu)  { ((mips_cpu *)cpu)->yielding = 0; }
static uint32_t mips_fault_pc(void *cpu)     { return ((mips_cpu *)cpu)->cur_pc; }
static uint32_t mips_bt_fp(void *cpu)        { return ((mips_cpu *)cpu)->r[30]; }
static int      mips_trap_signal(void *cpu)  { return ((mips_cpu *)cpu)->trap_signal; }

#ifdef SKJ_MIPS_GDB
static void
mips_gdb(proc *root, int port, int *ec)
{
    mips_gdb_serve(root->cpu, &root->g, port, ec);
}
#endif

static const skj_cpu_ops mips_ops = {
    .name = "MIPS",
    .machine = EM_MIPS,
    .arch = GUEST_ARCH_MIPS32,
    .big_endian = 0,
    .alloc = mips_alloc,
    .free = mips_free,
    .ready = mips_ready,
    .run = mips_run_adapter,
    .halt = mips_halt_adapter,
    .clear_yield = mips_clear_yield,
    .fault_pc = mips_fault_pc,
    .trap_pc = mips_fault_pc,           /* the MIPS trap pc is the live cur_pc too */
    .bt_fp = mips_bt_fp,
    .bt_bigendian = 0,
    .report_trap = mips_report_trap,
    .trap_signal = mips_trap_signal,
    .dump_regs = mips_dump_regs,
    .dump_trace = mips_dump_trace,
#ifdef SKJ_MIPS_GDB
    .gdb = mips_gdb,
#else
    .gdb = NULL,
#endif
};

int
run_mips32(const run_opts *o, int *status)
{
    return run_sandbox(o, &mips_ops, status);
}
