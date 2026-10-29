/* vmm.h : a small hardware-virtualization seam for the HV cores
 *
 * This is the backend-neutral abstraction the HV cores (emu/hvcore.c) drive.
 * It has one backend per host hypervisor: Linux KVM (emu/vmm_kvm.c) and, on
 * macOS, Hypervisor.framework (emu/vmm_hvf.c).  A host without a supported
 * hypervisor links a stub that reports the VMM unavailable.  The name stays
 * clear of the kernel's own `kvm_*` API, which a backend uses internally.
 *
 * The guest architecture equals the build host's, since cross-architecture
 * virtualization is not possible: an x86-64 host drives an x86-64 guest, an
 * arm64 host an arm64 guest.
 *
 * Create a VM, map one RAM slot, create a vcpu already in 64-bit supervisor
 * mode on an identity map, set the entry and stack, run until the guest faults
 * out, and read or write its general registers. */

#ifndef SKJ_VMM_H
#define SKJ_VMM_H

#include <stddef.h>
#include <stdint.h>

typedef struct vmm vmm;
typedef struct vmm_vcpu vmm_vcpu;

/* Why a run stopped. The MMIO case carries the faulting address so the core
 * can tell the syscall doorbell from any other unbacked access. */
enum vmm_exit_kind {
    VMM_X_DOORBELL,   /* an MMIO read at SKJ_HV_DOORBELL: a guest syscall */
    VMM_X_MMIO,       /* any other MMIO access (an error for our guests) */
    VMM_X_HALT,       /* the guest halted (HLT / WFI) */
    VMM_X_INTR,       /* broken by a signal or interrupt window; run again */
    VMM_X_DEBUG,      /* a single-step completed (guest-debug) */
    VMM_X_ERROR,      /* the hypervisor reported a fault we do not model */
};

struct vmm_exit {
    enum vmm_exit_kind kind;
    uint64_t mmio_addr;     /* valid for VMM_X_DOORBELL / VMM_X_MMIO */
    int mmio_write;
    uint8_t mmio_size;
    uint8_t *mmio_data;     /* `mmio_size` bytes inside the shared run page */
};

/* true when the VMM is usable on this host (a hypervisor device opens and the
 * build arch is supported). The HV core reports a clean error otherwise. */
int vmm_available(void);

vmm *vmm_new(void);
void vmm_free(vmm *v);

/* Map `size` bytes of host memory at guest-physical `gpa`. For gpa 0 on x86-64
 * this also writes the long-mode identity page tables into the backing buffer.
 * Returns 0 on success. */
int vmm_map(vmm *v, void *host, uint64_t gpa, size_t size);

vmm_vcpu *vmm_vcpu_new(vmm *v);
void vmm_vcpu_free(vmm_vcpu *vcpu);

void vmm_vcpu_set_pc(vmm_vcpu *vcpu, uint64_t pc);
void vmm_vcpu_set_sp(vmm_vcpu *vcpu, uint64_t sp);

/* Run until the guest faults out; fills `*ex`. */
void vmm_vcpu_run(vmm_vcpu *vcpu, struct vmm_exit *ex);

/* The syscall bridge, in the platform's Linux ABI numbering. `nr` receives the
 * syscall number register; `args[0..5]` the six argument registers. Valid at a
 * VMM_X_DOORBELL exit, before the run that completes the doorbell load. */
void vmm_vcpu_syscall(vmm_vcpu *vcpu, uint64_t *nr, uint64_t args[6]);

/* Deliver the syscall result as the doorbell load's MMIO data. The next run
 * places it in the guest's return register. Call once per doorbell exit. */
void vmm_vcpu_syscall_ret(struct vmm_exit *ex, uint64_t ret);

/* The guest program counter, for diagnostics and the fault report. */
uint64_t vmm_vcpu_pc(vmm_vcpu *vcpu);

/* Read all general registers into a caller buffer of VMM_NGPR uint64_t, in the
 * platform's own order, for a register dump. Returns the count written. */
#define VMM_NGPR 32
int vmm_vcpu_gpregs(vmm_vcpu *vcpu, uint64_t *out);
const char *const *vmm_gpreg_names(int *count);

/* The GDB remote-protocol support for the HV cores.
 *
 * The register block matches gdb's built-in layout for the architecture (gdb
 * learns the arch from the ELF, so no target description is sent): x86-64 is the
 * 164-byte amd64 block (rax..r15, rip, eflags, then the six segment selectors);
 * aarch64 the 268-byte block (x0..x30, sp, pc, cpsr). read fills `out` (at least
 * VMM_GDB_REGBYTES bytes) with the raw little-endian bytes and returns the
 * count; write consumes the same layout (segment selectors ignored). */
#define VMM_GDB_REGBYTES 512
int  vmm_vcpu_gdb_read_regs(vmm_vcpu *vcpu, uint8_t *out);
void vmm_vcpu_gdb_write_regs(vmm_vcpu *vcpu, const uint8_t *in, int len);

/* Turn single-stepping on or off (the hypervisor's guest-debug facility); a run
 * then stops with VMM_X_DEBUG after each instruction. */
void vmm_vcpu_debug_singlestep(vmm_vcpu *vcpu, int on);

/* Arm a periodic preemption timer so a compute-bound guest (one that never
 * traps to a doorbell) still yields the CPU.  A host timer delivers a signal
 * every `usec` microseconds; the signal is blocked everywhere except inside the
 * guest run, so it interrupts only KVM_RUN (never host-side work or a guest
 * fault handler) and surfaces as a VMM_X_INTR exit, which the core treats as a
 * timeslice checkpoint.  The process-global parts (the signal handler and the
 * timer) are set up once; the per-vcpu run mask is set on every call.  Passing
 * usec == 0 disarms the timer.  A best-effort no-op where the host lacks the
 * facility.  See the HV core (emu/hvcore.c) for the slice policy. */
void vmm_vcpu_arm_preempt(vmm_vcpu *vcpu, unsigned usec);

#endif /* SKJ_VMM_H */
