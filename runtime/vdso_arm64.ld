/* vdso_arm64.ld : link the AArch64 skjegg vDSO image at the fixed guest address
 * SKJ_HV_VDSO (0x20000000, see emu/skj_hv_abi.h). It sits above the HV guest's
 * RAM but inside the identity-mapped first gigabyte, so the host backs it with
 * one read-only memory slot shared across every VM, and the guest reaches it
 * through its own flat page tables. The entry is the table only to satisfy the
 * format; it is never run. */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(skj_vdso_table)
MEMORY { VDSO (rx) : ORIGIN = 0x20000000, LENGTH = 0x10000 }
SECTIONS {
    .text : { *(.text .text.*) } > VDSO
    .rodata : { *(.rodata .rodata.*) } > VDSO
    .data : { *(.data .data.*) } > VDSO
    /DISCARD/ : { *(.note .note.*) *(.comment) *(.eh_frame)
                  *(.gnu.hash) *(.bss .bss.*) *(COMMON) }
}
