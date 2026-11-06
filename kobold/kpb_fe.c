/* kpb_fe.c : the front-end-facing shims for the in-memory module builder.

   The native single-step skj-kpc links the self-hosted Kobold front end (kpcom,
   compiled to native) beside the C module builder (kpbuild.c).  The front end
   drives the builder as it lowers, calling these kpbp_* entry points by name
   (external cdecl).  It passes strings as runtime string descriptors, not C
   strings, so each shim converts the descriptor to a C string and calls the
   const char * core in kpbuild.c.  The native Pascal string descriptor and the
   JIT's kp_str share one layout ({refcount,kind,len,cap} 16-byte header, then
   the bytes), so one reader serves the descriptor here; the JIT binds its own
   copy that first translates the guest pointer.

   The native single-step output stage (kpb_emit_native) lives in kpb_native.c,
   so a tool that drives the builder but emits another way (the in-process JIT)
   can link these shims without the AOT backend. */

#include "kpbuild.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* A runtime string descriptor: a 16-byte {refcount, kind, len, cap} header,
   then the bytes.  len is a 32-bit field at offset 8. */
#define KPB_STR_LEN(d)   (*(const int32_t *)((const char *)(d) + 8))
#define KPB_STR_BYTES(d) ((const char *)(d) + 16)

/* Copy a descriptor's bytes into a fresh NUL-terminated C string (caller frees).
   A null descriptor is the empty string. */
static char *
desc_dup(const void *d)
{
    int32_t len;
    char *out;

    if (!d) {
        out = malloc(1);
        out[0] = '\0';
        return out;
    }
    len = KPB_STR_LEN(d);
    if (len < 0)
        len = 0;
    out = malloc((size_t)len + 1);
    memcpy(out, KPB_STR_BYTES(d), (size_t)len);
    out[len] = '\0';
    return out;
}

void kpbp_begin(void) { kpb_begin(); }

void kpbp_target(const void *tag)
{ char *s = desc_dup(tag); kpb_target(s); free(s); }

void kpbp_kind(int kind, const void *name)
{ char *s = desc_dup(name); kpb_kind(kind, s); free(s); }

void kpbp_entry(const void *name)
{ char *s = desc_dup(name); kpb_entry(s); free(s); }

void kpbp_file(const void *src)
{ char *s = desc_dup(src); kpb_file(s); free(s); }

void kpbp_dep(const void *name, int64_t fp)
{ char *s = desc_dup(name); kpb_dep(s, (unsigned int)fp); free(s); }

void kpbp_export(const void *name)
{ char *s = desc_dup(name); kpb_export(s); free(s); }

void kpbp_import_begin(int kind, const void *name)
{ char *s = desc_dup(name); kpb_import_begin(kind, s); free(s); }

void kpbp_import_param(const void *cls)
{ char *s = desc_dup(cls); kpb_import_param(s); free(s); }

void kpbp_import_ret(const void *cls)
{ char *s = desc_dup(cls); kpb_import_ret(s); free(s); }

void kpbp_method_begin(const void *name)
{ char *s = desc_dup(name); kpb_method_begin(s); free(s); }

void kpbp_method_param(const void *cls)
{ char *s = desc_dup(cls); kpb_method_param(s); free(s); }

void kpbp_method_ret(const void *cls)
{ char *s = desc_dup(cls); kpb_method_ret(s); free(s); }

void kpbp_global_begin(const void *name, const void *base,
                       int arr, int align, int flags)
{
    char *n = desc_dup(name), *b = desc_dup(base);
    kpb_global_begin(n, b, arr, align, flags);
    free(n);
    free(b);
}

void kpbp_global_init_int(int64_t v) { kpb_global_init_int(v); }

void kpbp_global_init_sym(const void *name)
{ char *s = desc_dup(name); kpb_global_init_sym(s); free(s); }

void kpbp_global_string(const void *s)
{
    /* the string literal's bytes, which may contain NULs, come straight from the
       descriptor with its own length */
    int len = s ? KPB_STR_LEN(s) : 0;
    if (len < 0)
        len = 0;
    kpb_global_string(s ? KPB_STR_BYTES(s) : "", len);
}

void kpbp_global_end(void) { kpb_global_end(); }

void kpbp_iface_begin(int64_t fp) { kpb_iface_begin((unsigned int)fp); }

void kpbp_iface_line(const void *line)
{ char *s = desc_dup(line); kpb_iface_line(s); free(s); }

void kpbp_func_begin(const void *name, int is_local, int is_variadic,
                     int nparams, int nslots, int ntemps, int nlabels)
{
    char *s = desc_dup(name);
    kpb_func_begin(s, is_local, is_variadic, nparams, nslots, ntemps, nlabels);
    free(s);
}

void kpbp_slotsize(int size) { kpb_slotsize(size); }
void kpbp_paramclass(int cls) { kpb_paramclass(cls); }
void kpbp_paramfw(int fw) { kpb_paramfw(fw); }

void kpbp_insn(const void *mnem, int dst, int a, int b, int64_t imm,
               int slot, int lab, int nargs, const void *sym)
{
    char *m = desc_dup(mnem), *y = desc_dup(sym);
    kpb_insn(m, dst, a, b, imm, slot, lab, nargs, y);
    free(m);
    free(y);
}

int kpbp_asm(const void *args)
{
    char *s = desc_dup(args);
    int r = kpb_asm_text(s);
    free(s);
    return r;
}

void kpbp_func_end(void) { kpb_func_end(); }
