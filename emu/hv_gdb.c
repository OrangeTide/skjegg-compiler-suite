/* hv_gdb.c : the HV (VMM) backend for the shared GDB stub (see hv_gdb.h).

   Supplies the gdb_target vtable over the VMM seam: registers via the vcpu,
   memory via the shared guest-memory layer, and execution by single-stepping
   the vcpu while servicing the syscall doorbell in between so the guest keeps
   making progress.  The protocol, transport, and the breakpoint/watchpoint
   tables live in the shared core (gdbstub.c).  gdb learns the architecture from
   the ELF, so this backend sends no target description. */

#include "hv_gdb.h"
#include "hvcore.h"
#include "vmm.h"
#include "guest.h"
#include "gdbstub.h"

#include <stdlib.h>

struct hv_ctx {
    proc *p;
    guest *g;
    vmm_vcpu *vcpu;
    int fault;                      /* a guest fault stopped the last run */
};

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

/****************************************************************
 * The gdb_target vtable
 ****************************************************************/

static int
hv_read_regs(void *ctx, char *out, size_t cap)
{
    struct hv_ctx *c = ctx;
    uint8_t regs[VMM_GDB_REGBYTES];
    char *w = out;
    int n = vmm_vcpu_gdb_read_regs(c->vcpu, regs), i;
    (void)cap;
    for (i = 0; i < n; i++) {
        *w++ = hexdig(regs[i] >> 4);
        *w++ = hexdig(regs[i] & 0xf);
    }
    *w = '\0';
    return 0;
}

static int
hv_write_regs(void *ctx, const char *hex)
{
    struct hv_ctx *c = ctx;
    uint8_t regs[VMM_GDB_REGBYTES];
    const char *h = hex;
    int want = vmm_vcpu_gdb_read_regs(c->vcpu, regs);  /* expected byte count */
    int i;
    for (i = 0; i < want; i++) {        /* require exactly `want` valid hex bytes */
        int hi = hexval(h[0]), lo = hexval(h[1]);
        if (hi < 0 || lo < 0)           /* short (NUL hexval<0) or non-hex */
            return -1;
        regs[i] = (uint8_t)((hi << 4) | lo);
        h += 2;
    }
    vmm_vcpu_gdb_write_regs(c->vcpu, regs, want);
    return 0;
}

static int
hv_mem_mapped(void *ctx, uint64_t addr)
{
    guest *g = ((struct hv_ctx *)ctx)->g;
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
hv_read8(void *ctx, uint64_t addr)
{
    return gm_read8(&((struct hv_ctx *)ctx)->g->mem, (uint32_t)addr);
}

static void
hv_write8(void *ctx, uint64_t addr, uint8_t v)
{
    gm_write8(&((struct hv_ctx *)ctx)->g->mem, (uint32_t)addr, v);
}

static uint64_t
hv_get_pc(void *ctx)
{
    return vmm_vcpu_pc(((struct hv_ctx *)ctx)->vcpu);
}

static void
hv_set_pc(void *ctx, uint64_t pc)
{
    vmm_vcpu_set_pc(((struct hv_ctx *)ctx)->vcpu, pc);
}

/* Enter/leave a run region: toggle the vcpu's hardware single-step mode and, on
   entry, clear a prior fault so a continue resumes past it (step re-sets it if
   the guest faults again, rather than reporting SIGSEGV forever). */
static void
hv_run_begin(void *ctx)
{
    struct hv_ctx *c = ctx;
    c->fault = 0;
    vmm_vcpu_debug_singlestep(c->vcpu, 1);
}

static void
hv_run_end(void *ctx)
{
    vmm_vcpu_debug_singlestep(((struct hv_ctx *)ctx)->vcpu, 0);
}

/* Step one guest instruction, servicing any doorbell MMIO on the way. */
static int
hv_step_cb(void *ctx)
{
    struct hv_ctx *c = ctx;
    for (;;) {
        struct vmm_exit ex;

        if (c->g->exited)
            return GDB_RUN_EXITED;
        if (hv_is_halted(c->p))
            return GDB_RUN_STOP;
        vmm_vcpu_run(c->vcpu, &ex);
        switch (ex.kind) {
        case VMM_X_DOORBELL:
            hv_service_doorbell(c->p, &ex);
            if (c->g->exited)
                return GDB_RUN_EXITED;
            break;                      /* not an instruction boundary */
        case VMM_X_DEBUG:
            return GDB_RUN_OK;          /* one instruction retired */
        case VMM_X_INTR:
            break;
        case VMM_X_HALT:
            return c->g->exited ? GDB_RUN_EXITED : GDB_RUN_STOP;
        default:                        /* MMIO / ERROR: a guest fault */
            c->fault = 1;
            return GDB_RUN_STOP;
        }
    }
}

static int
hv_exited(void *ctx)
{
    return ((struct hv_ctx *)ctx)->g->exited;
}

static int
hv_exit_code(void *ctx)
{
    return ((struct hv_ctx *)ctx)->g->exit_code;
}

static int
hv_stop_signal(void *ctx)
{
    return ((struct hv_ctx *)ctx)->fault ? 11 /* SIGSEGV */ : 5 /* SIGTRAP */;
}

static const struct gdb_target hv_target = {
    0,                              /* no target description */
    hv_read_regs, hv_write_regs, 0, 0,  /* no p/P */
    hv_mem_mapped, hv_read8, hv_write8,
    hv_get_pc, hv_set_pc, hv_step_cb,
    hv_exited, hv_exit_code, hv_stop_signal,
    hv_run_begin, hv_run_end,
};

void
hv_gdb_serve(proc *root, int port, int *exit_code)
{
    struct hv_ctx ctx;

    ctx.p = root;
    ctx.g = &root->g;
    ctx.vcpu = hv_vcpu_of(root);
    ctx.fault = 0;
    /* The stub drives one lone vcpu with no scheduler, so it cannot service a
     * cooperative park.  Clear can_park: a blocking syscall blocks on the host
     * (as a lone process would) rather than deferring a doorbell the stub's
     * single-step loop would then mis-complete. */
    ctx.g->can_park = 0;

    gdb_serve(&hv_target, &ctx, port, exit_code);
}
