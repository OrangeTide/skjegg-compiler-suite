/* aot_main.c : skj-kp, the ahead-of-time compiler for Kobold modules.

   Reads a KPM1 text module (the Kobold front end's output) and writes native
   assembly through the shared emit pipeline (kp_aot.c), the same selector the
   JIT drives.  This is the AOT counterpart of skj-kp-jit: instead of emitting
   machine code into a mapped buffer and running it, it writes an assembly file
   that an assembler and linker turn into a native object.  The module parse
   (kpm_read) and the emit (kp_aot_emit) are separate so the single-step skj-kpc
   can build the module in memory and share the same emit stage. */

#include "kpm.h"
#include "kp_aot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
kp_slurp(const char *path, size_t *lenp)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    *lenp = (size_t)sz;
    return buf;
}

int
main(int argc, char **argv)
{
    const char *inpath = NULL;
    const char *outpath = NULL;
    char *text;
    size_t len;
    struct kpm_module m;
    char err[256];
    FILE *out;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: skj-kp module.kpm [-o out.asm]\n");
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outpath = argv[++i];
        else if (argv[i][0] != '-' && !inpath)
            inpath = argv[i];
    }
    if (!inpath) {
        fprintf(stderr, "usage: skj-kp module.kpm [-o out.asm]\n");
        return 2;
    }

    text = kp_slurp(inpath, &len);
    if (!text) {
        fprintf(stderr, "skj-kp: cannot read '%s'\n", inpath);
        return 1;
    }
    if (kpm_read(&m, text, len, err, sizeof err) != 0) {
        fprintf(stderr, "skj-kp: %s\n", err);
        free(text);
        return 1;
    }

    if (outpath) {
        out = fopen(outpath, "w");
        if (!out) {
            fprintf(stderr, "skj-kp: cannot write '%s'\n", outpath);
            kpm_free(&m);
            free(text);
            return 1;
        }
    } else {
        out = stdout;
    }

    kp_aot_emit(&m, out);

    if (out != stdout)
        fclose(out);
    kpm_free(&m);
    free(text);
    return 0;
}
