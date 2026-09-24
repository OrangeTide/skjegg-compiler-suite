/* jit_x86.h : JIT an ir_program to executable x86-64 code, in-process.

   The library face of the JIT: kp_jit() compiles a whole ir_program (from any
   front end) into an executable mapping, and kp_jit_entry()/kp_call_guest()
   run a compiled function.  A thin driver (skj-jit) sits on top; a host
   embedding the toolchain links this library and supplies its own bindings. */

#ifndef JIT_X86_H
#define JIT_X86_H

#include "ir.h"

#include <stddef.h>
#include <stdint.h>

/* One host symbol the module imports, bound to its runtime address. */
struct kp_binding {
    const char *name;
    void *addr;
};

/* A global's runtime home in the module's data region. */
struct kp_gslot {
    const char *name;
    uint8_t *addr;
};

/* A function's byte offset within the executable code region. */
struct kp_fslot {
    const char *name;
    size_t off;
};

/* One address-to-line entry: from `off` bytes into the code region onward, the
   code came from source `line`. Built in increasing-offset order under -g, so a
   trap maps its return address to a line with a simple scan. */
struct kp_line {
    size_t off;
    int line;
};

/* A JIT-compiled module: a writable data region for globals and a
   read-execute code region, plus the tables that map names to them. */
struct kp_jit {
    uint8_t *data;
    size_t data_len;
    size_t data_map;        /* mapped length of the data region (page-rounded) */
    uint8_t *code;          /* mmap'd, read-execute after kp_jit */
    size_t code_len;
    size_t code_map;        /* the mapped length (page-rounded) */
    uint8_t *stack;         /* low-2GB execution stack (see kp_jit_call) */
    size_t stack_len;
    struct kp_gslot *globals;
    int nglobals;
    struct kp_fslot *funcs;
    int nfuncs;
    struct kp_line *lines;  /* address-to-line table (-g), by increasing off */
    int nlines;
    const char *source_file; /* the module's .file (-g), or NULL */
};

/** Compile every function in `prog` to native code.
 *
 * Globals are laid out in a fresh data region and initialized; each `sym`
 * is resolved against the program's globals and functions or the supplied
 * host bindings. Returns 0 on success. On failure returns -1 and writes a
 * one-line reason into err; an unresolved import or an opcode outside the
 * supported subset is reported rather than miscompiled.
 */
int kp_jit(struct kp_jit *j, struct ir_program *prog,
           const struct kp_binding *binds, int nbinds,
           char *err, size_t errlen);

/* The native entry point of a compiled function, or NULL if absent. */
void *kp_jit_entry(struct kp_jit *j, const char *name);

/** Call a no-argument compiled entry on the module's low-2GB stack.
 *
 * The front end models every address as 32 bits, so a pointer to a local
 * (a nested-procedure upvalue, a by-reference aggregate) must fit in 32
 * bits. The ordinary thread stack sits high in the address space, so the
 * JIT runs compiled code on a stack mapped in the low 2GB instead.
 */
void kp_jit_call(struct kp_jit *j, void *entry);

/* Call a JIT-compiled guest entry with marshaled arguments, on the module's low-2GB
   execution stack. The integer arguments (up to 6) and float arguments (up to 8) are
   read from `iargs` and `fargs`; the guest reads only those its signature declares.
   `stack_top` is a 16-byte-aligned address at the top of the execution stack. Returns
   the integer result in the return value and the float result through `fret`. Defined
   in call_guest_<arch>.S. */
uint64_t kp_call_guest(void *entry, void *stack_top,
                       const int64_t *iargs, const double *fargs, double *fret);

/* Release the data and code regions and the name tables. */
void kp_jit_free(struct kp_jit *j);

#endif /* JIT_X86_H */
