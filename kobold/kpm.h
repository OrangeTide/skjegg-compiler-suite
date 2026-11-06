/* kpm.h : reader for the KPM1 text module format */

#ifndef KPM_H
#define KPM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ir.h"

/* IR_ADDO, IR_SUBO, IR_MULO, and IR_LOC are defined directly in the Skjegg
   ir.h opcode enum, so no layered definitions are needed here. */

/* An imported symbol the host must provide before the module runs. */
enum kp_import_kind {
    KP_IMPORT_FUNC,     /* a host function called directly */
    KP_IMPORT_HELPER,   /* a compiler runtime helper */
    KP_IMPORT_TYPE,     /* a host handle (interface) type and its vtable */
};

/* One method of an imported handle type, in vtable-slot order. */
struct kp_method {
    char *name;
    char **param_cls;   /* ABI class token per declared parameter */
    int nparams;
    char *ret_cls;
};

struct kp_import {
    int kind;
    char *name;
    char **param_cls;   /* func/helper: class token per parameter */
    int nparams;
    char *ret_cls;      /* func/helper: result class */
    struct kp_method *methods;  /* type: one per vtable slot */
    int nmethods;
};

/* A direct unit dependency: the used unit's name and the interface fingerprint the module
   was compiled against, so the linker rejects a consumer built against a stale interface. */
struct kp_dep {
    char *name;
    uint32_t fingerprint;
};

/* What a module is (the .module <kind> header). Absent, a module is an object:
   a compiled unit with unresolved imports, for later linking. */
enum kp_module_kind {
    KP_MOD_OBJECT = 0,      /* the default: a compiled unit, for linking */
    KP_MOD_LIBRARY,         /* a loadable library: resolved exports, may import host */
    KP_MOD_EXECUTABLE,      /* a fully linked image with an entry point, ready to run */
};

/* A parsed module: the IR program plus the import and export tables the
   linker resolves against. The arena owns every allocation here. The public
   kp_module wrapper (kp.h) holds this alongside the module's compiled code. */
struct kpm_module {
    struct arena arena;
    struct ir_program prog;
    struct kp_import *imports;
    int nimports;
    char **exports;
    int nexports;
    int kind;           /* enum kp_module_kind, from .module (default object) */
    char *name;         /* the unit's own name (.module object <name>), or NULL */
    char *entry;        /* the .entry routine of an executable, or NULL */
    char *target;       /* the .target hint, or NULL */
    char *source_file;  /* the .file debug hint (-g), or NULL */
    uint32_t iface_fp;  /* the .interface fingerprint, or 0 without an interface */
    struct kp_dep *deps;    /* direct unit dependencies and their expected fingerprints */
    int ndeps;
    /* The body of the .interface block, one entry line per element, carried
       verbatim.  kpm_read does not fill this (it keeps only iface_fp and skips
       the body); the module builder sets it so kpm_write can re-emit the block
       for a unit.  The lines do not include the .interface/.endinterface wrapper. */
    char **iface_lines;
    int niface_lines;
};

/** Parse a KPM1 text image into a module.
 *
 * Returns 0 on success. On a format error returns -1 and writes a
 * one-line message into err (line-numbered where possible). The module's
 * arena holds all output; release it with kpm_free. On failure the reader
 * releases its own arena and zeroes the module, so no cleanup is required
 * (a defensive kpm_free is harmless).
 */
int kpm_read(struct kpm_module *m, const char *text, size_t len,
             char *err, size_t errlen);

/* Release everything the module owns. */
void kpm_free(struct kpm_module *m);

/** Serialize a module back to KPM1 text.
 *
 * The inverse of kpm_read: writes a valid KPM1 image that kpm_read parses
 * back to an equivalent module and that the Kobold front end's unit loader
 * reads for the .interface block.  The output is canonical, not necessarily
 * byte-identical to a particular front end's historical formatting; what it
 * guarantees is that a reader restores the same module.  A field left at its
 * ir_emit default (temp/slot/label index -1, imm 0, nargs 0, NULL sym) is
 * omitted, since the reader restores that default.  Returns 0, or -1 on a
 * write error.
 */
int kpm_write(FILE *out, const struct kpm_module *m);

/* The mnemonic for an ir_op, e.g. "ADD" (without the IR_ prefix), or NULL. */
const char *kpm_op_name(int op);

/* The ir_op for a KPM mnemonic, e.g. "ADD" -> IR_ADD, or -1 if unknown. */
int kpm_op_by_name(const char *name);

/* The ir base-type for a KPM type name, e.g. "i32" -> IR_I32, or -1. */
int kpm_base_type_by_name(const char *name);

/* Parse one ASM instruction line (it begins with "ASM") into a fresh IR_ASM
   instruction on fn, using arena a for strings.  The one asm-line grammar has
   one parser, shared by the reader and the module builder.  The line is mutated
   in place.  Returns 1 when it parsed an ASM line, 0 when the line is not ASM,
   or -1 with *errmsg set to a static message on a malformed ASM line. */
int kpm_emit_asm_line(struct arena *a, struct ir_func *fn, char *line,
                      const char **errmsg);

#endif /* KPM_H */
