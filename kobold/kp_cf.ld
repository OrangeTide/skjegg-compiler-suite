/* kp_cf.ld : linker script for a freestanding ColdFire Kobold guest.

   The default skj-ld script sizes RAM at 1 MiB, but kp_crt_cf.S reserves its
   guest heap (8 MiB) and execution stack (2 MiB) in bss, so the image needs a
   larger region.  This is the ColdFire counterpart of skj-ld-rv's built-in
   freestanding layout; the base is 0x80000000 (skj-run's CFV4e user mode and
   qemu-m68k both load there), and skj-run maps the bss from the PT_LOAD memsz
   and zero-fills it, so only the location counter grows, not the file. */

OUTPUT_FORMAT("elf32-m68k")
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 32M }
SECTIONS {
    .text : { *(.text .text.*) } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data : { *(.data .data.*) } > RAM
    .bss : { __bss_start = .; *(.bss .bss.*) *(COMMON) __bss_end = .; } > RAM
    /DISCARD/ : { *(.note .note.*) *(.comment) *(.eh_frame) *(.gnu.hash) }
}
