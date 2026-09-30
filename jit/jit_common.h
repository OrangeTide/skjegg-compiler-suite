/* jit_common.h : the architecture-neutral core of the in-process JIT.

   kp_jit() lays a whole ir_program's globals into a writable data region and
   its functions into an executable code region, resolving every symbol against
   the program's globals and functions or the supplied host bindings, then hands
   back the tables that map names to their runtime addresses.  The container, the
   layout, the symbol resolution, and the relocation bookkeeping here are shared
   by every target; each target (x86-64 in jit_x86.c, AArch64 in jit_arm64.c)
   supplies a struct jit_target: a byte-sink emit_func plus the two relocation
   appliers whose encodings differ per architecture. */

#ifndef JIT_COMMON_H
#define JIT_COMMON_H

#include "ir.h"
#include "code.h"
#include "jit_arena.h"

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
    uint8_t *stack;         /* low-memory execution stack (see kp_jit_call) */
    size_t stack_len;
    struct kp_arena arena;  /* the guest runtime arena (__moo_arena_alloc),
                               mapped lazily; only Excelsior guests use it */
    struct kp_gslot *globals;
    int nglobals;
    struct kp_fslot *funcs;
    int nfuncs;
    struct kp_line *lines;  /* address-to-line table (-g), by increasing off */
    int nlines;
    const char *source_file; /* the module's .file (-g), or NULL */
};

/* A deferred intra-module call: patch the site to reach `target`. */
struct callreloc {
    size_t site;
    const char *target;
};

/* Per-compile emit state: the destination jit, the program and its bindings,
   the pending relocation and line tables, and the caller's error buffer.  A
   target's byte sink records relocations and lines through the helpers below. */
struct emitctx {
    struct kp_jit *j;
    struct ir_program *prog;
    const struct kp_binding *binds;
    int nbinds;
    struct callreloc *relocs;       /* intra-module calls (target-relative) */
    int nrelocs, crelocs;
    struct callreloc *absrelocs;    /* absolute references to a function's code */
    int nabs, cabs;
    struct kp_line *lines;          /* address-to-line entries from IR_LOC (-g) */
    int nlines, clines;
    char *err;
    size_t errlen;
};

/* One line of failure text into e->err (if any), returning -1. */
int kp_fail(struct emitctx *e, const char *fmt, const char *a, const char *b);

/* Relocation and line bookkeeping the target's byte sink calls. */
void kp_add_reloc(struct emitctx *e, size_t site, const char *target);
void kp_add_absreloc(struct emitctx *e, size_t site, const char *target);
void kp_add_line(struct emitctx *e, size_t off, int line);

/* Symbol lookup the target's byte sink calls while emitting a function. */
uint8_t *kp_global_addr(struct kp_jit *j, const char *name);
void *kp_import_addr(struct emitctx *e, const char *name);
int kp_is_module_func(struct ir_program *prog, const char *name);

/* The per-architecture back end kp_jit() drives.  A JIT binary links exactly
   one, defined as the symbol kp_target. */
struct jit_target {
    /* Emit one function's body into c through the arch's byte sink.  Returns 0,
       or -1 with a one-line reason written into e->err. */
    int (*emit_func)(struct emitctx *e, struct code *c, struct ir_func *fn);
    /* Patch an intra-module call at `site` in c to reach code offset `target`.
       Returns 0, or -1 if the target is out of the branch's range. */
    int (*apply_call_reloc)(struct code *c, size_t site, size_t target);
    /* Patch an absolute reference at `site` in the mapped code to hold addr. */
    void (*apply_abs_reloc)(uint8_t *code, size_t site, uint64_t addr);
};

extern const struct jit_target kp_target;

/** Compile every function in `prog` to native code (see the file comment). */
int kp_jit(struct kp_jit *j, struct ir_program *prog,
           const struct kp_binding *binds, int nbinds,
           char *err, size_t errlen);

/* The native entry point of a compiled function, or NULL if absent. */
void *kp_jit_entry(struct kp_jit *j, const char *name);

/** Call a no-argument compiled entry on the module's low-memory stack. */
void kp_jit_call(struct kp_jit *j, void *entry);

/* Call a JIT-compiled guest entry with marshaled arguments, on the module's
   low-memory execution stack. Up to 6 integer and 8 float arguments are read
   from iargs/fargs; the guest reads only those its signature declares.
   stack_top is 16-byte-aligned at the top of the execution stack. Returns the
   integer result and the float result through fret. Defined in
   call_guest_<arch>.S. */
uint64_t kp_call_guest(void *entry, void *stack_top,
                       const int64_t *iargs, const double *fargs, double *fret);

/* Release the data and code regions, the runtime arena, and the name tables. */
void kp_jit_free(struct kp_jit *j);

/* The guest runtime arena binding: a driver running an Excelsior program binds
   the name "__moo_arena_alloc" to this, so libexc allocates from the active
   JIT's low-memory arena.  Returns a low (< 4GB) pointer, or NULL. */
void *__moo_arena_alloc(int size);

#endif /* JIT_COMMON_H */
