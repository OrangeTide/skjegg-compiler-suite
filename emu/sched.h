/* sched.h : the arch-neutral process sandbox and scheduler
 *
 * One scheduler drives every CPU core through a small vtable over an opaque
 * CPU handle (void *cpu).  It owns the process table, the round-robin slices,
 * spawn, the shared gm_pool memory budget, the handle generations, the fault
 * backtrace cache, and the emulator-private host-call selectors.  A core
 * supplies a `struct skj_cpu_ops` that knows how to build, run, stop and
 * inspect its own CPU; rv_user.c, mips_user.c and cf_user.c are those cores.
 */

#ifndef SKJ_SCHED_H
#define SKJ_SCHED_H

#include <stdint.h>
#include <stdio.h>

#include "guest.h"
#include "run_reason.h"

#ifdef SKJ_DWARF_BACKTRACE
#include "dwread.h"
#include "arena.h"
#endif

/* Emulator-private host-call selectors, far above the Linux syscall space, so a
 * real host kernel answers each with -ENOSYS.  The values and meanings match
 * across every core (and boris's VM_SYS_TRAP for FATAL), so a guest built for
 * one sandbox runs on another.
 *
 *   FATAL  arg0=exit code -> symbolize the stack, then end the process
 *   MAP    arg0=size, arg1=flags -> base (0 on failure)
 *   MUNMAP arg0=base, arg1=size  -> 0
 *   SPAWN  arg0=image ptr, arg1=len -> child handle (0 on failure)
 *   YIELD  give up the rest of the scheduling slice
 */
#define SKJ_SYS_FATAL   0x000f0000u
#define SKJ_SYS_MAP     0x000f0001u
#define SKJ_SYS_MUNMAP  0x000f0002u
#define SKJ_SYS_SPAWN   0x000f0003u
#define SKJ_SYS_YIELD   0x000f0004u

#define SKJ_MAX_PROC    64
#define SKJ_QUANTUM     8192            /* instructions per scheduling slice */

struct sched;
struct skj_cpu_ops;

/* One process: its CPU (a core-owned struct reached through the vtable), its
 * guest memory and host bindings, and the scheduler's bookkeeping. */
typedef struct proc {
    void *cpu;                  /* the core's CPU struct (ops->alloc) */
    guest g;
    struct sched *sched;        /* for spawn, to reach the process table */
    const struct skj_cpu_ops *ops;
    int slot;                   /* its index in the process table */
    uint32_t gen;               /* the slot's generation when this process took it */
    uint32_t handle;            /* the token spawn returns (slot + generation) */
    int done;                   /* it has stopped and needs no more slices */
    int rc;                     /* a nonzero process exit status (a fault) */
    int fatal;                  /* it exited through SKJ_SYS_FATAL (abort/trap) */
    uint32_t fatal_pc;          /* the call site to root the fatal backtrace at */
    uint64_t insns;             /* instructions retired by this process */
    uint64_t insn_budget;       /* its instruction cap, 0 = unlimited */
} proc;

typedef struct sched {
    proc *procs[SKJ_MAX_PROC];      /* a NULL slot is free for reuse */
    uint32_t slot_gen[SKJ_MAX_PROC];/* per-slot generation, bumped on reuse */
    int nprocs;                     /* high-water mark of slots ever used */
    const run_opts *o;
    const struct skj_cpu_ops *ops;
    struct gm_pool pool;            /* aggregate memory budget shared by all */
    uint64_t insns;                 /* instructions retired across all processes */
    int limited;                    /* the global instruction budget was hit */
#ifdef SKJ_DWARF_BACKTRACE
    struct arena bt_arena;          /* holds the lazily built reader's tables */
    struct dw_reader bt_reader;     /* the root image's DWARF line + symbol tables */
    uint8_t *bt_elf;                /* the root ELF bytes the reader points into */
    int bt_state;                   /* 0 untried, 1 ready, -1 unavailable */
#endif
} sched;

/* What the shared private-selector handler decided. */
enum skj_priv {
    SKJ_PRIV_NO = 0,    /* not a private selector: do the normal guest syscall */
    SKJ_PRIV_OK,        /* handled; *result holds the value for the result reg */
    SKJ_PRIV_FATAL,     /* FATAL: the process is ending; the shim halts the cpu */
    SKJ_PRIV_YIELD,     /* YIELD: the shim sets the core's park flag */
};

