/* rv_user.c : Linux user-mode machine for the RV32 emulator */

#include "elf32.h"
#include "guest.h"
#include "rv32.h"

/* The DWARF backtrace symbolizer is an optional add-on: the in-tree skj-run
 * build defines SKJ_DWARF_BACKTRACE and links dwarf/dwread.c + ir/arena.c, so a
 * guest fault prints a symbolized backtrace.  A bare emu-rv vendoring without
 * dwarf/ leaves it undefined and still builds, reporting the fault pc alone. */
#ifdef SKJ_DWARF_BACKTRACE
#include "dwread.h"
#include "arena.h"
#endif

/* The GDB remote-protocol stub (--gdb PORT); the in-tree skj-run build defines
 * SKJ_RV_GDB and links emu/rv_gdb.c.  Independent of the backtrace add-on. */
#ifdef SKJ_RV_GDB
#include "rv_gdb.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Linux/RISC-V syscall numbers (the generic table). */
#define RV_NR_OPENAT        56
#define RV_NR_CLOSE         57
#define RV_NR_LSEEK         62
#define RV_NR_READ          63
#define RV_NR_WRITE         64
#define RV_NR_WRITEV        66
#define RV_NR_EXIT          93
#define RV_NR_EXIT_GROUP    94
#define RV_NR_BRK           214
#define RV_NR_STATX         291
#define RV_NR_MKDIRAT       34
#define RV_NR_UNLINKAT      35
#define RV_NR_RENAMEAT2     276
#define RV_NR_GETDENTS64    61
#define RV_NR_CLOCK_GETTIME 113
#define RV_NR_GETRANDOM     278
#define RV_NR_NANOSLEEP     101
#define RV_NR_SCHED_YIELD   124

/* An emulator-private ecall facility: a7 in a range far above the Linux
 * syscall space, so qemu-user answers the same call with -ENOSYS and a guest
 * that wants a guarded region can fall back. The numbers match the sandbox
 * tree's so a guest built for one runs on the other.
 *
 *   RV_SYS_FATAL  a0=exit code -> does not return
 *   RV_SYS_MAP    a0=size, a1=flags -> a0=base (0 on failure)
 *   RV_SYS_MUNMAP a0=base, a1=size  -> a0=0
 *   RV_SYS_SPAWN  a0=image ptr, a1=len -> a0=child handle (0 on failure)
 *
 * RV_SYS_FATAL is boris's VM_SYS_TRAP (0x000f0000): a guest runtime fault, so a
 * Kobold or C abort/trap faults identically on skj-run and in the boris VM. It
 * prints a symbolized backtrace for the calling frame and ends the process with
 * the given exit code, so a guest abort or runtime trap shows a source-level
 * stack like a CPU fault does; on qemu-user it is -ENOSYS and the guest exits
 * itself. The mapping calls reach gm_mmap/gm_munmap; spawn loads a fresh child
 * process from an image the guest holds in its own memory. MAP/MUNMAP/SPAWN
 * stay skj-run-only (boris faults them as unknown low selectors). */
#define RV_SYS_FATAL        0x000f0000u
#define RV_SYS_MAP          0x000f0001u
#define RV_SYS_MUNMAP       0x000f0002u
#define RV_SYS_SPAWN        0x000f0003u

#define MAX_PROC        64
#define QUANTUM         8192            /* instructions per scheduling slice */
#define SPAWN_MAX       (16u * 1024u * 1024u)   /* cap on a spawned image */

static int
sys_of_rv(uint32_t nr)
{
    switch (nr) {
    case RV_NR_OPENAT:      return GSYS_OPENAT;
    case RV_NR_CLOSE:       return GSYS_CLOSE;
    case RV_NR_LSEEK:       return GSYS_LLSEEK;      /* rv32 #62 is _llseek */
    case RV_NR_READ:        return GSYS_READ;
    case RV_NR_WRITE:       return GSYS_WRITE;
    case RV_NR_WRITEV:      return GSYS_WRITEV;
    case RV_NR_EXIT:        return GSYS_EXIT;
    case RV_NR_EXIT_GROUP:  return GSYS_EXIT_GROUP;
    case RV_NR_BRK:         return GSYS_BRK;
    case RV_NR_STATX:       return GSYS_STATX;
    case RV_NR_MKDIRAT:     return GSYS_MKDIRAT;
    case RV_NR_UNLINKAT:    return GSYS_UNLINKAT;
    case RV_NR_RENAMEAT2:   return GSYS_RENAMEAT2;
    case RV_NR_GETDENTS64:  return GSYS_GETDENTS64;
    case RV_NR_CLOCK_GETTIME: return GSYS_CLOCK_GETTIME;
    case RV_NR_GETRANDOM:   return GSYS_GETRANDOM;
    case RV_NR_NANOSLEEP:   return GSYS_NANOSLEEP;
    case RV_NR_SCHED_YIELD: return GSYS_SCHED_YIELD;
    default:                return GSYS_UNKNOWN;
    }
}

