/* sched.c : the arch-neutral process sandbox and scheduler (see sched.h) */

#include "sched.h"
#include "elf32.h"
#include "elf64.h"
#include "hostplat.h"           /* hp_clock: the monotonic clock for park deadlines */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

#define PROC_HANDLE(slot, gen)  ((uint32_t)((slot) + 1) | ((uint32_t)(gen) << 8))

static int proc_new_mem(sched *S, const uint8_t *img, long len,
                        const char *what);

/****************************************************************
 * Memory bus
 *
 * ctx is the whole process, so a faulting access halts the CPU through the
 * vtable: the batched run then stops on the offending instruction (the
 * guest-memory fault flag records the address and reason).  The `be` argument
 * is the process's image endianness, so the one set of callbacks serves the
 * big-endian ColdFire guest and the little-endian rv/MIPS guests alike.
 ****************************************************************/

uint32_t
skj_bus_r8(void *ctx, uint32_t addr)
{
    proc *p = ctx;
    uint32_t v = gm_load(&p->g.mem, addr, 1, p->g.big_endian);

    if (p->g.mem.fault)
        p->ops->halt(p->cpu);
    return v;
}

uint32_t
skj_bus_r16(void *ctx, uint32_t addr)
{
    proc *p = ctx;
    uint32_t v = gm_load(&p->g.mem, addr, 2, p->g.big_endian);

    if (p->g.mem.fault)
        p->ops->halt(p->cpu);
    return v;
}

uint32_t
skj_bus_r32(void *ctx, uint32_t addr)
{
    proc *p = ctx;
    uint32_t v = gm_load(&p->g.mem, addr, 4, p->g.big_endian);

    if (p->g.mem.fault)
        p->ops->halt(p->cpu);
    return v;
}

void
skj_bus_w8(void *ctx, uint32_t addr, uint32_t val)
{
    proc *p = ctx;

    gm_store(&p->g.mem, addr, 1, p->g.big_endian, val);
    if (p->g.mem.fault)
        p->ops->halt(p->cpu);
}

void
skj_bus_w16(void *ctx, uint32_t addr, uint32_t val)
{
    proc *p = ctx;

    gm_store(&p->g.mem, addr, 2, p->g.big_endian, val);
    if (p->g.mem.fault)
        p->ops->halt(p->cpu);
}

void
skj_bus_w32(void *ctx, uint32_t addr, uint32_t val)
{
    proc *p = ctx;

    gm_store(&p->g.mem, addr, 4, p->g.big_endian, val);
    if (p->g.mem.fault)
        p->ops->halt(p->cpu);
}

/****************************************************************
 * The emulator-private host calls
 ****************************************************************/

/* Load a child from an image the guest holds in its own memory, and return the
 * child's handle (0 on any failure: a bad length, out of host memory, or an
 * image the loader rejects). */
static uint32_t
do_spawn(proc *p, uint32_t ptr, uint32_t len)
{
    uint8_t *buf;
    int h;

    if (len == 0 || len > (16u * 1024u * 1024u))        /* SPAWN image cap */
        return 0;
    buf = malloc(len);
    if (!buf)
        return 0;
    gm_read_out(&p->g.mem, ptr, buf, len);
    if (p->g.mem.fault) {
        /* The image range was not fully mapped. That is a bad spawn argument,
         * not a fault of the calling process, so clear it (the process is mid
         * call and was not faulted before this) and fail the spawn rather than
         * let the parent's next access see the stale flag and die. */
        p->g.mem.fault = 0;
        free(buf);
        return 0;
    }
    h = proc_new_mem(p->sched, buf, (long)len, "<spawned image>");
    free(buf);
    return (uint32_t)h;
}

enum skj_priv
sched_private(proc *p, uint32_t nr, uint32_t arg0, uint32_t arg1,
              uint32_t pc, uint32_t *result)
{
    switch (nr) {
    case SKJ_SYS_MAP:
        *result = gm_mmap(&p->g.mem, arg0, (unsigned)arg1);
        return SKJ_PRIV_OK;
    case SKJ_SYS_MUNMAP:
        gm_munmap(&p->g.mem, arg0, arg1);
        *result = 0;
        return SKJ_PRIV_OK;
    case SKJ_SYS_SPAWN:
        *result = do_spawn(p, arg0, arg1);
        return SKJ_PRIV_OK;
    case SKJ_SYS_FATAL:
        p->fatal = 1;
        p->fatal_pc = pc;
        p->g.exited = 1;
        p->g.exit_code = (int)(arg0 & 0xFF);
        return SKJ_PRIV_FATAL;
    case SKJ_SYS_YIELD:
        return SKJ_PRIV_YIELD;
    case SKJ_SYS_P9:
        *result = (uint32_t)guest_p9_doorbell(&p->g, arg0);
        return SKJ_PRIV_OK;
    default:
        return SKJ_PRIV_NO;
    }
}

