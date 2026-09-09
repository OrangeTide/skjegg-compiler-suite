/* link.ld : linker script for riscv-arch-test binaries */
/* Machine-mode image at 0x80000000, matching the qemu virt board so the
 * same binary runs on the emulator and on qemu-system-riscv32. */

OUTPUT_ARCH("riscv")
ENTRY(rvtest_entry_point)

SECTIONS
{
    . = 0x80000000;

    .text.init : { *(.text.init) }
    .text : { *(.text) *(.text.*) }

    . = ALIGN(0x1000);
    .rodata : { *(.rodata) *(.rodata.*) *(.srodata) *(.srodata.*) }
    .data : { *(.data) *(.data.*) *(.sdata) *(.sdata.*) }

    . = ALIGN(0x1000);
    .tohost : { *(.tohost) }

    . = ALIGN(0x1000);
    .bss : {
        *(.sbss) *(.sbss.*) *(.bss) *(.bss.*) *(COMMON)
    }

    . = ALIGN(16);
    _end = .;
}
