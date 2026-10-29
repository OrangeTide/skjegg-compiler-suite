/* vmm_hvf.c : the macOS Hypervisor.framework backend of the VMM seam (see vmm.h)
 *
 * The Apple Silicon sibling of vmm_kvm.c.  The guest is an arm64 EL1 supervisor
 * running with its MMU off (so guest-virtual equals guest-physical), on one RAM
 * slot mapped through stage-2.  The only guest/host contract is the syscall
 * doorbell of skj_hv_abi.h: an eight-byte load from SKJ_HV_DOORBELL, an address
 * outside the mapped RAM, which faults out as a stage-2 data abort.  Framework
 * names stay as Apple spells them (hv_*); the seam's own identifiers are vmm_*.
 *
 * Cross-architecture virtualization is impossible, so this backend is arm64
 * only.  On any other Darwin host (an Intel Mac) it compiles to the unavailable
 * stub, exactly as vmm_kvm.c does off Linux or off a supported arch. */

#include "vmm.h"
#include "skj_hv_abi.h"

#if defined(__APPLE__) && defined(__aarch64__)
#define VMM_HVF_OK 1
#else
#define VMM_HVF_OK 0
#endif

#if VMM_HVF_OK

#include <Hypervisor/Hypervisor.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>

/* AArch64 exception syndrome: the top six bits of ESR_EL2 are the exception
 * class, and the low 25 bits are the instruction-specific syndrome (ISS). */
#define ESR_EC(esr)     (((esr) >> 26) & 0x3f)
#define EC_WFI_WFE      0x01    /* trapped WFI/WFE: the guest went idle */
#define EC_SOFT_STEP    0x32    /* software-step debug exception */
#define EC_DATA_ABORT   0x24    /* stage-2 data abort: an MMIO access */
#define EC_BRK_AARCH64  0x3c    /* BRK software breakpoint */

/* Initial PSTATE for the guest: EL1h (its stack pointer is SP_EL1, the one set
 * through vmm_vcpu_set_sp) with DAIF masked, so a stray interrupt cannot vector
 * into an uninitialised exception table before the guest installs one. */
#define PSTATE_EL1H_DAIF 0x3c5

/* MDSCR_EL1.SS (bit 0) and PSTATE.SS (bit 21) drive the software single-step
 * state machine; the step completes as an EC_SOFT_STEP exception. */
#define MDSCR_SS    (1u << 0)
#define PSTATE_SS   (1ull << 21)

/* On Apple Silicon the VM is a per-process singleton: hv_vm_create() takes no
 * handle and there is only ever one.  The seam still hands back an allocated
 * struct so the opaque-handle shape matches the KVM backend. */
struct vmm {
    int created;
};

struct vmm_vcpu {
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit;       /* framework-owned, refreshed after each run */

    /* An MMIO access the last run faulted on, to be completed on the next run:
     * KVM finishes these in the kernel, but HVF leaves the backend to deliver a
     * load result into the destination register and step PC past the faulting
     * instruction itself. */
    int mmio_pending;
    int mmio_write;             /* a store (no writeback) rather than a load */
    int mmio_sign;              /* sign-extend a load result */
    int mmio_wide;              /* destination is a 64-bit (X) register */
    uint32_t mmio_srt;          /* data register number, 31 == XZR */
    uint8_t mmio_bytes;         /* access width in bytes */
    uint8_t mmio_data[8];       /* the doorbell load's result lands here */

    int stepping;               /* single-step armed via debug_singlestep */
};

/* The preemption timer fires a SIGALRM; the handler forces the running vcpu out
 * of hv_vcpu_run with hv_vcpus_exit, which surfaces as HV_EXIT_REASON_CANCELED.
 * Our sandbox drives a single vcpu, so one global handle suffices.  (A multi-
 * vcpu host would track a set here.) */
static hv_vcpu_t g_preempt_vcpu;
static volatile int g_preempt_armed;

/* ---- small register helpers ------------------------------------------- */

static uint64_t
reg_get(vmm_vcpu *vcpu, hv_reg_t r)
{
    uint64_t v = 0;
    hv_vcpu_get_reg(vcpu->vcpu, r, &v);
    return v;
}

static void
reg_set(vmm_vcpu *vcpu, hv_reg_t r, uint64_t v)
{
    hv_vcpu_set_reg(vcpu->vcpu, r, v);
}

