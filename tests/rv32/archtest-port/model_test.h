/* model_test.h : riscv-arch-test target port for this emulator */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The compliance suite is written against a small set of target hooks.
 * This port targets a bare machine-mode image at 0x80000000 that ends by
 * writing to the test-finisher register the qemu virt board provides, so
 * one binary terminates cleanly on both the emulator and on
 * qemu-system-riscv32. */

#ifndef _COMPLIANCE_MODEL_H
#define _COMPLIANCE_MODEL_H

#define RVMODEL_DATA_SECTION                                            \
    .pushsection .tohost, "aw", @progbits;                              \
    .align 8; .global tohost; tohost: .dword 0;                         \
    .align 8; .global fromhost; fromhost: .dword 0;                     \
    .popsection;                                                        \
    .align 8; .global begin_regstate; begin_regstate:                   \
    .word 128;                                                          \
    .align 8; .global end_regstate; end_regstate:                       \
    .word 4;

/* Nothing to bring up: the emulator starts in machine mode with the
 * whole address space available. */
#define RVMODEL_BOOT

/* Terminate the run.
 *
 * The signature region is written out as hex over the 16550 transmitter at
 * 0x10000000, one word per line, before the board is asked to power down
 * through the test-finisher register at 0x00100000. Dumping from inside
 * the guest is what makes the two models directly comparable: the same
 * instructions produce the text on both, with no debugger attached. That
 * matters because a debugger changes behaviour -- qemu's gdb stub claims
 * the guest's own ebreak instead of delivering it as a trap. */
#define RVMODEL_HALT                                                    \
    .option push;                                                       \
    .option norvc;                                                      \
    .global rvmodel_halt_label;                                         \
rvmodel_halt_label:                                                     \
    la a0, begin_signature;                                             \
    la a1, end_signature;                                               \
    li a2, 0x10000000;                                                  \
10: bgeu a0, a1, 13f;                                                   \
    lw a3, 0(a0);                                                       \
    li a4, 8;                                                           \
11: addi a4, a4, -1;                                                    \
    slli a6, a4, 2;                                                     \
    srl a5, a3, a6;                                                     \
    andi a5, a5, 15;                                                    \
    li a7, 10;                                                          \
    blt a5, a7, 12f;                                                    \
    addi a5, a5, 87;                                                    \
    j 14f;                                                              \
12: addi a5, a5, 48;                                                    \
14: sb a5, 0(a2);                                                       \
    bnez a4, 11b;                                                       \
    li a5, 10;                                                          \
    sb a5, 0(a2);                                                       \
    addi a0, a0, 4;                                                     \
    j 10b;                                                              \
13: li t0, 0x00100000;                                                  \
    li t1, 0x5555;                                                      \
    sw t1, 0(t0);                                                       \
15: j 15b;                                                              \
    .option pop;

#define RVMODEL_DATA_BEGIN                                              \
    .align 4; .global begin_signature; begin_signature:

#define RVMODEL_DATA_END                                                \
    .align 4; .global end_signature; end_signature:                     \
    RVMODEL_DATA_SECTION

/* This port has no console. The suite only uses these for diagnostics. */
#define RVMODEL_IO_INIT
#define RVMODEL_IO_WRITE_STR(_R, _STR)
#define RVMODEL_IO_CHECK()
#define RVMODEL_IO_ASSERT_GPR_EQ(_S, _R, _I)
#define RVMODEL_IO_ASSERT_SFPR_EQ(_F, _R, _I)
#define RVMODEL_IO_ASSERT_DFPR_EQ(_D, _R, _I)

/* No interrupt controller is modelled. Tests that need one are outside
 * the extensions this emulator implements. */
#define RVMODEL_SET_MSW_INT
#define RVMODEL_CLR_MSW_INT
#define RVMODEL_CLR_MTIMER_INT
#define RVMODEL_CLR_MEXT_INT
#define RVMODEL_SET_VSW_INT
#define RVMODEL_CLR_VSW_INT
#define RVMODEL_CLR_VTIMER_INT
#define RVMODEL_CLR_VEXT_INT
#define RVMODEL_SET_SSW_INT
#define RVMODEL_CLR_SSW_INT
#define RVMODEL_CLR_STIMER_INT
#define RVMODEL_CLR_SEXT_INT

/* fence.i is a no-op on an interpreter with no instruction cache. */
#define RVMODEL_FENCEI fence.i

#endif /* _COMPLIANCE_MODEL_H */