/* Handle MAP/MUNMAP/SPAWN/FATAL/YIELD.  arg0/arg1 are the call's two arguments
 * from wherever the core's ABI puts them; pc is the call site (for FATAL's
 * backtrace).  On SKJ_PRIV_OK, *result is the value to place in the result
 * register.  A non-private selector returns SKJ_PRIV_NO and the core shim then
 * runs the normal guest_syscall path. */
enum skj_priv sched_private(proc *p, uint32_t nr, uint32_t arg0, uint32_t arg1,
                            uint32_t pc, uint32_t *result);

/* The shared guest-memory bus callbacks (ctx is the proc; the signatures are
 * identical across the cores).  A faulting access halts the CPU through
 * ops->halt so a batched run stops on the offending instruction. */
uint32_t skj_bus_r8(void *ctx, uint32_t addr);
uint32_t skj_bus_r16(void *ctx, uint32_t addr);
uint32_t skj_bus_r32(void *ctx, uint32_t addr);
void skj_bus_w8(void *ctx, uint32_t addr, uint32_t val);
void skj_bus_w16(void *ctx, uint32_t addr, uint32_t val);
void skj_bus_w32(void *ctx, uint32_t addr, uint32_t val);

/* A core's binding for the shared scheduler.  The scheduler never names a
 * concrete CPU type: it holds `void *cpu` and reaches the core only here. */
typedef struct skj_cpu_ops {
    const char *name;           /* "rv32" / "mipsel" / "coldfire" */
    uint32_t machine;           /* EM_RISCV / EM_MIPS / EM_68K (image check) */
    int arch;                   /* GUEST_ARCH_* for guest.arch */
    int big_endian;             /* image endianness; also the bus `be` */

    void *(*alloc)(void);                   /* allocate a zeroed CPU struct */
    void  (*free)(void *cpu);               /* release it (rv also frees its dec cache) */

    /* Make a fresh process runnable: bind the shared bus (ctx=p), configure
     * features, install the core's syscall shim, reset to `entry`, and set the
     * stack pointer to `sp`.  The guest memory, heap and stack are already laid
     * out; `sp` is the built initial stack pointer. */
    void  (*ready)(proc *p, uint32_t entry, uint32_t sp);

    /* Run the process for up to `budget` instructions, servicing its own
     * syscalls, and return why it stopped (and the count via *ran).  rv/mips
     * service syscalls in a step callback; cf services its TRAP #0 inside this
     * wrapper, so all three present the same contract to the scheduler. */
    enum skj_run_reason (*run)(proc *p, int budget, int *ran);

    void  (*halt)(void *cpu);               /* stop cleanly */
    void  (*clear_yield)(void *cpu);        /* clear the park flag at a slice start */

    uint32_t (*fault_pc)(void *cpu);        /* live pc for a bus fault + its backtrace */
    uint32_t (*trap_pc)(void *cpu);         /* faulting pc for a CPU trap's backtrace */
    uint32_t (*bt_fp)(void *cpu);           /* frame pointer the backtrace walks */
    int   bt_bigendian;                     /* read backtrace slots big-endian (cf) */

    void  (*report_trap)(FILE *out, int slot, void *cpu); /* the trap one-liner */
    int   (*trap_signal)(void *cpu);        /* process rc = 128 + this */
    void  (*dump_regs)(FILE *out, int slot, void *cpu);
    void  (*dump_trace)(FILE *out, void *cpu);

    /* Optional: serve the GDB remote protocol on the root process instead of
     * the scheduler (rv only; NULL elsewhere).  The exit code is returned via
     * *ec and whether the guest exited via g.exited on the root. */
    void  (*gdb)(proc *root, int port, int *ec);
} skj_cpu_ops;

/* The whole run: load the root image, install mounts, schedule to completion
 * (or serve GDB), print --stats, and report the status.  Returns 0 on a normal
 * run, -1 if the root image could not be loaded. */
int run_sandbox(const run_opts *o, const skj_cpu_ops *ops, int *status);

#endif /* SKJ_SCHED_H */
