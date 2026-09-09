/* start.S : freestanding RV32 entry point */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The stack pointer is set from a linker symbol rather than inherited from
 * the process image. qemu-riscv32 hands the guest a stack containing argv
 * and the environment, which would differ from the emulator's blank memory
 * and break instruction-by-instruction comparison. Starting from a linked
 * address makes both models agree on every byte the program can observe. */

    .section .text.entry, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top
    li      a0, 0
    li      a1, 0
    call    main

    /* exit(main()) */
    li      a7, 93
    ecall
1:  j       1b
    .size _start, . - _start
