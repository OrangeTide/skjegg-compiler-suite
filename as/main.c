/* main.c : driver for skj-as ColdFire assembler */

#include "as.h"
#include "version.h"

#include <stdlib.h>
#include <string.h>

static void
cleanup_as(void *p)
{
    as_free(p);
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
        "usage: skj-as [-o output.o] input.s\n"
        "\n"
        "ColdFire/m68k assembler (GAS syntax subset).\n"
        "Reads assembly text and produces ELF32 big-endian relocatable objects.\n"
        "\n"
        "options:\n"
        "  -o FILE            write output to FILE (default: stdout)\n"
        "  -mshort-branches   force word-form branches and error on a\n"
        "                     displacement that overflows 16 bits (default:\n"
        "                     widen such a branch to the 32-bit form)\n"
        "  -h                 show this help\n"
        "  -V                 show version\n");
}

int
main(int argc, char **argv)
{
    const char *outpath;
    const char *inpath;
    FILE *out;
    char *src;
    struct assembler a;
    int k;
    volatile int short_branches = 0;    /* read after setjmp; volatile keeps it */

    util_set_progname("skj-as");

    outpath = NULL;
    inpath = NULL;
    for (k = 1; k < argc; k++) {
        if (strcmp(argv[k], "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[k], "-V") == 0) {
            printf("skj-as %s\n", SKJ_VERSION);
            return 0;
        }
        if (strcmp(argv[k], "-o") == 0 && k + 1 < argc) {
            outpath = argv[++k];
        } else if (strcmp(argv[k], "-mshort-branches") == 0) {
            short_branches = 1;
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

    as_init(&a, src);
    a.short_branches = short_branches;
    lex_set_source_path(&a.lex, inpath);        /* for relative .include */
    util_cleanup_push(cleanup_as, &a);

    as_pass1(&a);
    as_pass2(&a);
    cf_emit_debug(&a);          /* build .debug_* from the -g .loc rows */

    if (outpath) {
        out = fopen(outpath, "wb");
        if (!out)
            die("cannot open %s for writing", outpath);
    } else {
        out = stdout;
    }

    elf_write(&a, out);

    if (outpath)
        fclose(out);

    util_cleanup_run();

    return 0;
}
