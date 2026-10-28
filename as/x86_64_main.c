/* x86_64_main.c : driver for skj-as-x86-64, the in-tree x86-64 assembler */

#include "x86_64.h"
#include "util.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
cleanup_x86_64(void *p)
{
    x86_64_free(p);
}

static void
cleanup_free(void *p)
{
    free(p);
}

static void
usage(void)
{
    fprintf(stderr,
        "usage: skj-as-x86-64 [-o output.o] input.s\n"
        "\n"
        "x86-64 assembler (GNU .intel_syntax noprefix subset).\n"
        "Reads the assembly the toolkit's x86-64 backends emit and produces\n"
        "ELF64 little-endian EM_X86_64 relocatable objects.  Linking stays\n"
        "external (GNU ld / gcc -no-pie).\n"
        "\n"
        "options:\n"
        "  -o FILE   write output to FILE (default: stdout)\n"
        "  -f FMT    accepted and ignored (output is always ELF64)\n"
        "  --64      accepted and ignored\n"
        "  -h        show this help\n"
        "  -V        show version\n");
}

int
main(int argc, char **argv)
{
    const char *outpath;
    const char *inpath;
    FILE *out;
    char *src;
    struct x86_64_asm a;
    int k;

    util_set_progname("skj-as-x86-64");

    outpath = NULL;
    inpath = NULL;
    for (k = 1; k < argc; k++) {
        if (strcmp(argv[k], "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[k], "-V") == 0) {
            printf("skj-as-x86-64 %s\n", SKJ_VERSION);
            return 0;
        }
        if (strcmp(argv[k], "-o") == 0 && k + 1 < argc) {
            outpath = argv[++k];
        } else if (strcmp(argv[k], "-f") == 0 && k + 1 < argc) {
            k++;                            /* -f elf64: ignore the format arg */
        } else if (strncmp(argv[k], "-f", 2) == 0) {
            /* glued form -felf64, or a bare -f: ignore */
        } else if (strcmp(argv[k], "--64") == 0 || strcmp(argv[k], "--32") == 0) {
            /* accepted and ignored: output is always ELF64 */
        } else if (argv[k][0] == '-') {
            die("unknown option: %s", argv[k]);
        } else {
            if (inpath)
                die("multiple input files");
            inpath = argv[k];
        }
    }

    if (!inpath) {
        usage();
        return 1;
    }

    if (setjmp(util_die_env) != 0)
        return 1;
    util_die_active = 1;

    src = slurp(inpath);
    util_cleanup_push(cleanup_free, src);

    x86_64_init(&a, src);
    lex_set_source_path(&a.lex, inpath);        /* for relative .include */
    util_cleanup_push(cleanup_x86_64, &a);

    x86_64_assemble(&a);

    if (outpath) {
        out = fopen(outpath, "wb");
        if (!out)
            die("cannot open %s for writing", outpath);
    } else {
        out = stdout;
    }

    x86_64_elf_write(&a, out);

    if (outpath)
        fclose(out);

    util_cleanup_run();

    return 0;
}