static uint64_t
x_get(vmm_vcpu *vcpu, int i)
{
    return reg_get(vcpu, (hv_reg_t)(HV_REG_X0 + i));
}

static void
x_set(vmm_vcpu *vcpu, int i, uint64_t v)
{
    reg_set(vcpu, (hv_reg_t)(HV_REG_X0 + i), v);
}

static uint64_t
sys_get(vmm_vcpu *vcpu, hv_sys_reg_t r)
{
    uint64_t v = 0;
    hv_vcpu_get_sys_reg(vcpu->vcpu, r, &v);
    return v;
}

static void
sys_set(vmm_vcpu *vcpu, hv_sys_reg_t r, uint64_t v)
{
    hv_vcpu_set_sys_reg(vcpu->vcpu, r, v);
}

/* ---- lifecycle --------------------------------------------------------- */

int
vmm_available(void)
{
    int supported = 0;
    size_t len = sizeof supported;
    /* kern.hv_support is the documented way to ask whether the Hypervisor
     * framework can run on this host. */
    if (sysctlbyname("kern.hv_support", &supported, &len, NULL, 0) != 0)
        return 0;
    return supported != 0;
}

vmm *
vmm_new(void)
{
    vmm *vm = calloc(1, sizeof *vm);
    if (!vm)
        return NULL;
    if (hv_vm_create(NULL) != HV_SUCCESS) {
        free(vm);
        return NULL;
    }
    vm->created = 1;
    return vm;
}

void
vmm_free(vmm *vm)
{
    if (!vm)
        return;
    hv_vm_destroy();
    free(vm);
}

int
vmm_map(vmm *vm, void *host, uint64_t gpa, size_t size)
{
    (void)vm;
    /* RAM is readable, writable, and executable; the guest runs code out of it.
     * Only this slot is mapped, so the doorbell above RAM is unbacked and faults
     * out as a stage-2 data abort.  arm64 needs no host-built page tables: the
     * guest starts with its MMU off, an identity map by construction. */
    hv_memory_flags_t flags = HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC;
    if (hv_vm_map(host, gpa, size, flags) != HV_SUCCESS)
        return -1;
    /* Lay the identity page table into the base RAM slot; vmm_vcpu_new programs
     * the translation registers to point at it and turns the MMU on. */
    if (gpa == 0)
        skj_hv_a64_identity_pt(host);
    return 0;
}

vmm_vcpu *
vmm_vcpu_new(vmm *vm)
{
    (void)vm;
    vmm_vcpu *vcpu = calloc(1, sizeof *vcpu);
    if (!vcpu)
        return NULL;
    /* The vcpu must be created on the thread that later calls hv_vcpu_run. */
    if (hv_vcpu_create(&vcpu->vcpu, &vcpu->exit, NULL) != HV_SUCCESS) {
        free(vcpu);
        return NULL;
    }
    reg_set(vcpu, HV_REG_CPSR, PSTATE_EL1H_DAIF);

    /* Turn the MMU on with the flat identity map built in vmm_map, so guest RAM
     * is Normal memory (unaligned accesses work) rather than Device memory.
     * The table itself is already in guest RAM; here we point the translation
     * registers at it and enable translation and the caches. */
    sys_set(vcpu, HV_SYS_REG_MAIR_EL1, SKJ_HV_A64_MAIR);
    sys_set(vcpu, HV_SYS_REG_TCR_EL1, SKJ_HV_A64_TCR);
    sys_set(vcpu, HV_SYS_REG_TTBR0_EL1, SKJ_HV_A64_TTBR0);
    sys_set(vcpu, HV_SYS_REG_SCTLR_EL1,
            sys_get(vcpu, HV_SYS_REG_SCTLR_EL1) | SKJ_HV_A64_SCTLR_ON);
    return vcpu;
}

void
vmm_vcpu_free(vmm_vcpu *vcpu)
{
    if (!vcpu)
        return;
    hv_vcpu_destroy(vcpu->vcpu);
    free(vcpu);
}

void
vmm_vcpu_set_pc(vmm_vcpu *vcpu, uint64_t pc)
{
    reg_set(vcpu, HV_REG_PC, pc);
}

void
vmm_vcpu_set_sp(vmm_vcpu *vcpu, uint64_t sp)
{
    /* The guest runs at EL1, so its stack pointer is SP_EL1. */
    sys_set(vcpu, HV_SYS_REG_SP_EL1, sp);
}

