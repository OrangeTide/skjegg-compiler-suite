/* mips_gdb.c : the MIPS I backend for the shared GDB stub (see mips_gdb.h).

   Supplies the gdb_target vtable; the protocol, transport, and the
   breakpoint/watchpoint tables live in the shared core (gdbstub.c).  Only the
   register layout, the delay-slot program-counter handling, and the trap-to-
   signal map are MIPS-specific.  gdb derives the o32 register layout from the
   ELF, so this backend sends no target description. */

#include "mips_gdb.h"
#include "gdbstub.h"

#include <string.h>

#define NGPR        32

/* gdb register numbers for the o32 layout (gdb's built-in MIPS layout, since
 * this stub sends no target description).  The FP registers follow pc: f0-f31
 * at 38-69, the FP control/status (fcsr) at 70, the FP implementation (fir) at
 * 71.  All are 32 bits in the o32 g packet. */
enum {
    REG_STATUS   = 32,
    REG_LO       = 33,
    REG_HI       = 34,
    REG_BADVADDR = 35,
    REG_CAUSE    = 36,
    REG_PC       = 37,
    REG_FP0      = 38,
    REG_FCSR     = 70,
    REG_FIR      = 71,
    NREG_ALL     = 72,          /* r0-pc, f0-f31, fcsr, fir */
};

struct mips_ctx {
    mips_cpu *cpu;
    guest *g;
};

/****************************************************************
 * Register hex marshalling (MIPS specific)
 ****************************************************************/

static char
hexdig(int v)
{
    return "0123456789abcdef"[v & 0xf];
}

static int
hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Little-endian u32 -> 8 lowercase hex chars appended at *p. */
static void
put_u32_hex(char **p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) {        /* byte 0 first (little-endian) */
        unsigned b = (v >> (8 * i)) & 0xff;
        *(*p)++ = hexdig(b >> 4);
        *(*p)++ = hexdig(b & 0xf);
    }
}

