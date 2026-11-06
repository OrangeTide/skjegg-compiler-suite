/* kpbuild.h : the in-memory KPM module builder interface.

   The single-step Kobold front end drives these entry points as it lowers,
   handing structured IR and module metadata across the boundary, and the driver
   then takes the finished module with kpb_module_ptr.  See kpbuild.c. */

#ifndef KPBUILD_H
#define KPBUILD_H

#include "kpm.h"

/* kpb_global_begin flag bits. */
#define KPB_G_EXPORT 1
#define KPB_G_PTR    2
#define KPB_G_RODATA 4
#define KPB_G_STROBJ 8

/* kpb_asm_op role codes. */
#define KPB_ASM_IN    0
#define KPB_ASM_OUT   1
#define KPB_ASM_INOUT 2

/* Start a fresh module (releases any previous one). */
void kpb_begin(void);

/* Header and metadata. */
void kpb_target(const char *tag);
void kpb_kind(int kind, const char *name);   /* kind: enum kp_module_kind */
void kpb_entry(const char *name);
void kpb_file(const char *src);
void kpb_dep(const char *name, unsigned int fp);
void kpb_export(const char *name);

/* Imports.  A func/helper: begin, then a param per class token, then ret.
   A type: begin(KP_IMPORT_TYPE), then per method a begin/param.../ret. */
void kpb_import_begin(int kind, const char *name);
void kpb_import_param(const char *cls);
void kpb_import_ret(const char *cls);
void kpb_method_begin(const char *name);
void kpb_method_param(const char *cls);
void kpb_method_ret(const char *cls);

/* Globals.  begin, then inits and/or a string, then end. */
void kpb_global_begin(const char *name, const char *base, int arr, int align, int flags);
void kpb_global_init_int(long long v);
void kpb_global_init_sym(const char *name);
void kpb_global_string(const char *bytes, int len);
void kpb_global_end(void);

/* Interface block (a unit): the fingerprint, then one entry line each. */
void kpb_iface_begin(unsigned int fp);
void kpb_iface_line(const char *line);

/* Functions.  begin, then slotsizes/paramclasses/paramfws, then instructions,
   then end.  A field left at its default is given with the sentinel the reader
   restores: index -1, imm/nargs 0, NULL sym. */
void kpb_func_begin(const char *name, int is_local, int is_variadic,
                    int nparams, int nslots, int ntemps, int nlabels);
void kpb_slotsize(int size);
void kpb_paramclass(int cls);
void kpb_paramfw(int fw);
void kpb_insn(const char *mnem, int dst, int a, int b, long long imm,
              int slot, int label, int nargs, const char *sym);
void kpb_asm_basic(const char *tmpl);
void kpb_asm_ext_begin(int nout, int nin, int vol);
void kpb_asm_op(int role, const char *constraint, int temp, int size);
void kpb_asm_clobber(const char *name);
void kpb_asm_ext_end(const char *tmpl);
/* Append an ASM instruction from its preformatted operand text (the part after
   "ASM"), the form the front end carries.  Returns 0, or -1 on a malformed line. */
int kpb_asm_text(const char *args);
void kpb_func_end(void);

/* The finished module, ready for kp_aot_emit or kpm_write. */
struct kpm_module *kpb_module_ptr(void);

#endif /* KPBUILD_H */
