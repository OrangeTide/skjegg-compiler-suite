/* vmm_kvm.c : the Linux KVM backend of the VMM seam (see vmm.h)
 *
 * An identity-mapped, flat-segment 64-bit supervisor guest with one RAM slot.
 * The only guest/host contract is the syscall doorbell of skj_hv_abi.h.  The
 * macOS Hypervisor.framework backend (vmm_hvf.c) is the sibling of this file.
 * Names from <linux/kvm.h> stay as the kernel spells them; the seam's own
 * identifiers are vmm_*. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "vmm.h"
#include "skj_hv_abi.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#define VMM_KVM_OK 1
#include <linux/kvm.h>
#else
#define VMM_KVM_OK 0
#endif

#if VMM_KVM_OK

/* The preemption-timer signal.  It is blocked in the thread's normal mask and
 * unblocked only during KVM_RUN (via KVM_SET_SIGNAL_MASK), so it interrupts the
 * guest and nothing else.  A trivial handler without SA_RESTART is enough to
 * make the KVM_RUN ioctl return -EINTR; delivery alone is the signal. */
#define VMM_PREEMPT_SIG SIGALRM

/* x86-64 long-mode page-table entry flags. */
#define PTE_P     0x1
#define PTE_RW    0x2
#define PTE_PS    0x80      /* a 2 MiB page */

struct vmm {
    int kvm_fd;
    int vm_fd;
    uint32_t next_slot;
    int next_vcpu;
};

struct vmm_vcpu {
    int kvm_fd;
    int vcpu_fd;
    struct kvm_run *run;
    size_t run_size;
};

#if defined(__aarch64__)
/* The id of an AArch64 core register for KVM_{GET,SET}_ONE_REG. */
#define A64_CORE(field) \
    (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE | \
     (offsetof(struct kvm_regs, field) / sizeof(uint32_t)))

static uint64_t
a64_get(int fd, uint64_t id)
{
    uint64_t v = 0;
    struct kvm_one_reg one = { .id = id, .addr = (uint64_t)(uintptr_t)&v };
    ioctl(fd, KVM_GET_ONE_REG, &one);
    return v;
}

static void
a64_set(int fd, uint64_t id, uint64_t v)
{
    struct kvm_one_reg one = { .id = id, .addr = (uint64_t)(uintptr_t)&v };
    ioctl(fd, KVM_SET_ONE_REG, &one);
}
#endif

int
vmm_available(void)
{
    int fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

vmm *
vmm_new(void)
{
    vmm *vm = calloc(1, sizeof(*vm));
    if (!vm)
        return NULL;
    vm->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (vm->kvm_fd < 0) {
        free(vm);
        return NULL;
    }
    vm->vm_fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 0);
    if (vm->vm_fd < 0) {
        close(vm->kvm_fd);
        free(vm);
        return NULL;
    }
    return vm;
}

void
vmm_free(vmm *vm)
{
    if (!vm)
        return;
    if (vm->vm_fd >= 0)
        close(vm->vm_fd);
    if (vm->kvm_fd >= 0)
        close(vm->kvm_fd);
    free(vm);
}

