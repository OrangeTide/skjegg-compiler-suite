/* hv_gdb.h : a GDB remote-serial-protocol server for the HV (VMM) cores.
 *
 * skj-run opens this stub with --gdb PORT: it listens on a TCP port, waits for
 * a debugger (gdb learns the architecture from the ELF, so no target
 * description is sent), and drives one process under the RSP subset the other
 * cores serve: `?`, `g`/`G`, `m`/`M`, `c`, `s`, `Z0`/`z0` software breakpoints,
 * the raw Ctrl-C interrupt byte, and `qSupported`.
 *
 * Breakpoints are kept in a host-side table checked while single-stepping the
 * vcpu through the VMM, so guest memory is never patched.  The doorbell MMIO is
 * serviced transparently during stepping, so a guest's syscalls still run. */

#ifndef SKJ_HV_GDB_H
#define SKJ_HV_GDB_H

#include "sched.h"

/* Serve the protocol for one HV process on `port` until the guest exits or the
 * debugger detaches or kills it.  The process's vcpu must already be set up (the
 * core's ready() has run).  The guest's exit status is written through
 * *exit_code.  Returns 0 on an orderly end, non-zero if the server could not be
 * set up. */
void hv_gdb_serve(proc *root, int port, int *exit_code);

#endif /* SKJ_HV_GDB_H */