/****************************************************************
 * Building a process
 ****************************************************************/

/* A fresh process with its own CPU and guest memory, sharing the aggregate
 * budget, or NULL if the table is full or host memory runs out. The image is
 * not loaded yet. */
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
        if (S->nprocs >= SKJ_MAX_PROC)
            return NULL;
        slot = S->nprocs++;
    }
    p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->ops = S->ops;
    p->cpu = S->ops->alloc();
    if (!p->cpu) {
        free(p);
        return NULL;
    }
    gm_init(&p->g.mem, S->o->mem_limit);
    p->g.mem.pool = &S->pool;           /* share the aggregate memory budget */
    p->g.arch = S->ops->arch;
    p->g.big_endian = S->ops->big_endian;
    p->g.quiet = S->o->quiet;
    p->sched = S;
    p->slot = slot;
    p->gen = S->slot_gen[slot];
    if (S->ops->mem_init && S->ops->mem_init(p, S->o) != 0) {
        S->ops->free(p->cpu);
        gm_free(&p->g.mem);
        free(p);
        return NULL;
    }
    S->procs[slot] = p;
    return p;
}

/* Discard a process that failed to build: free its CPU and memory and release
 * its slot without bumping the generation (the slot never hosted a live
 * process). */
static void
proc_discard(sched *S, proc *p)
{
    S->procs[p->slot] = NULL;
    S->ops->free(p->cpu);
    guest_p9_free(&p->g);
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
    sp = S->ops->word64 ? guest_build_stack64(&p->g, &so)
                        : guest_build_stack(&p->g, &so);

    p->ops->ready(p, info->entry, sp);

    p->handle = PROC_HANDLE(p->slot, p->gen);
    return (int)p->handle;
}

/* Common tail: the image must match the core (machine and endianness). Returns
 * 0 (and frees p) on any rejection. */
