/* gdbclient.h : minimal GDB remote serial protocol client */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

#ifndef GDBCLIENT_H
#define GDBCLIENT_H

#include <stdint.h>
#include <sys/types.h>

/* Architectural state read back from the remote stub. RISC-V gdb numbers
 * registers x0-x31, then pc, then f0-f31, then fflags, frm and fcsr. The
 * floating-point registers are held at the stub's native FLEN, which is 8
 * bytes when the remote CPU implements the D extension even though our
 * guest code only uses F. */
typedef struct gdb_state {
    uint32_t x[32];
    uint32_t pc;
    uint64_t f[32];
    uint32_t fflags, frm, fcsr;
} gdb_state;

typedef struct gdb_client {
    int   fd;
    pid_t child;
    int   ack_mode;
    int   flen;         /* bytes per floating-point register: 4 or 8 */
    int   has_fpu;
    /* Register numbers discovered from the remote's target description.
     * They cannot be hardcoded: qemu places the floating-point file and
     * the control registers in separate features, with vector registers
     * in between, so fcsr's number depends on the CPU model. */
    int   fpu_base;
    int   fflags_num;
    int   frm_num;
    int   fcsr_num;
} gdb_client;

/* Launch qemu-riscv32 under a gdb stub and connect to it. The guest is
 * stopped at its entry point when this returns. */
int gdb_launch(gdb_client *g, const char *qemu, const char *elf, int port);

/* Read the integer registers and the program counter. */
int gdb_read_state(gdb_client *g, gdb_state *st);

/* Read the floating-point registers and fcsr. These are not part of the
 * stub's bulk register block, so each one costs a round trip. Callers
 * should only ask after an instruction that can change them. */
int gdb_read_fpu(gdb_client *g, gdb_state *st);

/* Execute one instruction. Returns 1 if still running, 0 if the guest
 * exited, -1 on protocol error. *exit_code is set when it returns 0. */
int gdb_step(gdb_client *g, int *exit_code);

/* Read a single floating-point register, or the fcsr. */
int gdb_read_freg(gdb_client *g, int n, uint64_t *out);
int gdb_read_fcsr(gdb_client *g, uint32_t *out);

/* Write the integer registers and the program counter. */
int gdb_write_state(gdb_client *g, const gdb_state *st);

/* Write the floating-point registers and fcsr. Single-precision values
 * are NaN-boxed on the way out when the remote's FLEN is wider than 32
 * bits, which is what an F-only guest would have left there. */
int gdb_write_fpu(gdb_client *g, const gdb_state *st);

/* Launch a bare-metal reference instead of the Linux user-mode one. The
 * extra arguments are passed to the emulator verbatim. */
int gdb_launch_argv(gdb_client *g, char *const argv[], int port);

/* Set a breakpoint and resume. gdb_continue returns 1 when the guest
 * stopped again, 0 when it exited, -1 on error. */
int gdb_set_break(gdb_client *g, uint32_t addr, int kind);
int gdb_continue(gdb_client *g, int *exit_code);

/* Read len bytes of guest memory. Returns 0 on success. */
int gdb_read_mem(gdb_client *g, uint32_t addr, uint8_t *buf, uint32_t len);

/* Write len bytes of guest memory. Returns 0 on success. */
int gdb_write_mem(gdb_client *g, uint32_t addr, const uint8_t *buf,
                  uint32_t len);

void gdb_close(gdb_client *g);

#endif /* GDBCLIENT_H */