int
vmm_map(vmm *vm, void *host, uint64_t gpa, size_t size)
{
    struct kvm_userspace_memory_region region = {
        .slot = vm->next_slot++,
        .flags = 0,
        .guest_phys_addr = gpa,
        .memory_size = size,
        .userspace_addr = (uint64_t)(uintptr_t)host,
    };
    if (ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
        return -1;

#if defined(__x86_64__)
    /* Long mode needs page tables in guest RAM. Build an identity map covering
       the first 1 GiB: a single page directory of 512 two-megabyte pages. RAM
       faults normally; the doorbell above RAM is present but unbacked, so it
       faults out as MMIO. CR3 points here for every vcpu. */
    if (gpa == 0) {
        uint8_t *base = host;
        uint64_t *pml4 = (uint64_t *)(base + SKJ_HV_PT_PML4);
        uint64_t *pdpt = (uint64_t *)(base + SKJ_HV_PT_PDPT);
        uint64_t *pd = (uint64_t *)(base + SKJ_HV_PT_PD);
        int i;
        pml4[0] = SKJ_HV_PT_PDPT | PTE_P | PTE_RW;
        pdpt[0] = SKJ_HV_PT_PD | PTE_P | PTE_RW;
        for (i = 0; i < 512; i++)
            pd[i] = ((uint64_t)i << 21) | PTE_P | PTE_RW | PTE_PS;
    }
#elif defined(__aarch64__)
    /* The guest must run with its MMU on, or every access is Device memory
       where the compiler's unaligned loads fault.  Lay the flat identity map
       into guest RAM; vmm_vcpu_new programs the translation registers to point
       at it.  The doorbell above RAM is mapped at stage 1 but unbacked, so it
       still faults out as MMIO. */
    if (gpa == 0)
        skj_hv_a64_identity_pt(host);
#endif
    return 0;
}

vmm_vcpu *
vmm_vcpu_new(vmm *vm)
{
    vmm_vcpu *vcpu = calloc(1, sizeof(*vcpu));
    if (!vcpu)
        return NULL;
    vcpu->kvm_fd = vm->kvm_fd;
    vcpu->vcpu_fd = ioctl(vm->vm_fd, KVM_CREATE_VCPU, (unsigned long)vm->next_vcpu++);
    if (vcpu->vcpu_fd < 0) {
        free(vcpu);
        return NULL;
    }

#if defined(__x86_64__)
    {
        struct kvm_sregs sregs;
        struct kvm_regs regs;
        struct kvm_segment code = {
            .base = 0, .limit = 0xffffffff, .selector = 0x08,
            .type = 0xb, .present = 1, .dpl = 0, .db = 0, .s = 1, .l = 1, .g = 1,
        };
        struct kvm_segment data = {
            .base = 0, .limit = 0xffffffff, .selector = 0x10,
            .type = 0x3, .present = 1, .dpl = 0, .db = 1, .s = 1, .l = 0, .g = 1,
        };
        if (ioctl(vcpu->vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
            goto fail;
        /* Flat segments straight through the hidden descriptor cache, so no GDT
           lives in guest memory as long as the guest never reloads a segment. */
        sregs.cr3 = SKJ_HV_PT_PML4;
        sregs.cr4 |= (1u << 5);                 /* CR4.PAE */
        sregs.cr4 |= (1u << 9) | (1u << 10);    /* CR4.OSFXSR | OSXMMEXCPT: SSE */
        sregs.cr0 |= (1u << 0) | (1u << 31);    /* CR0.PE | CR0.PG */
        sregs.cr0 |= (1u << 1);                 /* CR0.MP */
        sregs.cr0 &= ~(1u << 2);                /* clear CR0.EM so SSE is not #UD */
        sregs.efer |= (1u << 8) | (1u << 10);   /* EFER.LME | EFER.LMA */
        sregs.cs = code;
        sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = data;
        if (ioctl(vcpu->vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
            goto fail;
        memset(&regs, 0, sizeof(regs));
        regs.rflags = 0x2;
        if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &regs) < 0)
            goto fail;
    }
#elif defined(__aarch64__)
    {
        struct kvm_vcpu_init init;
        uint64_t sctlr;
        if (ioctl(vm->vm_fd, KVM_ARM_PREFERRED_TARGET, &init) < 0)
            goto fail;
        if (ioctl(vcpu->vcpu_fd, KVM_ARM_VCPU_INIT, &init) < 0)
            goto fail;
        /* Turn the MMU on with the flat identity map built in vmm_map, so guest
           RAM is Normal memory.  Program the translation registers with the
           shared SKJ_HV_A64_* values; the sysreg ids are ARM64_SYS_REG(op0, op1,
           crn, crm, op2) as the psABI encodes them. */
        a64_set(vcpu->vcpu_fd, ARM64_SYS_REG(3, 0, 10, 2, 0), SKJ_HV_A64_MAIR);
        a64_set(vcpu->vcpu_fd, ARM64_SYS_REG(3, 0, 2, 0, 2), SKJ_HV_A64_TCR);
        a64_set(vcpu->vcpu_fd, ARM64_SYS_REG(3, 0, 2, 0, 0), SKJ_HV_A64_TTBR0);
        sctlr = a64_get(vcpu->vcpu_fd, ARM64_SYS_REG(3, 0, 1, 0, 0));
        a64_set(vcpu->vcpu_fd, ARM64_SYS_REG(3, 0, 1, 0, 0),
                sctlr | SKJ_HV_A64_SCTLR_ON);
    }
#endif

    vcpu->run_size = (size_t)ioctl(vcpu->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if ((ssize_t)vcpu->run_size <= 0)
        goto fail;
    vcpu->run = mmap(NULL, vcpu->run_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     vcpu->vcpu_fd, 0);
    if (vcpu->run == MAP_FAILED)
        goto fail;
    return vcpu;

fail:
    if (vcpu->vcpu_fd >= 0)
        close(vcpu->vcpu_fd);
    free(vcpu);
    return NULL;
}

void
vmm_vcpu_free(vmm_vcpu *vcpu)
{
    if (!vcpu)
        return;
    if (vcpu->run && vcpu->run != MAP_FAILED)
        munmap(vcpu->run, vcpu->run_size);
    if (vcpu->vcpu_fd >= 0)
        close(vcpu->vcpu_fd);
    free(vcpu);
}

void
vmm_vcpu_set_pc(vmm_vcpu *vcpu, uint64_t pc)
{
#if defined(__x86_64__)
    struct kvm_regs regs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0)
        return;
    regs.rip = pc;
    ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &regs);
#elif defined(__aarch64__)
    a64_set(vcpu->vcpu_fd, A64_CORE(regs.pc), pc);
#endif
}

void
vmm_vcpu_set_sp(vmm_vcpu *vcpu, uint64_t sp)
{
#if defined(__x86_64__)
    struct kvm_regs regs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0)
        return;
    regs.rsp = sp;
    ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &regs);
#elif defined(__aarch64__)
    a64_set(vcpu->vcpu_fd, A64_CORE(sp_el1), sp);
#endif
}