static int
proc_accept(sched *S, proc *p, const elf32_info *info, const char *what,
            char **argv, int argc)
{
    if (info->machine != S->ops->machine) {
        fprintf(stderr, "skj-run: %s is not a %s image\n", what, S->ops->name);
        goto fail;
    }
    if (info->big_endian != S->ops->big_endian) {
        fprintf(stderr, "skj-run: %s has the wrong byte order for %s\n",
                what, S->ops->name);
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
    if ((S->ops->word64 ? elf64_load(&p->g, path, &info)
                        : elf32_load(&p->g, path, &info)) != 0) {
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
    if ((S->ops->word64 ? elf64_load_mem(&p->g, img, len, &info, what)
                        : elf32_load_mem(&p->g, img, len, &info, what)) != 0) {
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
 * yields a zero return address, which ends the walk.  A big-endian core (cf)
 * composes each 4-byte slot into host order; a little-endian one reads the
 * native uint32 straight through. */
struct bt_ctx {
    guest_mem *mem;
    int big_endian;
};

static int
bt_read(void *ctx, uint32_t addr, void *dst, uint32_t len)
{
    struct bt_ctx *b = ctx;

    gm_read_out(b->mem, addr, dst, len);
    if (b->big_endian && len == 4) {
        uint8_t *p = dst;
        uint32_t v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
                   | (uint32_t)p[2] << 8 | (uint32_t)p[3];
        memcpy(dst, &v, 4);
    }
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
 * live instruction; the frame pointer comes from the vtable. */
static void
print_backtrace(sched *S, proc *p, uint32_t pc)
{
    uint32_t pcs[BT_MAX_FRAMES];
    const struct dw_reader *r = (p->slot == 0) ? root_reader(S) : NULL;
    struct bt_ctx bc;
    char buf[2048];
    int n;

    bc.mem = &p->g.mem;
    bc.big_endian = S->ops->bt_bigendian;
    n = dw_backtrace(pc, S->ops->bt_fp(p->cpu), bt_read, &bc, pcs, BT_MAX_FRAMES);
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
 * budget is hit; on a stop, record its status and mark it done.  The core's
 * run() returns the reason it handed control back, so the checks happen once
 * per slice rather than once per instruction. */
static void
proc_slice(sched *S, proc *p)
{
    const run_opts *o = S->o;
    const skj_cpu_ops *ops = S->ops;
    uint64_t budget = SKJ_QUANTUM;
    enum skj_run_reason reason;
    int ran = 0;

    /* This process is getting a slice, so it is runnable: clear any cooperative
     * yield from a previous slice (rv's wfi park, or the MAP-range YIELD host
     * call on mips/cf).  User mode has no interrupt to clear it, so without this
     * a single yield would park the task forever. */
    ops->clear_yield(p->cpu);

    /* Shrink the batch so a per-process or global instruction limit is met
     * exactly rather than overshot by up to a quantum. A live process has room
     * left, so budget stays positive. */
    if (p->insn_budget && p->insn_budget - p->insns < budget)
        budget = p->insn_budget - p->insns;
    if (o->max_insns && o->max_insns - S->insns < budget)
        budget = o->max_insns - S->insns;
    if (budget == 0)
        budget = 1;

    reason = ops->run(p, (int)budget, &ran);
    S->insns += (uint64_t)ran;
    p->insns += (uint64_t)ran;

    /* A memory fault halts through the bus, so it reads as a halt with the
     * fault flag set; report it before the plain halt cases. */
    if (p->g.mem.fault) {
        uint32_t pc = ops->fault_pc(p->cpu);

        if (p->g.mem.fault_guard)
            fprintf(stderr, "skj-run: [proc %d] stack overflow at 0x%08x "
                    "(pc=0x%08x)\n", p->slot + 1, p->g.mem.fault_addr, pc);
        else
            fprintf(stderr, "skj-run: [proc %d] bad memory %s at 0x%08x "
                    "(%s), pc=0x%08x\n", p->slot + 1,
                    p->g.mem.fault_write ? "write" : "read",
                    p->g.mem.fault_addr, p->g.mem.fault_why, pc);
        ops->dump_regs(stderr, p->slot, p->cpu);
        if (o->trace)
            ops->dump_trace(stderr, p->cpu);
        print_backtrace(S, p, pc);
        p->rc = 128 + 11;               /* SIGSEGV */
        p->done = 1;
        return;
    }
    if (reason == SKJ_RUN_TRAP && !p->g.exited) {
        ops->report_trap(stderr, p->slot, p->cpu);
        ops->dump_regs(stderr, p->slot, p->cpu);
        if (o->trace)
            ops->dump_trace(stderr, p->cpu);
        print_backtrace(S, p, ops->trap_pc(p->cpu));
        p->rc = 128 + ops->trap_signal(p->cpu);
        p->done = 1;
        return;
    }
    if (p->g.exited) {                  /* a clean exit through the exit syscall */
        if (p->fatal)                   /* ... or SKJ_SYS_FATAL: show the stack first */
            print_backtrace(S, p, p->fatal_pc);
        p->done = 1;
        return;
    }
    if (reason == SKJ_RUN_HALT) {       /* halted without exiting: a double fault */
        fprintf(stderr, "skj-run: [proc %d] stopped without calling exit "
                "(pc=0x%08x)\n", p->slot + 1, ops->fault_pc(p->cpu));
        if (o->trace)
            ops->dump_trace(stderr, p->cpu);
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
    /* A cooperative park on a blocking syscall (read/nanosleep): record what the
     * process is waiting on so the scheduler can skip its slices and poll on its
     * behalf, rather than spinning the syscall every round.  A plain yield
     * (sched_yield / SKJ_SYS_YIELD) leaves io_blocked clear and just reschedules. */
    if (reason == SKJ_RUN_YIELD && p->g.park == GPARK_RETRY) {
        p->io_blocked = 1;
        p->park_fd = p->g.park_fd;
        p->park_events = p->g.park_events;
        p->park_deadline = p->g.park_deadline;
    }
    p->g.park = GPARK_NONE;

    /* SKJ_RUN_BUDGET or SKJ_RUN_YIELD with room left: still runnable, reschedule. */
}

/* Reclaim a finished child: free its CPU and guest memory (returning its pages
 * to the pool), then free its slot for reuse with a bumped generation so a
 * stale handle to it no longer matches. The root process (slot 0) is kept for
 * the run's status and freed at the end. */
static void
retire_child(sched *S, int slot)
{
    proc *p = S->procs[slot];

    S->ops->free(p->cpu);
    guest_p9_free(&p->g);
    gm_free(&p->g.mem);
    free(p);
    S->procs[slot] = NULL;
    S->slot_gen[slot]++;
}

/* A monotonic clock in nanoseconds, matching guest.c's sleep deadlines. */
static uint64_t
mono_ns(void)
{
    uint32_t sec = 0, nsec = 0;

    hp_clock(0, &sec, &nsec);                   /* 0 = monotonic */
    return (uint64_t)sec * 1000000000ull + nsec;
}

/* Wait for an I/O-parked process to become runnable.  Build a pollfd set from
 * every io_blocked process with a real fd, take the nearest sleep deadline as a
 * timeout, and poll.  When `block` is 0 the poll returns at once (timeout 0),
 * just harvesting fds that are already ready; when 1 it blocks until something
 * is ready or the nearest deadline passes.  A process is unblocked when its fd
 * signals or its deadline has arrived.  Returns the number unblocked. */
static int
poll_blocked(sched *S, int block)
{
    struct pollfd fds[SKJ_MAX_PROC];
    int idx[SKJ_MAX_PROC];              /* idx[k] = proc slot for fds[k] */
    int nfds = 0, i, woke = 0;
    uint64_t now, deadline = 0;         /* nearest future sleep deadline, 0 = none */
    int timeout;

    for (i = 0; i < S->nprocs; i++) {
        proc *p = S->procs[i];

        if (!p || p->done || !p->io_blocked)
            continue;
        if (p->park_fd >= 0) {
            fds[nfds].fd = p->park_fd;
            fds[nfds].events = p->park_events;
            fds[nfds].revents = 0;
            idx[nfds] = i;
            nfds++;
        }
        if (p->park_deadline && (deadline == 0 || p->park_deadline < deadline))
            deadline = p->park_deadline;
    }

    if (!block) {
        timeout = 0;
    } else if (deadline) {
        now = mono_ns();
        if (deadline <= now) {
            timeout = 0;
        } else {
            uint64_t ms = (deadline - now + 999999ull) / 1000000ull;
            /* Cap a very long sleep: poll wakes early, the deadline loop below
             * leaves the sleeper parked, and the scheduler polls again. */
            timeout = ms > 3600000ull ? 3600000 : (int)ms;
        }
    } else {
        timeout = -1;                   /* only fd waiters: block indefinitely */
    }

    if (nfds > 0) {
        int r;
        do {
            r = poll(fds, (nfds_t)nfds, timeout);
        } while (r < 0 && errno == EINTR);
        for (i = 0; i < nfds; i++)
            if (fds[i].revents) {
                S->procs[idx[i]]->io_blocked = 0;
                woke++;
            }
    } else if (block && deadline) {
        /* Pure sleepers, nothing to poll: wait out the nearest deadline. */
        if (timeout > 0)
            hp_sleep((uint32_t)(timeout / 1000),
                     (uint32_t)((timeout % 1000) * 1000000));
    }

    /* Wake any sleeper whose deadline has now passed. */
    now = mono_ns();
    for (i = 0; i < S->nprocs; i++) {
        proc *p = S->procs[i];

        if (p && !p->done && p->io_blocked && p->park_deadline &&
            now >= p->park_deadline) {
            p->io_blocked = 0;
            woke++;
        }
    }
    return woke;
}

/* Round-robin the live processes until none is runnable, retiring each finished
 * child as soon as its round ends so its memory and slot free up for the next
 * spawn. A child spawned during a round is picked up on the next one.  A process
 * parked on a blocking syscall (io_blocked) is skipped until poll_blocked finds
 * its fd ready or its sleep deadline reached; when every live process is parked,
 * the scheduler blocks in poll rather than spinning. */
static void
run_scheduler(sched *S)
{
    int live, ran, i;

    for (;;) {
        ran = 0;
        for (i = 0; i < S->nprocs; i++) {
            proc *p = S->procs[i];

            if (!p || p->done || p->io_blocked)
                continue;
            proc_slice(S, p);
            ran = 1;
            if (S->limited)
                return;                 /* the whole run stops at the budget */
        }
        for (i = 1; i < S->nprocs; i++)         /* reclaim finished children */
            if (S->procs[i] && S->procs[i]->done)
                retire_child(S, i);

        live = 0;
        for (i = 0; i < S->nprocs; i++)
            if (S->procs[i] && !S->procs[i]->done) {
                live = 1;
                break;
            }
        if (!live)
            return;                     /* every process has exited */

        /* If anything ran this round, more work is immediately available, so
         * only harvest already-ready parked fds without waiting.  If nothing
         * ran, every live process is I/O-parked: block until one is ready. */
        poll_blocked(S, !ran);
    }
}

/****************************************************************
 * The run loop
 ****************************************************************/

int
run_sandbox(const run_opts *o, const skj_cpu_ops *ops, int *status)
{
    sched S;
    proc *rp;
    int rc, i;

    memset(&S, 0, sizeof S);
    S.o = o;
    S.ops = ops;
    S.pool.limit = o->total_mem;        /* aggregate commit cap, 0 = unlimited */

    if (proc_new_file(&S, o->path) <= 0)        /* the root image failed to load */
        return -1;

    rp = S.procs[0];
    guest_install_mounts(&rp->g, o->path, o);

    if (ops->gdb && o->gdb_port > 0) {
        /* Debug the root process under the GDB remote protocol instead of
         * running the scheduler.  Single-process: a guest that spawns while
         * debugged leaves its child unscheduled. */
        int ecode = 0;
        ops->gdb(rp, o->gdb_port, &ecode);
        rp->done = 1;
        if (rp->g.exited)
            rp->g.exit_code = ecode;
    } else {
        run_scheduler(&S);
    }

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
        ops->free(S.procs[i]->cpu);
        guest_p9_free(&S.procs[i]->g);
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
