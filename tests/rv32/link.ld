/* link.ld : linker script for RV32 test programs */
/* Base address 0x10000 matches where qemu-riscv32 maps a static ET_EXEC,
 * so one binary runs unmodified on both the emulator and QEMU. */

OUTPUT_FORMAT("elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x00010000, LENGTH = 0x200000
}

SECTIONS
{
    .text : {
        *(.text.entry)
        *(.text .text.*)
    } > RAM

    /* Executable scratch used by the instruction fuzzer. Page aligned:
     * qemu write-protects a whole page once it has translated code from
     * it, and the fuzzer hands out one page per round. */
    .fuzzcode : ALIGN(0x1000) {
        *(.fuzzcode)
    } > RAM

    .rodata : {
        *(.rodata .rodata.*)
    } > RAM

    .data : {
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        *(.data .data.*)
    } > RAM

    /* Test result slots, read back by the harness after the run */
    .results : {
        __results_start = .;
        *(.results)
        __results_end = .;
    } > RAM

    .bss : {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;

        /* The stack lives inside a PT_LOAD segment so that the region is
         * mapped identically by the emulator and by qemu-riscv32. */
        . = ALIGN(16);
        . = . + 0x4000;
        __stack_top = .;
    } > RAM
}