/* ---- run --------------------------------------------------------------- */

/* Finish the MMIO access the previous run faulted on: deliver the load result
 * (filled by vmm_vcpu_syscall_ret) into its destination register, then step PC
 * past the faulting instruction.  Every AArch64 instruction is four bytes. */
static void
complete_mmio(vmm_vcpu *vcpu)
{
    if (!vcpu->mmio_write && vcpu->mmio_srt != 31) {
        uint64_t val = 0;
        memcpy(&val, vcpu->mmio_data, vcpu->mmio_bytes);
        if (vcpu->mmio_sign && vcpu->mmio_bytes < 8) {
            unsigned bits = (unsigned)vcpu->mmio_bytes * 8;
            if (val & (1ull << (bits - 1)))
                val |= ~0ull << bits;
        }
        if (!vcpu->mmio_wide)
            val &= 0xffffffffull;
        x_set(vcpu, (int)vcpu->mmio_srt, val);
    }
    reg_set(vcpu, HV_REG_PC, reg_get(vcpu, HV_REG_PC) + 4);
    vcpu->mmio_pending = 0;
}

void
vmm_vcpu_run(vmm_vcpu *vcpu, struct vmm_exit *ex)
{
    memset(ex, 0, sizeof *ex);
    if (vcpu->mmio_pending)
        complete_mmio(vcpu);

    if (hv_vcpu_run(vcpu->vcpu) != HV_SUCCESS) {
        ex->kind = VMM_X_ERROR;
        return;
    }

    /* Opt-in per-exit trace: set SKJ_HV_DEBUG to print one line per exit
     * (reason, exception class, ISS, pc, faulting address). */
    {
        static int dbg = -1;
        if (dbg < 0)
            dbg = getenv("SKJ_HV_DEBUG") != NULL;
        if (dbg) {
            uint64_t s = vcpu->exit->exception.syndrome;
            fprintf(stderr, "[hvf] reason=%u ec=0x%llx iss=0x%llx pc=0x%llx "
                    "pa=0x%llx | esr_el1=0x%llx elr_el1=0x%llx far_el1=0x%llx\n",
                    (unsigned)vcpu->exit->reason,
                    (unsigned long long)ESR_EC(s),
                    (unsigned long long)(s & 0x1ffffff),
                    (unsigned long long)reg_get(vcpu, HV_REG_PC),
                    (unsigned long long)vcpu->exit->exception.physical_address,
                    (unsigned long long)sys_get(vcpu, HV_SYS_REG_ESR_EL1),
                    (unsigned long long)sys_get(vcpu, HV_SYS_REG_ELR_EL1),
                    (unsigned long long)sys_get(vcpu, HV_SYS_REG_FAR_EL1));
        }
    }

    switch (vcpu->exit->reason) {
    case HV_EXIT_REASON_EXCEPTION: {
        uint64_t syndrome = vcpu->exit->exception.syndrome;
        switch (ESR_EC(syndrome)) {
        case EC_DATA_ABORT: {
            /* Data-abort ISS fields (ESR_EL2[24:0]):
             *   [24]    ISV  syndrome valid (set for normal loads/stores)
             *   [23:22] SAS  access size, log2 bytes
             *   [21]    SSE  sign-extend a load
             *   [20:16] SRT  transfer register (Xt), 31 == XZR
             *   [15]    SF   64-bit register
             *   [6]     WnR  write, not read */
            uint32_t iss = (uint32_t)(syndrome & 0x1ffffff);
            uint64_t pa = vcpu->exit->exception.physical_address;
            if (!((iss >> 24) & 0x1)) {
                /* No decoded syndrome: the seam cannot complete it. */
                ex->kind = VMM_X_ERROR;
                break;
            }
            vcpu->mmio_bytes = (uint8_t)(1u << ((iss >> 22) & 0x3));
            vcpu->mmio_sign = (iss >> 21) & 0x1;
            vcpu->mmio_srt = (iss >> 16) & 0x1f;
            vcpu->mmio_wide = (iss >> 15) & 0x1;
            vcpu->mmio_write = (iss >> 6) & 0x1;
            memset(vcpu->mmio_data, 0, sizeof vcpu->mmio_data);
            if (vcpu->mmio_write && vcpu->mmio_srt != 31) {
                /* Hand the store value to the core in the shared buffer. */
                uint64_t v = x_get(vcpu, (int)vcpu->mmio_srt);
                memcpy(vcpu->mmio_data, &v, vcpu->mmio_bytes);
            }
            vcpu->mmio_pending = 1;

            ex->mmio_addr = pa;
            ex->mmio_write = vcpu->mmio_write;
            ex->mmio_size = vcpu->mmio_bytes;
            ex->mmio_data = vcpu->mmio_data;
            /* The syscall doorbell is an unbacked MMIO read at a fixed address. */
            ex->kind = (!vcpu->mmio_write && pa == SKJ_HV_DOORBELL)
                           ? VMM_X_DOORBELL : VMM_X_MMIO;
            break;
        }
        case EC_WFI_WFE:
            ex->kind = VMM_X_HALT;
            break;
        case EC_SOFT_STEP:
        case EC_BRK_AARCH64:
            ex->kind = VMM_X_DEBUG;
            break;
        default:
            ex->kind = VMM_X_ERROR;
            break;
        }
        break;
    }
    case HV_EXIT_REASON_CANCELED:
    case HV_EXIT_REASON_VTIMER_ACTIVATED:
        /* Forced out by the preemption timer: one timeslice checkpoint. */
        ex->kind = VMM_X_INTR;
        break;
    default:
        ex->kind = VMM_X_ERROR;
        break;
    }
}

