/* lua.ld : linker script for guests that link against a C library */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The plain link.ld is enough for freestanding guests. A C library needs
 * more: initialised data with its own load address, the constructor
 * arrays picolibc walks at startup, thread-local storage, and above all a
 * heap for malloc to grow into. */

OUTPUT_FORMAT("elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x00010000, LENGTH = 0xE00000
}

SECTIONS
{
    .text : {
        *(.text.entry)
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > RAM

    . = ALIGN(16);

    /* picolibc runs these before main */
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > RAM
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > RAM
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > RAM

    . = ALIGN(16);
    .data : {
        __data_start = .;
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        *(.data .data.*)
        __data_end = .;
    } > RAM
    /* picolibc's startup copies data from its load address; here they are
     * the same, and the copy is a no-op it can safely perform. */
    PROVIDE(__data_source = __data_start);

    . = ALIGN(16);
    .tdata : {
        __tdata_start = .;
        *(.tdata .tdata.*)
        __tdata_end = .;
    } > RAM
    PROVIDE(__tdata_source = __tdata_start);
    PROVIDE(__tdata_size = __tdata_end - __tdata_start);

    .tbss : {
        __tbss_start = .;
        *(.tbss .tbss.*)
        *(.tcommon)
        __tbss_end = .;
    } > RAM
    PROVIDE(__tbss_size = __tbss_end - __tbss_start);
    PROVIDE(__tls_size = __tbss_end - __tdata_start);
    PROVIDE(__tdata_size = __tdata_end - __tdata_start);

    . = ALIGN(16);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM

    /* Lua allocates constantly, so it gets most of the image.
     *
     * The heap and the stack are reserved as real sections rather than by
     * moving the location counter past the end of the image. That keeps
     * them inside a loadable segment, so a model that maps only what the
     * program headers declare still has them. The emulator's flat memory
     * would tolerate either arrangement, which is exactly why it is worth
     * doing properly: the same binary then also runs under qemu, and the
     * two can be compared. */
    . = ALIGN(16);
    .heap (NOLOAD) : {
        __heap_start = .;
        . = . + 0x800000;               /* 8 MiB */
        __heap_end = .;
    } > RAM

    .stack (NOLOAD) : {
        . = ALIGN(16);
        . = . + 0x40000;                /* 256 KiB */
        __stack_top = .;
    } > RAM
    PROVIDE(__stack = __stack_top);

    /* picolibc's crt0 expects these names */
    PROVIDE(end = __heap_start);
    PROVIDE(_end = __heap_start);
}