/* Read a little-endian u32 from 8 hex chars; returns 0 on success. */
static int
get_u32_hex(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        int hi = hexval(p[2 * i]), lo = hexval(p[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        v |= (uint32_t)((hi << 4) | lo) << (8 * i);
    }
    *out = v;
    return 0;
}

/* The pc gdb should see: the faulting instruction when stopped on a trap,
   otherwise the next instruction to fetch (so a breakpoint or step stop reads
   the address it stopped at). */
static uint32_t
gdb_pc(const mips_cpu *cpu)
{
    return cpu->trapped ? cpu->cur_pc : cpu->pc;
}

/* Redirect execution to `v`, resetting the delay-slot machinery so the next
   step fetches `v` cleanly.  A redirect is a recovery, so clear any trap that
   stopped the CPU: mips_step refuses to run while `trapped` is set, so without
   this a `c addr`/`s addr` after a fault would never resume. */
static void
gdb_set_pc(mips_cpu *cpu, uint32_t v)
{
    cpu->pc = v;
    cpu->next_pc = v + 4;
    cpu->branch_taken = 0;
    cpu->in_delay = 0;
    cpu->trapped = 0;
}

static uint32_t
reg_get(const mips_cpu *cpu, int n)
{
    if (n < NGPR)
        return cpu->r[n];
    if (n >= REG_FP0 && n < REG_FP0 + 32)
        return cpu->fpr[n - REG_FP0];
    switch (n) {
    case REG_LO:   return cpu->lo;
    case REG_HI:   return cpu->hi;
    case REG_PC:   return gdb_pc(cpu);
    case REG_FCSR: return cpu->fcsr;
    case REG_FIR:  return 0;         /* no modeled FP implementation register */
    default:       return 0;         /* status, badvaddr, cause: unmodeled */
    }
}

static void
reg_set(mips_cpu *cpu, int n, uint32_t v)
{
    if (n < NGPR) {
        cpu->r[n] = v;
        cpu->r[0] = 0;              /* r0 stays hardwired to zero */
        return;
    }
    if (n >= REG_FP0 && n < REG_FP0 + 32) {
        cpu->fpr[n - REG_FP0] = v;
        return;
    }
    switch (n) {
    case REG_LO:   cpu->lo = v; break;
    case REG_HI:   cpu->hi = v; break;
    case REG_PC:   gdb_set_pc(cpu, v); break;
    case REG_FCSR: cpu->fcsr = v; break;
    default:       break;           /* status, badvaddr, cause, fir: ignored */
    }
}

/****************************************************************
 * The gdb_target vtable
 ****************************************************************/

static int
mips_read_regs(void *ctx, char *out, size_t cap)
{
    mips_cpu *cpu = ((struct mips_ctx *)ctx)->cpu;
    char *w = out;
    int i;
    (void)cap;
    for (i = 0; i < NREG_ALL; i++)
        put_u32_hex(&w, reg_get(cpu, i));
    *w = '\0';
    return 0;
}

static int
mips_write_regs(void *ctx, const char *hex)
{
    mips_cpu *cpu = ((struct mips_ctx *)ctx)->cpu;
    const char *h = hex;
    int i;
    for (i = 0; i < NREG_ALL; i++) {
        uint32_t v;
        if (strlen(h) < 8 || get_u32_hex(h, &v))
            return -1;
        reg_set(cpu, i, v);
        h += 8;
    }
    return 0;
}

static int
mips_read_reg(void *ctx, int n, char *out, size_t cap)
{
    mips_cpu *cpu = ((struct mips_ctx *)ctx)->cpu;
    char *w = out;
    (void)cap;
    if (n < 0 || n >= NREG_ALL)
        return -1;
    put_u32_hex(&w, reg_get(cpu, n));
    *w = '\0';
    return 0;
}

static int
mips_write_reg(void *ctx, int n, const char *hex)
{
    mips_cpu *cpu = ((struct mips_ctx *)ctx)->cpu;
    uint32_t v;
    if (n < 0 || n >= NREG_ALL || get_u32_hex(hex, &v))
        return -1;
    reg_set(cpu, n, v);
    return 0;
}

static int
mips_mem_mapped(void *ctx, uint64_t addr)
{
    guest *g = ((struct mips_ctx *)ctx)->g;
    uint32_t a = (uint32_t)addr;
    int i;
    for (i = 0; i < g->mem.nregions; i++) {
        struct gm_region *r = &g->mem.regions[i];
        if (a >= r->base && a - r->base < r->size)
            return 1;
    }
    return 0;
}

static uint8_t
mips_read8(void *ctx, uint64_t addr)
{
    return gm_read8(&((struct mips_ctx *)ctx)->g->mem, (uint32_t)addr);
}

static void
mips_write8(void *ctx, uint64_t addr, uint8_t v)
{
    gm_write8(&((struct mips_ctx *)ctx)->g->mem, (uint32_t)addr, v);
}

/* The breakpoint-match pc is the raw next-fetch pc, not the display pc. */
static uint64_t
mips_get_pc(void *ctx)
{
    return ((struct mips_ctx *)ctx)->cpu->pc;
}

static void
mips_set_pc(void *ctx, uint64_t pc)
{
    gdb_set_pc(((struct mips_ctx *)ctx)->cpu, (uint32_t)pc);
}

/* Clear a prior trap before a run so execution resumes past it (it re-faults if
   unfixed rather than no-ooping); mips_step refuses to run while trapped. */
static void
mips_run_begin(void *ctx)
{
    ((struct mips_ctx *)ctx)->cpu->trapped = 0;
}

static int
mips_step_cb(void *ctx)
{
    struct mips_ctx *c = ctx;
    if (c->g->exited)
        return GDB_RUN_EXITED;
    if (c->cpu->halted || c->cpu->trapped)
        return GDB_RUN_STOP;
    if (mips_step(c->cpu) < 0)      /* halted or trapped on this step */
        return c->g->exited ? GDB_RUN_EXITED : GDB_RUN_STOP;
    if (c->g->exited)
        return GDB_RUN_EXITED;
    return GDB_RUN_OK;
}

static int
mips_exited(void *ctx)
{
    return ((struct mips_ctx *)ctx)->g->exited;
}

static int
mips_exit_code(void *ctx)
{
    return ((struct mips_ctx *)ctx)->g->exit_code;
}

/* A memory fault is a SIGSEGV; a CPU trap maps by kind to the matching gdb
   signal number (so gdb prints SIGBUS/SIGFPE/SIGILL by name); anything else is
   a SIGTRAP (step done or breakpoint). */
static int
mips_stop_signal(void *ctx)
{
    mips_cpu *cpu = ((struct mips_ctx *)ctx)->cpu;
    if (((struct mips_ctx *)ctx)->g->mem.fault)
        return 11;                  /* SIGSEGV */
    if (cpu->trapped) {
        switch (cpu->trap_type) {
        case MIPS_TR_FETCH_FAULT:
        case MIPS_TR_LOAD_FAULT:
        case MIPS_TR_STORE_FAULT:
            return 11;              /* SIGSEGV */
        case MIPS_TR_LOAD_MISALIGN:
        case MIPS_TR_STORE_MISALIGN:
            return 10;              /* SIGBUS */
        case MIPS_TR_OVERFLOW:
            return 8;               /* SIGFPE */
        case MIPS_TR_ILLEGAL:
            return 4;               /* SIGILL */
        default:
            return 5;               /* SIGTRAP (syscall/break/trap) */
        }
    }
    return 5;                       /* SIGTRAP: step done or breakpoint */
}

static const struct gdb_target mips_target = {
    0,                              /* no target description */
    mips_read_regs, mips_write_regs, mips_read_reg, mips_write_reg,
    mips_mem_mapped, mips_read8, mips_write8,
    mips_get_pc, mips_set_pc, mips_step_cb,
    mips_exited, mips_exit_code, mips_stop_signal,
    mips_run_begin, 0,
};

int
mips_gdb_serve(mips_cpu *cpu, guest *g, int port, int *exit_code)
{
    struct mips_ctx ctx;
    ctx.cpu = cpu;
    ctx.g = g;
    return gdb_serve(&mips_target, &ctx, port, exit_code);
}