/****************************************************************
 * Processes and the scheduler
 *
 * skj-run runs one root process, and a guest may spawn children. Each has its
 * own CPU, guest memory and host bindings, so a child is isolated from its
 * parent (they share no memory). The scheduler round-robins the live processes
 * a quantum of instructions at a time. A handle is a slot plus the slot's
 * generation, so a handle held past a process's death does not match a later
 * process that reuses the slot.
 ****************************************************************/

struct sched;

typedef struct proc {
    rv_cpu cpu;
    guest g;
    struct sched *sched;        /* for spawn, to reach the process table */
    int slot;                   /* its index in the process table */
    uint32_t gen;               /* the slot's generation when this process took it */
    uint32_t handle;            /* the token spawn returns (slot + generation) */
    int done;                   /* it has stopped and needs no more slices */
    int rc;                     /* a nonzero process exit status (a fault) */
    int fatal;                  /* it exited through RV_SYS_FATAL (abort/trap) */
    uint32_t fatal_pc;          /* the call site to root the fatal backtrace at */
    uint64_t insns;             /* instructions retired by this process */
    uint64_t insn_budget;       /* its instruction cap, 0 = unlimited */
} proc;

typedef struct sched {
    proc *procs[MAX_PROC];      /* a NULL slot is free for reuse */
    uint32_t slot_gen[MAX_PROC];/* per-slot generation, bumped on reuse */
    int nprocs;                 /* high-water mark of slots ever used */
    const run_opts *o;
    struct gm_pool pool;        /* aggregate memory budget shared by all processes */
    uint64_t insns;             /* instructions retired across all processes */
    int limited;                /* the global instruction budget was hit */
#ifdef SKJ_DWARF_BACKTRACE
    struct arena bt_arena;      /* holds the lazily built reader's tables */
    struct dw_reader bt_reader; /* the root image's DWARF line + symbol tables */
    uint8_t *bt_elf;            /* the root ELF bytes the reader points into */
    int bt_state;               /* 0 untried, 1 ready, -1 unavailable */
#endif
} sched;

#define PROC_HANDLE(slot, gen)  ((uint32_t)((slot) + 1) | ((uint32_t)(gen) << 8))

static int proc_new_mem(sched *S, const uint8_t *img, long len,
                        const char *what);

/****************************************************************
 * Memory bus
 *
 * ctx is the whole process, not just its memory, so a faulting access can halt
 * the CPU. The threaded rv_run runs a batch without returning between
 * instructions; the halt is what tells the loop to stop on the offending one,
 * so the scheduler can report it at the right PC. The guest-memory fault flag
 * already records the address and reason.
 ****************************************************************/

static uint32_t
bus_r8(void *ctx, uint32_t addr)
{
    proc *p = ctx;
    uint32_t v = gm_load(&p->g.mem, addr, 1, 0);

    if (p->g.mem.fault)
        p->cpu.halted = 1;
    return v;
}

static uint32_t
bus_r16(void *ctx, uint32_t addr)
{
    proc *p = ctx;
    uint32_t v = gm_load(&p->g.mem, addr, 2, 0);

    if (p->g.mem.fault)
        p->cpu.halted = 1;
    return v;
}

static uint32_t
bus_r32(void *ctx, uint32_t addr)
{
    proc *p = ctx;
    uint32_t v = gm_load(&p->g.mem, addr, 4, 0);

    if (p->g.mem.fault)
        p->cpu.halted = 1;
    return v;
}

