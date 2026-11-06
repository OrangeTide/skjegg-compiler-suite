/* kpm_roundtrip.c : read a KPM1 module and write it back out through
   kpm_write, so a caller can check the reader/writer pair is faithful by
   diffing the assembly a backend emits for the original and the rewritten
   module.  Reads argv[1] (a .kpm file), writes the re-serialized module to
   stdout. */

#include "kpm.h"

#include <stdio.h>
#include <stdlib.h>

static char *
rt_slurp(const char *path, size_t *lenp)
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
    char *text;
    size_t len;
    struct kpm_module m;
    char err[256];

    if (argc < 2) {
        fprintf(stderr, "usage: kpm_roundtrip module.kpm\n");
        return 2;
    }
    text = rt_slurp(argv[1], &len);
    if (!text) {
        fprintf(stderr, "kpm_roundtrip: cannot read '%s'\n", argv[1]);
        return 1;
    }
    if (kpm_read(&m, text, len, err, sizeof err) != 0) {
        fprintf(stderr, "kpm_roundtrip: %s\n", err);
        free(text);
        return 1;
    }
    if (kpm_write(stdout, &m) != 0) {
        fprintf(stderr, "kpm_roundtrip: write error\n");
        kpm_free(&m);
        free(text);
        return 1;
    }
    kpm_free(&m);
    free(text);
    return 0;
}