void
vmm_vcpu_syscall(vmm_vcpu *vcpu, uint64_t *nr, uint64_t args[6])
{
    int i;
    *nr = x_get(vcpu, 8);
    for (i = 0; i < 6; i++)
        args[i] = x_get(vcpu, i);
}

void
vmm_vcpu_syscall_ret(struct vmm_exit *ex, uint64_t ret)
{
    /* The doorbell load's destination register receives this on the next run.
     * Write the full eight bytes little-endian; the guest issues an 8-byte load. */
    int i;
    for (i = 0; i < 8 && i < ex->mmio_size; i++)
        ex->mmio_data[i] = (uint8_t)(ret >> (i * 8));
}

uint64_t
vmm_vcpu_pc(vmm_vcpu *vcpu)
{
    return reg_get(vcpu, HV_REG_PC);
}

/* ---- register dump + GDB support -------------------------------------- */

static const char *const a64_gpr_names[] = {
    "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",
    "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "x30", "pc",
};

const char *const *
vmm_gpreg_names(int *count)
{
    *count = (int)(sizeof a64_gpr_names / sizeof a64_gpr_names[0]);
    return a64_gpr_names;
}

int
vmm_vcpu_gpregs(vmm_vcpu *vcpu, uint64_t *out)
{
    int i, n = 0;
    for (i = 0; i < 31; i++)
        out[n++] = x_get(vcpu, i);
    out[n++] = reg_get(vcpu, HV_REG_PC);
    return n;
}