static void
bus_w8(void *ctx, uint32_t addr, uint32_t val)
{
    proc *p = ctx;

    gm_store(&p->g.mem, addr, 1, 0, val);
    if (p->g.mem.fault)
        p->cpu.halted = 1;
}

static void
bus_w16(void *ctx, uint32_t addr, uint32_t val)
{
    proc *p = ctx;

    gm_store(&p->g.mem, addr, 2, 0, val);
    if (p->g.mem.fault)
        p->cpu.halted = 1;
}

static void
bus_w32(void *ctx, uint32_t addr, uint32_t val)
{
    proc *p = ctx;

    gm_store(&p->g.mem, addr, 4, 0, val);
    if (p->g.mem.fault)
        p->cpu.halted = 1;
}

/****************************************************************
 * ECALL
 ****************************************************************/

/* Load a child from an image the guest holds in its own memory, and return the
 * child's handle (0 on any failure: a bad length, out of host memory, or an
 * image the loader rejects). */
static uint32_t
do_spawn(proc *p, uint32_t ptr, uint32_t len)
{
    uint8_t *buf;
    int h;

    if (len == 0 || len > SPAWN_MAX)
        return 0;
    buf = malloc(len);
    if (!buf)
        return 0;
    gm_read_out(&p->g.mem, ptr, buf, len);
    if (p->g.mem.fault) {
        /* The image range was not fully mapped. That is a bad spawn argument,
         * not a fault of the calling process, so clear it (the process is mid
         * ecall and was not faulted before this) and fail the spawn rather
         * than let the parent's next access see the stale flag and die. */
        p->g.mem.fault = 0;
        free(buf);
        return 0;
    }
    h = proc_new_mem(p->sched, buf, (long)len, "<spawned image>");
    free(buf);
    return (uint32_t)h;
}

static int
do_ecall(rv_cpu *cpu, void *ctx)
{
    proc *p = ctx;
    guest *g = &p->g;
    uint32_t nr = cpu->x[17];                    /* a7 */
    int32_t r;

    if (nr == RV_SYS_MAP) {                      /* map a fresh region */
        cpu->x[10] = gm_mmap(&g->mem, cpu->x[10], (unsigned)cpu->x[11]);
        return 0;
    }
    if (nr == RV_SYS_MUNMAP) {                   /* release one gm_mmap gave back */
        gm_munmap(&g->mem, cpu->x[10], cpu->x[11]);
        cpu->x[10] = 0;
        return 0;
    }
    if (nr == RV_SYS_SPAWN) {                    /* load a child from a guest image */
        cpu->x[10] = do_spawn(p, cpu->x[10], cpu->x[11]);
        return 0;
    }
    if (nr == RV_SYS_FATAL) {                    /* abort/trap: backtrace, then exit */
        p->fatal = 1;
        p->fatal_pc = cpu->pc;                   /* the ecall site, inside abort/trap */
        g->exited = 1;
        g->exit_code = (int)(cpu->x[10] & 0xFF);
        rv_halt(cpu);
        return 0;
    }

    r = guest_syscall(g, sys_of_rv(nr), cpu->x[10], cpu->x[11], cpu->x[12],
                      cpu->x[13], cpu->x[14], cpu->x[15]);
    cpu->x[10] = (uint32_t)r;
    if (g->exited)
        rv_halt(cpu);
    return 0;
}

/****************************************************************
 * Diagnostics
 ****************************************************************/

static const char *
trace_name(int type)
{
    switch (type) {
    case RV_TR_ILLEGAL:         return "illegal instruction";
    case RV_TR_FETCH_FAULT:     return "instruction fetch fault";
    case RV_TR_LOAD_FAULT:      return "load fault";
    case RV_TR_STORE_FAULT:     return "store fault";
    case RV_TR_LOAD_MISALIGN:   return "misaligned load";
    case RV_TR_STORE_MISALIGN:  return "misaligned store";
    case RV_TR_ECALL:           return "ecall";
    case RV_TR_EBREAK:          return "ebreak";
    case RV_TR_CSR:             return "csr";
    case RV_TR_TRAP:            return "trap";
    case RV_TR_DOUBLE_FAULT:    return "double fault";
    case RV_TR_INTERRUPT:       return "interrupt";
    default:                    return "event";
    }
}

