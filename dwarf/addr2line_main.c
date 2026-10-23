/* addr2line_main.c : skj-addr2line, a standalone address-to-source tool over
   the shared DWARF reader (dwread.c).  It mirrors the GNU addr2line surface
   skjegg uses: `-e FILE` names the executable, each argument is a code address
   (hex with 0x, else decimal), and for each one it prints file:line, or, with
   -f, the function name on the line before.  An unresolved address prints the
   usual `??` / `??:0`. */

#include "dwread.h"
#include "arena.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *
read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
        die("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0)
        die("cannot size %s", path);
    rewind(f);
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf)
        die("out of memory");
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        die("short read on %s", path);
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static void
usage(FILE *out)
{
    fprintf(out, "usage: skj-addr2line [-f] -e <file> <addr>...\n");
}

int
main(int argc, char **argv)
{
    const char *elf_path = NULL;
    int want_func = 0;
    int i;
    uint8_t *elf;
    size_t elf_len;
    struct arena a;
    struct dw_reader r;

    util_set_progname("skj-addr2line");

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "-f") == 0) {
            want_func = 1;
        } else if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) {
                usage(stderr);
                return 2;
            }
            elf_path = argv[i];
        } else {
            break;              /* first address argument */
        }
    }
    if (!elf_path || i >= argc) {
        usage(stderr);
        return 2;
    }

    elf = read_file(elf_path, &elf_len);
    arena_init(&a);
    if (dw_read_elf(&a, &r, elf, elf_len) != 0)
        die("%s: not a readable ELF32 with debug line info", elf_path);

    for (; i < argc; i++) {
        uint32_t addr = (uint32_t)strtoul(argv[i], NULL, 0);
        const char *file = NULL, *fn = NULL;
        int line = 0;

        if (want_func)
            printf("%s\n", dw_lookup_func(&r, addr, &fn) ? fn : "??");
        if (dw_lookup_line(&r, addr, &file, &line))
            printf("%s:%d\n", file ? file : "??", line);
        else
            printf("??:0\n");
    }

    arena_free(&a);
    free(elf);
    return 0;
}
