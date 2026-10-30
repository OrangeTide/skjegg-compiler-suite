/* aarch64_main.c : driver for skj-as-aarch64, the in-tree AArch64 assembler */

#include "aarch64.h"
#include "util.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
cleanup_a64(void *p)
{
    a64_free(p);
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
        "usage: skj-as-aarch64 [-o output.o] input.s\n"
        "\n"
        "AArch64 assembler (GNU as syntax subset).\n"
        "Reads the assembly the toolkit's arm64 backends emit and produces\n"
        "ELF64 little-endian EM_AARCH64 relocatable objects.  Linking stays\n"
        "external (GNU ld / gcc).\n"
        "\n"
        "options:\n"
        "  -o FILE   write output to FILE (default: stdout)\n"
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
    struct a64_asm a;
    int k;

    util_set_progname("skj-as-aarch64");

    outpath = NULL;
    inpath = NULL;
    for (k = 1; k < argc; k++) {
        if (strcmp(argv[k], "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[k], "-V") == 0) {
            printf("skj-as-aarch64 %s\n", SKJ_VERSION);
            return 0;
        }
        if (strcmp(argv[k], "-o") == 0 && k + 1 < argc) {
            outpath = argv[++k];
        } else if (argv[k][0] == '-' && argv[k][1] != '\0') {
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

    a64_init(&a, src);
    lex_set_source_path(&a.lex, inpath);        /* for relative .include */
    util_cleanup_push(cleanup_a64, &a);

    a64_assemble(&a);

    if (outpath) {
        out = fopen(outpath, "wb");
        if (!out)
            die("cannot open %s for writing", outpath);
    } else {
        out = stdout;
    }

    /* The object format is fixed at build time (Plan 9 style): a native macOS
       build emits Mach-O, a cross build emits it with -DSKJ_DARWIN, otherwise
       ELF.  There is no run-time flag. */
#if defined(__APPLE__) || defined(SKJ_DARWIN)
    a64_emit_debug(&a);         /* __DWARF line info from .file/.loc, if any */
    a64_macho_write(&a, out);
#else
    a64_elf_write(&a, out);
#endif

    if (outpath)
        fclose(out);

    util_cleanup_run();

    return 0;
}
