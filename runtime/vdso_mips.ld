/* vdso_mips.ld : link the MIPS skjegg vDSO image at a fixed guest address, clear
 * of the program (0x00400000..), its heap, and the mmap area (0x70000000). The
 * host maps it here read-only and passes the address of skj_vdso_table to the
 * guest. The entry is set to the table only to satisfy the format; it is never
 * run. */
OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(skj_vdso_table)
MEMORY { VDSO (rx) : ORIGIN = 0x50000000, LENGTH = 0x10000 }
/* Two program headers, as in the MIPS linker's own default script: the writer
 * forces a leading PT_MIPS_ABIFLAGS, so both must be declared here for ld_layout
 * to reserve file and address space for them and keep the file-offset to vaddr
 * congruence the single-segment writer relies on.  Omitting them skews every
 * section by one phdr (0x20), so the advertised skj_vdso_table address no longer
 * matches where the bytes land. */
PHDRS {
    abiflags PT_LOAD FLAGS(4);
    load PT_LOAD FLAGS(7);
}
SECTIONS {
    .text : { *(.text .text.*) } > VDSO
    .rodata : { *(.rodata .rodata.*) } > VDSO
    .data : { *(.data .data.*) *(.sdata .sdata.*) } > VDSO
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.gnu.attributes)
                  *(.mdebug.*) *(.note .note.*) *(.comment) *(.eh_frame)
                  *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) }
}