static void le64(uint8_t *p, uint64_t v) { int i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void le32(uint8_t *p, uint32_t v) { int i; for (i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; int i; for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v = 0; int i; for (i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); return v; }

int
vmm_vcpu_gdb_read_regs(vmm_vcpu *vcpu, uint8_t *out)
{
    /* The 268-byte aarch64 block: x0..x30, sp (SP_EL1), pc, then cpsr. */
    int i, n = 0;
    for (i = 0; i < 31; i++) { le64(out + n, x_get(vcpu, i)); n += 8; }
    le64(out + n, sys_get(vcpu, HV_SYS_REG_SP_EL1)); n += 8;
    le64(out + n, reg_get(vcpu, HV_REG_PC)); n += 8;
    le32(out + n, (uint32_t)reg_get(vcpu, HV_REG_CPSR)); n += 4;
    return n;                                   /* 268 */
}

void
vmm_vcpu_gdb_write_regs(vmm_vcpu *vcpu, const uint8_t *in, int len)
{
    int i;
    for (i = 0; i < 31 && (i + 1) * 8 <= len; i++)
        x_set(vcpu, i, rd64(in + i * 8));
    if (len >= 256) sys_set(vcpu, HV_SYS_REG_SP_EL1, rd64(in + 248));
    if (len >= 264) reg_set(vcpu, HV_REG_PC, rd64(in + 256));
    if (len >= 268) reg_set(vcpu, HV_REG_CPSR, rd32(in + 264));
}

void
vmm_vcpu_debug_singlestep(vmm_vcpu *vcpu, int on)
{
    /* Software single-step: arm MDSCR_EL1.SS and PSTATE.SS, and route guest
     * debug exceptions to the host so the completed step surfaces as an
     * EC_SOFT_STEP exception (mapped to VMM_X_DEBUG above).  Implemented by
     * analogy with the KVM guest-debug path; validated once HVF runs here. */
    uint64_t mdscr = sys_get(vcpu, HV_SYS_REG_MDSCR_EL1);
    uint64_t cpsr = reg_get(vcpu, HV_REG_CPSR);
    hv_vcpu_set_trap_debug_exceptions(vcpu->vcpu, on != 0);
    if (on) {
        sys_set(vcpu, HV_SYS_REG_MDSCR_EL1, mdscr | MDSCR_SS);
        reg_set(vcpu, HV_REG_CPSR, cpsr | PSTATE_SS);
    } else {
        sys_set(vcpu, HV_SYS_REG_MDSCR_EL1, mdscr & ~(uint64_t)MDSCR_SS);
        reg_set(vcpu, HV_REG_CPSR, cpsr & ~PSTATE_SS);
    }
    vcpu->stepping = on != 0;
}

/* ---- preemption timer -------------------------------------------------- */

static void
preempt_tick(int sig)
{
    (void)sig;
    /* Kick the running vcpu out of hv_vcpu_run; it returns with
     * HV_EXIT_REASON_CANCELED, which the run loop reports as VMM_X_INTR. */
    if (g_preempt_armed)
        hv_vcpus_exit(&g_preempt_vcpu, 1);
}

void
vmm_vcpu_arm_preempt(vmm_vcpu *vcpu, unsigned usec)
{
    static int installed = 0;
    struct itimerval it;

    if (!installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = preempt_tick;       /* no SA_RESTART needed: it only kicks */
        sigaction(SIGALRM, &sa, NULL);
        installed = 1;
    }

    g_preempt_vcpu = vcpu->vcpu;
    g_preempt_armed = usec != 0;

    memset(&it, 0, sizeof it);
    it.it_interval.tv_sec = usec / 1000000u;
    it.it_interval.tv_usec = usec % 1000000u;
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);
}

#else /* !VMM_HVF_OK : Darwin on a non-arm64 host, or a non-Darwin build */

#include <stdlib.h>

int vmm_available(void) { return 0; }
vmm *vmm_new(void) { return NULL; }
void vmm_free(vmm *vm) { (void)vm; }
int vmm_map(vmm *vm, void *h, uint64_t g, size_t s)
{ (void)vm; (void)h; (void)g; (void)s; return -1; }
vmm_vcpu *vmm_vcpu_new(vmm *vm) { (void)vm; return NULL; }
void vmm_vcpu_free(vmm_vcpu *v) { (void)v; }
void vmm_vcpu_set_pc(vmm_vcpu *v, uint64_t p) { (void)v; (void)p; }
void vmm_vcpu_set_sp(vmm_vcpu *v, uint64_t s) { (void)v; (void)s; }
void vmm_vcpu_run(vmm_vcpu *v, struct vmm_exit *ex)
{ (void)v; ex->kind = VMM_X_ERROR; }
void vmm_vcpu_syscall(vmm_vcpu *v, uint64_t *nr, uint64_t a[6])
{ (void)v; (void)a; *nr = (uint64_t)-1; }
void vmm_vcpu_syscall_ret(struct vmm_exit *ex, uint64_t r) { (void)ex; (void)r; }
uint64_t vmm_vcpu_pc(vmm_vcpu *v) { (void)v; return 0; }
int vmm_vcpu_gpregs(vmm_vcpu *v, uint64_t *o) { (void)v; (void)o; return 0; }
const char *const *vmm_gpreg_names(int *count) { *count = 0; return NULL; }
int vmm_vcpu_gdb_read_regs(vmm_vcpu *v, uint8_t *o) { (void)v; (void)o; return 0; }
void vmm_vcpu_gdb_write_regs(vmm_vcpu *v, const uint8_t *i, int l) { (void)v; (void)i; (void)l; }
void vmm_vcpu_debug_singlestep(vmm_vcpu *v, int on) { (void)v; (void)on; }
void vmm_vcpu_arm_preempt(vmm_vcpu *v, unsigned usec) { (void)v; (void)usec; }

#endif /* VMM_HVF_OK */
