/* skj_hv_abi.h : the guest/host contract for the hardware-virtualization cores
 *
 * A guest reaches the sandbox syscall layer through one trap: an eight-byte
 * load from SKJ_HV_DOORBELL, an unbacked guest-physical address, which faults
 * out as an MMIO read. The syscall number is in the number register and the
 * arguments in the argument registers, in the platform's Linux syscall ABI:
 *   x86-64: nr=rax, args=rdi,rsi,rdx,r10,r8,r9, the load targets rax
 *   arm64 : nr=x8,  args=x0..x5,                 the load targets x0
 * The host returns the result as the MMIO read data, which the kernel places in
 * the load's destination register, advancing past the instruction.
 *
 * The guest image, heap, and stack live under 4 GiB, so the host's 32-bit
 * guest-memory index addresses them directly. */

#ifndef SKJ_HV_ABI_H
#define SKJ_HV_ABI_H

/* Guest RAM: mapped as one slot at guest-physical 0. The identity page tables
 * (x86-64) cover the first 1 GiB, so the doorbell sits above RAM but inside the
 * mapped range, where it is present but unbacked and therefore faults as MMIO. */
#define SKJ_HV_RAM_SIZE   0x10000000u   /* 256 MiB */
#define SKJ_HV_DOORBELL   0x30000000u   /* 768 MiB: above RAM, below the 1 GiB map */

/* The low-memory page-table frames (x86-64 long mode). Kept below the guest
 * image, which links no lower than 1 MiB. */
#define SKJ_HV_PT_PML4    0x1000u
#define SKJ_HV_PT_PDPT    0x2000u
#define SKJ_HV_PT_PD      0x3000u

#endif /* SKJ_HV_ABI_H */
