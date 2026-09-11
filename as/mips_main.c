/* mips_main.c : driver for skj-as-mips, the MIPS I assembler
 *
 * Step 3: the ELF writer.  The assembler runs the two-pass sizing and emit
 * over the real (non-macro) instruction set and writes an ELF32 little-endian
 * EM_MIPS relocatable object to '-o'.  '-d' dumps the encoded section bytes as
 * hex.  Macros and the .set reorder engine are still later steps. */

#include "mips.h"
#include "version.h"

#include <stdlib.h>
#include <string.h>

static void
cleanup_mips(void *p)
{
    mips_free(p);
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
        "usage: skj-as-mips [-o output.o] [-d] input.s\n"
        "\n"
        "MIPS I assembler (GAS syntax subset), little-endian o32.\n"
        "Writes ELF32 little-endian EM_MIPS relocatable objects.  The macros\n"
        "the backend leans on and the .set reorder engine are later steps.\n"
        "\n"
        "options:\n"
        "  -o FILE   write the object to FILE (default: stdout)\n"
        "  -d        dump the encoded section bytes as hex\n"
        "  -S        write the delay-slot-scheduled assembly, not an object\n"
        "  -h        show this help\n"
        "  -V        show version\n");
}

static const char *const section_names[MSEC_COUNT] = {
    ".text", ".data", ".rodata", ".bss",
};

static void
dump_sections(struct mips_asm *a)
{
    int sec, k;

    for (sec = 0; sec < MSEC_COUNT; sec++) {
        struct section *s = &a->sections[sec];
        if (s->len == 0)
            continue;
        printf("%s (%d bytes):\n", section_names[sec], s->len);
        for (k = 0; k < s->len; k++) {
            printf("%02x", s->data[k]);
            if ((k & 15) == 15 || k == s->len - 1)
                printf("\n");
            else
                printf(" ");
        }
    }
}

int
main(int argc, char **argv)
{
    const char *outpath;
    const char *inpath;
    FILE *out;
    char *src;
    struct mips_asm a;
    int dump, emit_asm;
    int k;

    util_set_progname("skj-as-mips");

    outpath = NULL;
    inpath = NULL;
    dump = 0;
    emit_asm = 0;
    for (k = 1; k < argc; k++) {
        if (strcmp(argv[k], "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[k], "-V") == 0) {
            printf("skj-as-mips %s\n", SKJ_VERSION);
            return 0;
        }
        if (strcmp(argv[k], "-d") == 0) {
            dump = 1;
        } else if (strcmp(argv[k], "-S") == 0) {
            emit_asm = 1;
        } else if (strcmp(argv[k], "-o") == 0 && k + 1 < argc) {
            outpath = argv[++k];
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

    mips_init(&a, src);
    util_cleanup_push(cleanup_mips, &a);

    if (emit_asm) {
        if (outpath) {
            out = fopen(outpath, "wb");
            if (!out)
                die("cannot open %s for writing", outpath);
        } else {
            out = stdout;
        }
        fputs(a.sched_src, out);
        if (outpath)
            fclose(out);
        util_cleanup_run();
        return 0;
    }

    mips_assemble(&a);

    if (dump)
        dump_sections(&a);

    if (outpath) {
        out = fopen(outpath, "wb");
        if (!out)
            die("cannot open %s for writing", outpath);
    } else {
        out = stdout;
    }

    mips_elf_write(&a, out);

    if (outpath)
        fclose(out);

    util_cleanup_run();

    return 0;
}