void
vmm_vcpu_run(vmm_vcpu *vcpu, struct vmm_exit *ex)
{
    struct kvm_run *run = vcpu->run;

    memset(ex, 0, sizeof(*ex));
    if (ioctl(vcpu->vcpu_fd, KVM_RUN, 0) < 0) {
        if (errno == EINTR) {
            /* The preemption signal broke the run.  It is blocked in the
             * thread's own mask (so it never disturbs host-side work) and KVM
             * restores that mask before returning, so the handler has not run
             * and the signal is still pending.  Drain it here, or the very next
             * KVM_RUN would see it pending and return EINTR at once, livelocking
             * with the guest making no progress. */
            static const struct timespec z = { 0, 0 };
            sigset_t a;
            sigemptyset(&a);
            sigaddset(&a, VMM_PREEMPT_SIG);
            while (sigtimedwait(&a, NULL, &z) > 0)
                ;
            ex->kind = VMM_X_INTR;
        } else {
            ex->kind = VMM_X_ERROR;
        }
        return;
    }

    switch (run->exit_reason) {
    case KVM_EXIT_MMIO:
        ex->mmio_addr = run->mmio.phys_addr;
        ex->mmio_write = run->mmio.is_write;
        ex->mmio_size = (uint8_t)run->mmio.len;
        ex->mmio_data = run->mmio.data;
        /* The syscall doorbell is an unbacked MMIO read at a fixed address. */
        ex->kind = (!run->mmio.is_write && run->mmio.phys_addr == SKJ_HV_DOORBELL)
                       ? VMM_X_DOORBELL : VMM_X_MMIO;
        break;
    case KVM_EXIT_HLT:
        ex->kind = VMM_X_HALT;
        break;
    case KVM_EXIT_INTR:
    case KVM_EXIT_IRQ_WINDOW_OPEN:
        ex->kind = VMM_X_INTR;
        break;
    case KVM_EXIT_DEBUG:
        ex->kind = VMM_X_DEBUG;
        break;
    default:
        ex->kind = VMM_X_ERROR;
        break;
    }
}

void
vmm_vcpu_syscall(vmm_vcpu *vcpu, uint64_t *nr, uint64_t args[6])
{
#if defined(__x86_64__)
    struct kvm_regs regs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0) {
        *nr = (uint64_t)-1;
        return;
    }
    *nr = regs.rax;
    args[0] = regs.rdi;
    args[1] = regs.rsi;
    args[2] = regs.rdx;
    args[3] = regs.r10;
    args[4] = regs.r8;
    args[5] = regs.r9;
#elif defined(__aarch64__)
    *nr = a64_get(vcpu->vcpu_fd, A64_CORE(regs.regs[8]));
    args[0] = a64_get(vcpu->vcpu_fd, A64_CORE(regs.regs[0]));
    args[1] = a64_get(vcpu->vcpu_fd, A64_CORE(regs.regs[1]));
    args[2] = a64_get(vcpu->vcpu_fd, A64_CORE(regs.regs[2]));
    args[3] = a64_get(vcpu->vcpu_fd, A64_CORE(regs.regs[3]));
    args[4] = a64_get(vcpu->vcpu_fd, A64_CORE(regs.regs[4]));
    args[5] = a64_get(vcpu->vcpu_fd, A64_CORE(regs.regs[5]));
