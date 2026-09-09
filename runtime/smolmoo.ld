/* smolmoo.ld : linker script for Excelsior verb programs on smolmoo
 * (https://github.com/OrangeTide/smolmoo). Mirrors the server's own
 * verb layout: a 128KB VM, 0x0-0x3FF host-reserved (vectors and the
 * verb-argument struct at 0x380), code from 0x400, the stack set by the
 * host growing down from 0x20000. The binding's _start sits in
 * .text.entry so it lands first.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x00000400, LENGTH = 0x1FC00
}

SECTIONS
{
    .text : {
        *(.text.entry)
        *(.text .text.*)
    } > RAM

    .rodata : {
        *(.rodata .rodata.*)
    } > RAM

    .data : {
        *(.data .data.*)
    } > RAM

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    __stack_top = 0x20000;
}
