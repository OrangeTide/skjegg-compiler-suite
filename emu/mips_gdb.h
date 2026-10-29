/* mips_gdb.h : a GDB remote-serial-protocol server for the MIPS I user machine.

   The MIPS sibling of rv_gdb.h.  skj-run opens this stub with --gdb PORT: it
   listens on a TCP port, waits for a debugger (gdb, gdb-multiarch, or the
   in-tree skj-dbg) to connect, and drives one process under the same RSP
   subset the RV32 stub serves: `?`, `g`/`G`, `p`/`P`, `m`/`M` (bound checked),
   `c`, `s`, `Z0`/`z0` software breakpoints, the raw Ctrl-C interrupt byte, and
   `qSupported`.

   Registers follow gdb's o32 numbering: r0-r31 are 0-31, then status (32),
   lo (33), hi (34), badvaddr (35), cause (36) and pc (37).  gdb derives that
   layout from the ELF (the stub advertises no target description, since gdb's
   MIPS tdesc validator rejects a supplied one; qemu-mipsel's stub does the
   same).  The core models only the general registers plus HI/LO and the
   program counter, so status, badvaddr and cause read back as zero and ignore
   writes.

   Breakpoints are kept in a host-side table checked while single-stepping, so
   guest memory is never patched.  v1 debugs the root process only. */

#ifndef SKJ_MIPS_GDB_H
#define SKJ_MIPS_GDB_H

#include "mips32.h"
#include "guest.h"

/* Serve the protocol for one MIPS process on `port` until the guest exits or
   the debugger detaches or kills it.  cpu and g are the process's CPU and guest
   (memory and exit state); the CPU's bus must already be wired (so mips_step
   runs).  The guest's exit status is written through *exit_code.  Returns 0
   when the session ends in an orderly way (guest exit, detach, or kill),
   non-zero if the server could not be set up. */
int mips_gdb_serve(mips_cpu *cpu, guest *g, int port, int *exit_code);

#endif /* SKJ_MIPS_GDB_H */