#endif
}

void
vmm_vcpu_syscall_ret(struct vmm_exit *ex, uint64_t ret)
{
    /* The doorbell load's destination register receives this on the next run.
       Write the full eight bytes little-endian; the guest always issues an
       eight-byte load. */
    int i;
    for (i = 0; i < 8 && i < ex->mmio_size; i++)
        ex->mmio_data[i] = (uint8_t)(ret >> (i * 8));
}

uint64_t
vmm_vcpu_pc(vmm_vcpu *vcpu)
{
#if defined(__x86_64__)
    struct kvm_regs regs;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs) < 0)
        return 0;
    return regs.rip;
#elif defined(__aarch64__)
    return a64_get(vcpu->vcpu_fd, A64_CORE(regs.pc));
#endif
}

#if defined(__x86_64__)
static const char *const x86_gpr_names[] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rsp", "rbp",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
    "rip", "rflags",
};
#elif defined(__aarch64__)
static const char *const a64_gpr_names[] = {
    "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",
    "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "x30", "pc",
};
#endif

const char *const *
vmm_gpreg_names(int *count)
{
#if defined(__x86_64__)
    *count = (int)(sizeof(x86_gpr_names) / sizeof(x86_gpr_names[0]));
    return x86_gpr_names;
#elif defined(__aarch64__)
    *count = (int)(sizeof(a64_gpr_names) / sizeof(a64_gpr_names[0]));
    return a64_gpr_names;
#endif
}

int
vmm_vcpu_gpregs(vmm_vcpu *vcpu, uint64_t *out)
{
#if defined(__x86_64__)
    struct kvm_regs r;
    int n = 0;
    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &r) < 0)
        return 0;
    out[n++] = r.rax; out[n++] = r.rbx; out[n++] = r.rcx; out[n++] = r.rdx;
    out[n++] = r.rsi; out[n++] = r.rdi; out[n++] = r.rsp; out[n++] = r.rbp;
    out[n++] = r.r8;  out[n++] = r.r9;  out[n++] = r.r10; out[n++] = r.r11;
    out[n++] = r.r12; out[n++] = r.r13; out[n++] = r.r14; out[n++] = r.r15;
    out[n++] = r.rip; out[n++] = r.rflags;
    return n;
#elif defined(__aarch64__)
    int i, n = 0;
    for (i = 0; i < 31; i++)
        out[n++] = a64_get(vcpu->vcpu_fd, A64_CORE(regs.regs[i]));
    out[n++] = a64_get(vcpu->vcpu_fd, A64_CORE(regs.pc));
    return n;
#endif
}

/****************************************************************
 * GDB remote-protocol support
 ****************************************************************/

static void le64(uint8_t *p, uint64_t v) { int i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void le32(uint8_t *p, uint32_t v) { int i; for (i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; int i; for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v = 0; int i; for (i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); return v; }

int
vmm_vcpu_gdb_read_regs(vmm_vcpu *vcpu, uint8_t *out)
{
#if defined(__x86_64__)
    struct kvm_regs r;
    struct kvm_sregs sr;
    uint64_t gp[17];
    int i, n = 0;

    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &r) < 0 ||
        ioctl(vcpu->vcpu_fd, KVM_GET_SREGS, &sr) < 0)
        return 0;
    gp[0]=r.rax; gp[1]=r.rbx; gp[2]=r.rcx; gp[3]=r.rdx;
    gp[4]=r.rsi; gp[5]=r.rdi; gp[6]=r.rbp; gp[7]=r.rsp;
    gp[8]=r.r8;  gp[9]=r.r9;  gp[10]=r.r10; gp[11]=r.r11;
    gp[12]=r.r12; gp[13]=r.r13; gp[14]=r.r14; gp[15]=r.r15; gp[16]=r.rip;
    for (i = 0; i < 17; i++) { le64(out + n, gp[i]); n += 8; }
    le32(out + n, (uint32_t)r.rflags); n += 4;
    le32(out + n, sr.cs.selector); n += 4;
    le32(out + n, sr.ss.selector); n += 4;
    le32(out + n, sr.ds.selector); n += 4;
    le32(out + n, sr.es.selector); n += 4;
    le32(out + n, sr.fs.selector); n += 4;
    le32(out + n, sr.gs.selector); n += 4;
    return n;                                   /* 164 */
#elif defined(__aarch64__)
    int i, n = 0;
    for (i = 0; i < 31; i++) { le64(out + n, a64_get(vcpu->vcpu_fd, A64_CORE(regs.regs[i]))); n += 8; }
    le64(out + n, a64_get(vcpu->vcpu_fd, A64_CORE(sp_el1))); n += 8;
    le64(out + n, a64_get(vcpu->vcpu_fd, A64_CORE(regs.pc))); n += 8;
    le32(out + n, (uint32_t)a64_get(vcpu->vcpu_fd, A64_CORE(regs.pstate))); n += 4;
    return n;                                   /* 268 */
#endif
}

