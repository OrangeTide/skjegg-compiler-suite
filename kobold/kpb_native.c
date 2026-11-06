/* kpb_native.c : the native single-step driver hook for the module builder.

   kpb_emit_native takes the module the front end built (kpb_module_ptr) and
   writes it, either as assembly through the AOT emit pipeline (kp_aot_emit) or as
   the KPM1 text image (kpm_write).  It is the output stage of the native
   single-step compiler (skj-kpc).  It is kept apart from the kpbp_* front-end
   shims in kpb_fe.c so a tool that drives the builder but emits another way (the
   in-process JIT, which JIT-compiles the module instead of writing assembly) can
   link the shims without pulling in the AOT backend. */

#include "kpbuild.h"
#include "kpm.h"
#ifndef KP_FREESTANDING
#include "kp_aot.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* A runtime string descriptor: a 16-byte {refcount, kind, len, cap} header, then
   the bytes; len is a 32-bit field at offset 8. */
#define KPB_STR_LEN(d)   (*(const int32_t *)((const char *)(d) + 8))
#define KPB_STR_BYTES(d) ((const char *)(d) + 16)

static char *
kpb_desc_dup(const void *d)
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

/* The driver hook: take the finished module and write it.  emit_kpm selects the
   KPM text image (kpm_write) over assembly (kp_aot_emit).  An empty output path
   writes to standard output.  Returns 0 on success, non-zero on a write error. */
int
kpb_emit_native(const void *outpath, int emit_kpm)
{
    char *path = kpb_desc_dup(outpath);
    struct kpm_module *m = kpb_module_ptr();
    FILE *out;
    int rc = 0;

    if (path[0]) {
        out = fopen(path, "w");
        if (!out) {
            fprintf(stderr, "skj-kpc: cannot write '%s'\n", path);
            free(path);
            return 1;
        }
    } else {
        out = stdout;
    }
    if (emit_kpm) {
        rc = kpm_write(out, m);
    } else {
#ifdef KP_FREESTANDING
        /* The freestanding self-host guest carries no backend: it only
           serializes the builder module (kpm_write).  Lowering to assembly is
           the host skj-kpc's job. */
        fprintf(stderr, "skj-kpc: assembly output is not available in the "
                        "freestanding compiler; use --emit-kpm\n");
        rc = 1;
#else
        rc = kp_aot_emit(m, out);
#endif
    }
    if (out != stdout)
        fclose(out);
    free(path);
    return rc;
}
