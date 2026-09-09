/* rvtest_config.h : riscv-arch-test configuration for this emulator */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

/* The official flow generates this header from a machine-readable
 * description of the implementation. The emulator is small enough to
 * describe by hand: RV32IMFC with the Zicsr and Zifencei extensions,
 * machine mode only, no PMP, no vector unit. */

#ifndef RVTEST_CONFIG_H
#define RVTEST_CONFIG_H

#define UDB_MXLEN 32

/* Implemented extensions */
#define F_SUPPORTED
#define ZIFENCEI_SUPPORTED
#define ZICNTR_SUPPORTED

/* Machine mode only: no supervisor, user, hypervisor or vector state */

/* mtvec supports direct mode with four-byte alignment */
#define UDB_MTVEC_MODES_0
#define UDB_MTVEC_BASE_ALIGNMENT_DIRECT 4
#define UDB_MTVEC_BASE_ALIGNMENT_VECTORED 4

/* No physical memory protection */
#define UDB_NUM_PMP_ENTRIES 0
#define UDB_NUM_USABLE_PMP_ENTRIES 0
#define UDB_PMP_GRANULARITY 0

#endif /* RVTEST_CONFIG_H */