static void
dump_trace(const rv_cpu *cpu)
{
    uint32_t n = rv_trace_count(&cpu->trace);
    uint32_t i;

    if (rv_trace_overflowed(&cpu->trace))
        fprintf(stderr, "  (earlier events dropped)\n");
    for (i = 0; i < n; i++) {
        const rv_trace_event_t *e = rv_trace_peek(&cpu->trace, i);
        fprintf(stderr, "  pc=0x%08x insn=0x%08x %s%s%s\n", e->pc, e->insn,
                trace_name(e->type), e->note[0] ? ": " : "", e->note);
    }
}

static const char *
cause_name(uint32_t cause)
{
    switch (cause) {
    case RV_CAUSE_INSN_MISALIGNED:      return "misaligned instruction";
    case RV_CAUSE_INSN_ACCESS_FAULT:    return "instruction access fault";
    case RV_CAUSE_ILLEGAL_INSN:         return "illegal instruction";
    case RV_CAUSE_BREAKPOINT:           return "breakpoint";
    case RV_CAUSE_LOAD_MISALIGNED:      return "misaligned load";
    case RV_CAUSE_LOAD_ACCESS_FAULT:    return "load access fault";
    case RV_CAUSE_STORE_MISALIGNED:     return "misaligned store";
    case RV_CAUSE_STORE_ACCESS_FAULT:   return "store access fault";
    default:                            return "trap";
    }
}

static int
signal_of_cause(uint32_t cause)
{
    switch (cause) {
    case RV_CAUSE_ILLEGAL_INSN:
        return 4;               /* SIGILL */
    case RV_CAUSE_BREAKPOINT:
        return 5;               /* SIGTRAP */
    default:
        return 11;              /* SIGSEGV */
    }
}

/****************************************************************
 * Building a process
 ****************************************************************/

/* A fresh process with its own guest memory, sharing the aggregate budget, or
 * NULL if the table is full or host memory runs out. The image is not loaded
 * yet. */
