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

/* AArch64 identity MMU.  Unlike x86-64, a 64-bit arm64 guest can run with its
 * MMU off, but then every access is Device-nGnRnE, where an unaligned load or
 * store faults.  The compilers emit such accesses (e.g. an eight-byte load of a
 * four-byte-aligned string word), so the guest needs the MMU on with RAM marked
 * Normal memory.  The host builds a flat identity map covering the low 1 GiB
 * (RAM, the page-table frame, and the doorbell) with one level-1 block in a
 * single 4 KiB table, then programs the translation registers before the first
 * run, the counterpart of writing CR3 and the page tables on x86-64.  The
 * doorbell at 768 MiB is mapped at stage 1 but unbacked at stage 2, so it still
 * faults out as MMIO. */
#define SKJ_HV_A64_TTBR0  0x1000u    /* one 4 KiB level-1 table, below the image */

/* The level-1 block descriptor for VA 0..1 GiB -> PA 0..1 GiB:
 *   [1:0]=0b01 valid block, [4:2]=AttrIndx 0, [7:6]=AP 0 (EL1 RW),
 *   [9:8]=SH 0b11 inner shareable, [10]=AF 1. */
#define SKJ_HV_A64_L1_BLOCK  0x701ull

/* MAIR_EL1: Attr0 = 0xFF, Normal memory, inner/outer write-back cacheable. */
#define SKJ_HV_A64_MAIR   0xffull

/* TCR_EL1: 4 KiB granule (TG0=0), T0SZ=25 (a 39-bit VA, a level-1 start),
 * inner/outer write-back write-allocate (IRGN0=ORGN0=0b01), inner shareable
 * (SH0=0b11), TTBR1 walks disabled (EPD1=1), 40-bit IPS (0b010). */
#define SKJ_HV_A64_TCR \
    (25ull | (1ull << 8) | (1ull << 10) | (3ull << 12) | \
     (1ull << 23) | (2ull << 32))

/* SCTLR_EL1 bits to set: M (MMU on), C (data cache), I (instruction cache). */
#define SKJ_HV_A64_SCTLR_ON  (0x1u | 0x4u | 0x1000u)

#if defined(__aarch64__)
#include <stdint.h>
/* Write the identity page table into guest RAM (the host's backing buffer for
 * guest-physical 0).  Shared by the KVM and HVF backends so both lay out the
 * table identically; each programs the translation registers with the
 * SKJ_HV_A64_* values through its own register interface (KVM_SET_ONE_REG on
 * KVM, hv_vcpu_set_sys_reg on HVF).  Without this the guest runs MMU-off, where
 * every access is Device memory and the compiler's unaligned loads fault. */
static inline void
skj_hv_a64_identity_pt(void *ram_base)
{
    uint64_t *l1 = (uint64_t *)((uint8_t *)ram_base + SKJ_HV_A64_TTBR0);
    l1[0] = SKJ_HV_A64_L1_BLOCK;
}
#endif

#endif /* SKJ_HV_ABI_H */
