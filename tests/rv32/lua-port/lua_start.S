/* lua_start.S : entry point for guests that link against a C library */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The freestanding start.S is not enough once a C library is involved.
 * The bss has to be cleared before anything reads a zero-initialised
 * global, and any constructors the library registered have to run before
 * main. picolibc's own crt0 does all this, but it also expects a set of
 * symbols that vary between its build configurations, so doing it here
 * keeps the guest independent of which one is installed. */

    .section .text.entry, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top

    /* Clear bss */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b

    /* Run the constructor array */
2:  la      s0, __init_array_start
    la      s1, __init_array_end
3:  bgeu    s0, s1, 4f
    lw      t0, 0(s0)
    jalr    t0
    addi    s0, s0, 4
    j       3b

4:  li      a0, 0
    li      a1, 0
    call    main

    /* exit(main()) */
    li      a7, 93
    ecall
5:  j       5b
    .size _start, . - _start