static proc *
proc_alloc(sched *S)
{
    proc *p;
    int slot = -1, i;

    for (i = 0; i < S->nprocs; i++)     /* reuse a retired slot before growing */
        if (!S->procs[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        if (S->nprocs >= MAX_PROC)
            return NULL;
        slot = S->nprocs++;
    }
    p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    gm_init(&p->g.mem, S->o->mem_limit);
    p->g.mem.pool = &S->pool;           /* share the aggregate memory budget */
    p->g.arch = GUEST_ARCH_RV32;
    p->g.big_endian = 0;
    p->g.quiet = S->o->quiet;
    p->sched = S;
    p->slot = slot;
    p->gen = S->slot_gen[slot];
    S->procs[slot] = p;
    return p;
}

/* Discard a process that failed to build: free its memory and release its slot
 * without bumping the generation (the slot never hosted a live process). */
static void
proc_discard(sched *S, proc *p)
{
    S->procs[p->slot] = NULL;
    rv_dec_free(&p->cpu);
    gm_free(&p->g.mem);
    free(p);
}

/* With the image loaded and its header validated, lay out the heap and stack,
 * seed the initial stack from argv, and ready the CPU. It already holds a table
 * slot; fill in its handle and return it. */
static int
proc_finish(sched *S, proc *p, const elf32_info *info, char **argv, int argc)
{
    run_opts so;
    uint32_t stack_base, sp, heap_base;

    heap_base = (info->hi + GM_PAGE_SIZE - 1) & ~(GM_PAGE_SIZE - 1);
    p->g.brk_base = p->g.brk = heap_base;
    p->g.brk_limit = heap_base + S->o->heap_size;
    gm_map(&p->g.mem, heap_base, S->o->heap_size, GM_R | GM_W, "heap");

    stack_base = p->g.brk_limit + GM_PAGE_SIZE;
    gm_map(&p->g.mem, stack_base, S->o->stack_size, GM_R | GM_W, "stack");
    p->g.stack_top = stack_base + S->o->stack_size;

    so = *S->o;                 /* the child keeps the driver's sizes, its own argv */
    so.argv = argv;
    so.argc = argc;
    sp = guest_build_stack(&p->g, &so);

    rv_init(&p->cpu, bus_r8, bus_r16, bus_r32, bus_w8, bus_w16, bus_w32, p);
    rv_reset(&p->cpu, info->entry);
    rv_set_ecall(&p->cpu, do_ecall, p);
    p->cpu.zcmp = 1;
    p->cpu.atomics = 1;
    p->cpu.bitmanip = 1;
    p->cpu.zcb = 1;
    p->cpu.dp = 1;              /* double precision, host-double (see rv_cpu.dp) */
    rv_dec_enable(&p->cpu);     /* the decoded-instruction cache: a pure speed-up */
    p->cpu.x[2] = sp;

    p->handle = PROC_HANDLE(p->slot, p->gen);
    return (int)p->handle;
}

/* Common tail: an RV32 little-endian image is required. Returns 0 (and frees p)
 * on any rejection. */
static int
proc_accept(sched *S, proc *p, const elf32_info *info, const char *what,
            char **argv, int argc)
{
    if (info->machine != EM_RISCV) {
        fprintf(stderr, "skj-run: %s is not a RISC-V image\n", what);
        goto fail;
    }
    if (info->big_endian) {
        fprintf(stderr, "skj-run: %s is big-endian; RV32 images are "
                "little-endian\n", what);
        goto fail;
    }
    return proc_finish(S, p, info, argv, argc);
fail:
    proc_discard(S, p);
    return 0;
}

/* The root process, from a file. */
static int
proc_new_file(sched *S, const char *path)
{
    proc *p = proc_alloc(S);
    elf32_info info;

    if (!p)
        return 0;
    if (elf32_load(&p->g, path, &info) != 0) {
        proc_discard(S, p);
        return 0;
    }
    return proc_accept(S, p, &info, path, S->o->argv, S->o->argc);
}

/* A spawned child, from an in-memory image. */
static int
proc_new_mem(sched *S, const uint8_t *img, long len, const char *what)
{
    static char *child_argv[] = { "child", NULL };
    proc *p = proc_alloc(S);
    elf32_info info;

    if (!p)
        return 0;
    p->insn_budget = S->o->child_max_insns;     /* a spawned child is budgeted */
    if (elf32_load_mem(&p->g, img, len, &info, what) != 0) {
        proc_discard(S, p);
        return 0;
    }
    return proc_accept(S, p, &info, what, child_argv, 1);
}

/****************************************************************
 * Fault backtrace (optional, SKJ_DWARF_BACKTRACE)
 ****************************************************************/

#ifdef SKJ_DWARF_BACKTRACE

#define BT_MAX_FRAMES   32

/* Guest-memory read adapter for dw_backtrace.  gm_read_out never faults (an
 * unmapped byte reads as zero), so a frame pointer off the end of the stack
 * yields a zero return address, which ends the walk. */
static int
bt_read(void *ctx, uint32_t addr, void *dst, uint32_t len)
{
    gm_read_out((guest_mem *)ctx, addr, dst, len);
    return 1;
}

/* The DWARF reader over the root image, built once on the first fault.  Only
 * the root (slot 0) is symbolized: a spawned child's image is not retained, so
 * a child fault prints raw addresses (NULL reader).  Returns NULL when the
 * image cannot be read or carries no debug info a reader can use. */
static const struct dw_reader *
root_reader(sched *S)
{
    FILE *f;
    long n;
    size_t got;

    if (S->bt_state != 0)
        return S->bt_state == 1 ? &S->bt_reader : NULL;
    S->bt_state = -1;                   /* assume failure until built */

    f = fopen(S->o->path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    S->bt_elf = malloc((size_t)n);
    if (!S->bt_elf) {
        fclose(f);
        return NULL;
    }
    got = fread(S->bt_elf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(S->bt_elf);
        S->bt_elf = NULL;
        return NULL;
    }

    arena_init(&S->bt_arena);
    if (dw_read_elf(&S->bt_arena, &S->bt_reader, S->bt_elf, (size_t)n) != 0) {
        arena_free(&S->bt_arena);
        free(S->bt_elf);
        S->bt_elf = NULL;
        return NULL;
    }
    S->bt_state = 1;
    return &S->bt_reader;
}

/* Print a symbolized backtrace for a faulting process to stderr.  pc is the
 * live instruction (the trap/fault pc); fp is the frame pointer (x8/s0). */
static void
print_backtrace(sched *S, proc *p, uint32_t pc)
{
    uint32_t pcs[BT_MAX_FRAMES];
    const struct dw_reader *r = (p->slot == 0) ? root_reader(S) : NULL;
    char buf[2048];
    int n;

    n = dw_backtrace(pc, p->cpu.x[8], bt_read, &p->g.mem, pcs, BT_MAX_FRAMES);
    if (n <= 0)
        return;
    dw_backtrace_format(r, pcs, n, buf, sizeof buf);
    fprintf(stderr, "skj-run: [proc %d] backtrace:\n%s", p->slot + 1, buf);
}

#else
#define print_backtrace(S, p, pc)      ((void)0)
#endif /* SKJ_DWARF_BACKTRACE */

/****************************************************************
 * The scheduler
 ****************************************************************/

/* Run one process for up to a quantum, or until it stops or an instruction
 * budget is hit; on a stop, record its status and mark it done. rv_run returns
 * the reason it handed control back, so the checks happen once per slice rather
 * than once per instruction. */
static void
proc_slice(sched *S, proc *p)
{
    const run_opts *o = S->o;
    uint64_t budget = QUANTUM;
    enum rv_run_reason reason;
    int ran = 0;

    /* This process is getting a slice, so it is runnable: clear any wfi park
     * from a previous slice. User mode has no interrupt to clear it (the core
     * clears waiting only when it takes one), so without this a single wfi
     * would leave the flag set and rv_run would yield after every following
     * instruction. Here wfi is a cooperative yield of the rest of the quantum,
     * and the process resumes on its next turn. */
    p->cpu.waiting = 0;

    /* Shrink the batch so a per-process or global instruction limit is met
     * exactly rather than overshot by up to a quantum. A live process has room
     * left, so budget stays positive. */
    if (p->insn_budget && p->insn_budget - p->insns < budget)
        budget = p->insn_budget - p->insns;
    if (o->max_insns && o->max_insns - S->insns < budget)
        budget = o->max_insns - S->insns;
    if (budget == 0)
        budget = 1;

    reason = rv_run(&p->cpu, (int)budget, &ran);
    S->insns += (uint64_t)ran;
    p->insns += (uint64_t)ran;

    /* A memory fault halts through the bus, so it reads as a halt with the
     * fault flag set; report it before the plain halt cases. */
    if (p->g.mem.fault) {
        if (p->g.mem.fault_guard)
            fprintf(stderr, "skj-run: [proc %d] stack overflow at 0x%08x "
                    "(pc=0x%08x)\n", p->slot + 1, p->g.mem.fault_addr,
                    p->cpu.pc);
        else
            fprintf(stderr, "skj-run: [proc %d] bad memory %s at 0x%08x "
                    "(%s), pc=0x%08x\n", p->slot + 1,
                    p->g.mem.fault_write ? "write" : "read",
                    p->g.mem.fault_addr, p->g.mem.fault_why, p->cpu.pc);
        if (o->trace)
            dump_trace(&p->cpu);
        print_backtrace(S, p, p->cpu.pc);
        p->rc = 128 + 11;
        p->done = 1;
        return;
    }
    if (reason == RV_RUN_TRAP && !p->g.exited) {
        fprintf(stderr, "skj-run: [proc %d] %s at 0x%08x (mtval=0x%08x)\n",
                p->slot + 1, cause_name(p->cpu.mcause), p->cpu.mepc,
                p->cpu.mtval);
        if (o->trace)
            dump_trace(&p->cpu);
        print_backtrace(S, p, p->cpu.mepc);
        p->rc = 128 + signal_of_cause(p->cpu.mcause);
        p->done = 1;
        return;
    }
    if (p->g.exited) {                  /* a clean exit through the exit syscall */
        if (p->fatal)                   /* ... or RV_SYS_FATAL: show the stack first */
            print_backtrace(S, p, p->fatal_pc);
        p->done = 1;
        return;
    }
    if (reason == RV_RUN_HALT) {        /* halted without exiting: a double fault */
        fprintf(stderr, "skj-run: [proc %d] stopped without calling exit "
                "(pc=0x%08x)\n", p->slot + 1, p->cpu.pc);
        if (o->trace)
            dump_trace(&p->cpu);
        p->rc = 128 + 6;
        p->done = 1;
        return;
    }
    if (p->insn_budget && p->insns >= p->insn_budget) {
        /* the child overran its own budget: kill it alone, not the run */
        fprintf(stderr, "skj-run: [proc %d] instruction budget (%llu) "
                "exceeded\n", p->slot + 1, (unsigned long long)p->insn_budget);
        p->rc = 128 + 24;               /* SIGXCPU */
        p->done = 1;
        return;
    }
    if (o->max_insns && S->insns >= o->max_insns) {
        fprintf(stderr, "skj-run: instruction limit (%llu) reached\n",
                (unsigned long long)o->max_insns);
        p->rc = 128 + 24;               /* SIGXCPU */
        p->done = 1;
        S->limited = 1;
        return;
    }
    /* RV_RUN_BUDGET or RV_RUN_YIELD with room left: still runnable, reschedule. */
}

/* Reclaim a finished child: free its guest memory (returning its pages to the
 * pool) and its struct, then free its slot for reuse with a bumped generation
 * so a stale handle to it no longer matches. The root process (slot 0) is kept
 * for the run's status and freed at the end. */
static void
retire_child(sched *S, int slot)
{
    proc *p = S->procs[slot];

    rv_dec_free(&p->cpu);
    gm_free(&p->g.mem);
    free(p);
    S->procs[slot] = NULL;
    S->slot_gen[slot]++;
}

/* Round-robin the live processes until none is runnable, retiring each finished
 * child as soon as its round ends so its memory and slot free up for the next
 * spawn. A child spawned during a round is picked up on the next one. */
static void
run_scheduler(sched *S)
{
    int any, i;

    do {
        for (i = 0; i < S->nprocs; i++) {
            proc *p = S->procs[i];

            if (!p || p->done)
                continue;
            proc_slice(S, p);
            if (S->limited)
                return;                 /* the whole run stops at the budget */
        }
        for (i = 1; i < S->nprocs; i++)         /* reclaim finished children */
            if (S->procs[i] && S->procs[i]->done)
                retire_child(S, i);
        any = 0;
        for (i = 0; i < S->nprocs; i++)
            if (S->procs[i] && !S->procs[i]->done) {
                any = 1;
                break;
            }
    } while (any);
}

/****************************************************************
 * The run loop
 ****************************************************************/

int
run_rv32(const run_opts *o, int *status)
{
    sched S;
    proc *rp;
    int rc, i;

    memset(&S, 0, sizeof S);
    S.o = o;
    S.pool.limit = o->total_mem;        /* aggregate commit cap, 0 = unlimited */

    if (proc_new_file(&S, o->path) <= 0)        /* the root image failed to load */
        return -1;

    rp = S.procs[0];
#ifdef SKJ_RV_GDB
    if (o->gdb_port > 0) {
        /* Debug the root process under the GDB remote protocol instead of
         * running the scheduler.  v1 is single-process: a guest that spawns
         * while debugged leaves its child unscheduled. */
        int ecode = 0;
        rv_gdb_serve(&rp->cpu, &rp->g, o->gdb_port, &ecode);
        rp->done = 1;
        if (rp->g.exited)
            rp->g.exit_code = ecode;
    } else {
        run_scheduler(&S);
    }
#else
    run_scheduler(&S);
#endif

    rc = rp->rc;
    if (S.limited && !rc)
        rc = 128 + 24;

    if (o->stats)
        fprintf(stderr, "skj-run: %llu instructions, %llu syscalls, "
                "%llu KB guest memory (proc 1)\n", (unsigned long long)S.insns,
                (unsigned long long)rp->g.syscalls,
                (unsigned long long)(rp->g.mem.committed / 1024));

    *status = rc ? rc : rp->g.exit_code;

    for (i = 0; i < S.nprocs; i++) {    /* the root, plus any child not yet retired */
        if (!S.procs[i])
            continue;
        rv_dec_free(&S.procs[i]->cpu);
        gm_free(&S.procs[i]->g.mem);
        free(S.procs[i]);
    }
#ifdef SKJ_DWARF_BACKTRACE
    if (S.bt_state == 1)
        arena_free(&S.bt_arena);
    free(S.bt_elf);
#endif
    return 0;
}