void
vmm_vcpu_gdb_write_regs(vmm_vcpu *vcpu, const uint8_t *in, int len)
{
#if defined(__x86_64__)
    struct kvm_regs r;

    if (len < 136 || ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &r) < 0)
        return;
    r.rax = rd64(in + 0*8);  r.rbx = rd64(in + 1*8);  r.rcx = rd64(in + 2*8);
    r.rdx = rd64(in + 3*8);  r.rsi = rd64(in + 4*8);  r.rdi = rd64(in + 5*8);
    r.rbp = rd64(in + 6*8);  r.rsp = rd64(in + 7*8);  r.r8  = rd64(in + 8*8);
    r.r9  = rd64(in + 9*8);  r.r10 = rd64(in + 10*8); r.r11 = rd64(in + 11*8);
    r.r12 = rd64(in + 12*8); r.r13 = rd64(in + 13*8); r.r14 = rd64(in + 14*8);
    r.r15 = rd64(in + 15*8); r.rip = rd64(in + 16*8);
    if (len >= 140)
        r.rflags = rd32(in + 136) | 0x2;
    ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &r);     /* segment selectors kept as-is */
#elif defined(__aarch64__)
    int i;
    for (i = 0; i < 31 && (i + 1) * 8 <= len; i++)
        a64_set(vcpu->vcpu_fd, A64_CORE(regs.regs[i]), rd64(in + i * 8));
    if (len >= 256) a64_set(vcpu->vcpu_fd, A64_CORE(sp_el1), rd64(in + 248));
    if (len >= 264) a64_set(vcpu->vcpu_fd, A64_CORE(regs.pc), rd64(in + 256));
    if (len >= 268) a64_set(vcpu->vcpu_fd, A64_CORE(regs.pstate), rd32(in + 264));
#endif
}

void
vmm_vcpu_debug_singlestep(vmm_vcpu *vcpu, int on)
{
    struct kvm_guest_debug dbg;

    memset(&dbg, 0, sizeof dbg);
    if (on)
        dbg.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
    ioctl(vcpu->vcpu_fd, KVM_SET_GUEST_DEBUG, &dbg);
}

static void
preempt_noop(int sig)
{
    (void)sig;
}

void
vmm_vcpu_arm_preempt(vmm_vcpu *vcpu, unsigned usec)
{
    static int installed = 0;
    struct itimerval it;
    sigset_t run_mask;
    struct {
        struct kvm_signal_mask hdr;
        uint8_t set[8];                 /* the kernel's sigset is 8 bytes */
    } km;

    if (!installed) {
        struct sigaction sa;
        sigset_t block;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = preempt_noop;   /* no SA_RESTART: KVM_RUN gets EINTR */
        sigaction(VMM_PREEMPT_SIG, &sa, NULL);
        /* Block it in the thread's own mask; KVM unblocks it only while the
         * guest runs, so host-side syscalls are never interrupted. */
        sigemptyset(&block);
        sigaddset(&block, VMM_PREEMPT_SIG);
        sigprocmask(SIG_BLOCK, &block, NULL);
        installed = 1;
    }

    /* The run mask lists the signals blocked during KVM_RUN: the thread's
     * current mask with the preempt signal cleared, so it (and only it) can
     * break into the guest. */
    sigprocmask(SIG_BLOCK, NULL, &run_mask);
    sigdelset(&run_mask, VMM_PREEMPT_SIG);
    memset(&km, 0, sizeof km);
    km.hdr.len = sizeof km.set;
    memcpy(km.hdr.sigset, &run_mask, sizeof km.set);
    ioctl(vcpu->vcpu_fd, KVM_SET_SIGNAL_MASK, &km.hdr);

    /* (Re)arm the process interval timer, or disarm it when usec is zero. */
    memset(&it, 0, sizeof it);
    it.it_interval.tv_sec = usec / 1000000u;
    it.it_interval.tv_usec = usec % 1000000u;
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);
}

#else /* !VMM_KVM_OK : a host without KVM or an unsupported architecture */

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

#endif /* VMM_KVM_OK */
