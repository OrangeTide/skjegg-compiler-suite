/* mips32.h : embeddable MIPS I (R3000-class) user-mode CPU emulator
 *
 * A little-endian o32 core for skj-run, the sibling of the ColdFire and RV32
 * cores.  It models the user-visible architecture only: the general registers,
 * HI/LO, the branch and load delay slots, the SYSCALL and BREAK traps, and the
 * COP1 floating-point unit (single and double precision, the condition flag and
 * rounding mode) that the hardware-float MIPS backend targets.  There is no
 * CP0, no cache and no MMIO.  The soft-float backend (MIPS_SOFTFLOAT) needs no
 * COP1 and runs here too.
 */

#ifndef MIPS32_H
#define MIPS32_H

#include <stdint.h>
#include <string.h>

#include "run_reason.h"

/****************************************************************
 * Trace event types
 ****************************************************************/

enum mips_trace_type {
    MIPS_TR_NONE = 0,
    MIPS_TR_ILLEGAL,
    MIPS_TR_FETCH_FAULT,
    MIPS_TR_LOAD_FAULT,
    MIPS_TR_STORE_FAULT,
    MIPS_TR_LOAD_MISALIGN,
    MIPS_TR_STORE_MISALIGN,
    MIPS_TR_SYSCALL,
    MIPS_TR_BREAK,
    MIPS_TR_OVERFLOW,
    MIPS_TR_TRAP,
};

/****************************************************************
 * Trace ring buffer (mirrors the RV32 and ColdFire cores)
 ****************************************************************/

#define MIPS_TRACE_NOTE_SIZE 48
#define MIPS_TRACE_CAPACITY  64

typedef struct mips_trace_event {
    uint32_t pc;
    uint32_t addr;
    uint32_t insn;
    uint8_t  type;
    uint8_t  _pad[3];
    char     note[MIPS_TRACE_NOTE_SIZE];
} mips_trace_event_t;

typedef struct mips_trace {
    mips_trace_event_t events[MIPS_TRACE_CAPACITY];
    uint32_t head;
    uint32_t total;
} mips_trace_t;

static inline void
mips_trace_init(mips_trace_t *t)
{
    memset(t, 0, sizeof(*t));
}

static inline void
mips_trace_push(mips_trace_t *t, uint8_t type, uint32_t pc,
                uint32_t insn, uint32_t addr, const char *note)
{
    mips_trace_event_t *ev = &t->events[t->head & (MIPS_TRACE_CAPACITY - 1)];

    ev->type = type;
    ev->pc = pc;
    ev->insn = insn;
    ev->addr = addr;
    if (note) {
        size_t n = strlen(note);
        if (n >= MIPS_TRACE_NOTE_SIZE)
            n = MIPS_TRACE_NOTE_SIZE - 1;
        memcpy(ev->note, note, n);
        ev->note[n] = '\0';
    } else {
        ev->note[0] = '\0';
    }
    t->head++;
    t->total++;
}

static inline uint32_t
mips_trace_count(const mips_trace_t *t)
{
    uint32_t n = t->head;
    return n < MIPS_TRACE_CAPACITY ? n : MIPS_TRACE_CAPACITY;
}

static inline int
mips_trace_overflowed(const mips_trace_t *t)
{
    return t->total > MIPS_TRACE_CAPACITY;
}

static inline const mips_trace_event_t *
mips_trace_peek(const mips_trace_t *t, uint32_t i)
{
    uint32_t n = mips_trace_count(t);
    uint32_t start;

    if (i >= n)
        return NULL;
    if (t->head <= MIPS_TRACE_CAPACITY)
        start = 0;
    else
        start = t->head;
    return &t->events[(start + i) & (MIPS_TRACE_CAPACITY - 1)];
}

/****************************************************************
 * Memory bus callbacks (the harness binds these to guest memory)
 ****************************************************************/

typedef uint32_t (*mips_read_fn)(void *ctx, uint32_t addr);
typedef void (*mips_write_fn)(void *ctx, uint32_t addr, uint32_t val);

struct mips_cpu;

/* SYSCALL hook: invoked on the SYSCALL instruction.  The handler reads the
 * o32 convention out of the register file ($v0 = r[2] number, $a0-$a3 =
 * r[4..7]) and writes the result back.  Returns 0 (reserved for future use). */
typedef int (*mips_syscall_fn)(struct mips_cpu *cpu, void *ctx);

/****************************************************************
 * The CPU
 ****************************************************************/

typedef struct mips_cpu {
    uint32_t r[32];             /* general registers; r[0] is hardwired 0 */
    uint32_t pc;                /* the instruction about to be fetched */
    uint32_t next_pc;           /* the one after it (the delay-slot machinery) */
    uint32_t cur_pc;            /* the instruction currently executing */
    uint32_t hi, lo;            /* multiply/divide result registers */

    uint32_t fpr[32];           /* COP1 register file; a double is an even/odd
                                 * pair (even = low word, little-endian) */
    uint32_t fcsr;              /* COP1 control/status: RM (bits 0-1), the FP
                                 * condition flag C (bit 23) */

    int      branch_taken;      /* a branch set next_pc this step */
    int      in_delay;          /* the current instruction is a delay slot */

    uint32_t load_reg;          /* pending load destination (0 = none) */
    uint32_t load_val;          /* its value, visible after the next instruction */

    int      halted;            /* stopped cleanly (exit) */
    int      trapped;           /* stopped on a fault or trap */
    int      yielding;          /* voluntarily parked: the host-call yield set it,
                                 * so the batched run reports SKJ_RUN_YIELD and the
                                 * scheduler clears it before the next slice (the
                                 * twin of the RV32 core's `waiting`) */
    int      trap_type;         /* enum mips_trace_type for the stop */
    int      trap_signal;       /* host signal a kernel would raise */
    uint32_t trap_addr;         /* faulting address, or the BREAK code */

    uint64_t cycles;            /* retired instruction count */
    mips_trace_t trace;

    mips_read_fn  read8, read16, read32;
    mips_write_fn write8, write16, write32;
    void *bus_ctx;

    mips_syscall_fn syscall;
    void *syscall_ctx;
} mips_cpu;

/****************************************************************
 * Public API
 ****************************************************************/

void mips_init(mips_cpu *cpu,
               mips_read_fn r8, mips_read_fn r16, mips_read_fn r32,
               mips_write_fn w8, mips_write_fn w16, mips_write_fn w32,
               void *bus_ctx);

/* Reset to a fresh state entering at `entry`. */
void mips_reset(mips_cpu *cpu, uint32_t entry);

/* Install the SYSCALL handler. */
void mips_set_syscall(mips_cpu *cpu, mips_syscall_fn fn, void *ctx);

/* Execute one instruction.  Returns 0 on success, -1 when the CPU is halted
 * or trapped (including the step that caused it). */
int mips_step(mips_cpu *cpu);

/* Execute up to count instructions, stopping the moment the machine halts or
 * traps; returns why (see run_reason.h) and, via retired when non-NULL, the
 * number of instructions executed. */
enum skj_run_reason mips_run(mips_cpu *cpu, int count, int *retired);

/* Stop the CPU (a clean halt, e.g. after the exit syscall). */
void mips_halt(mips_cpu *cpu);

/* ABI register names, indexed by register number, for diagnostics. */
extern const char *const mips_r_names[32];

#endif /* MIPS32_H */
