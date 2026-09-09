/* fuzz_target.S : guest shell whose code buffer the fuzzer fills in */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The program itself does nothing. It exists so that qemu-riscv32 has a
 * process image with a large writable and executable region at a known
 * address. The fuzzer writes random instruction encodings into fuzz_code
 * over the remote protocol, points both models at it, and steps. */

    .section .text.entry, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop
    la      sp, __stack_top
    la      t0, fuzz_code
    jr      t0
    .size _start, . - _start

    .globl exit_stub
    .type exit_stub, @function
exit_stub:
    li      a7, 93
    li      a0, 0
    ecall
1:  j       1b
    .size exit_stub, . - exit_stub

    /* An executable buffer: the fuzzer writes instructions here and jumps
     * into them, so the region has to carry execute permission. A plain
     * .bss lands in a read-write segment that qemu refuses to run. */
    .section .fuzzcode, "awx", @nobits
    .globl fuzz_code
fuzz_code:
    .space 262144
    .size fuzz_code, . - fuzz_code
