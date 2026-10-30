/* dwline_emit.c : write the DWARF 4 debug sections for a fixed, documented row
   table into the directory named by argv[1] (dwline.bin, dwinfo.bin,
   dwabbrev.bin).  The Makefile target test-dwarf-line wraps the bytes in an ELF
   (GNU as .incbin) and checks that readelf and llvm-dwarfdump decode exactly
   these rows, that the compilation unit attaches the line table through
   DW_AT_stmt_list, and that an address lookup resolves to file and line.  This
   validates the serializer's encoding against the real oracles before any
   assembler/linker plumbing. */

#include "dwarf.h"
#include "arena.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
write_section(const char *dir, const char *name, const uint8_t *buf, size_t n)
{
    char path[1024];
    FILE *out;

    if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, name) >= sizeof path)
        die("path too long: %s/%s", dir, name);
    out = fopen(path, "wb");
    if (!out)
        die("cannot write %s", path);
    if (fwrite(buf, 1, n, out) != n)
        die("short write to %s", path);
    fclose(out);
}

int
main(int argc, char **argv)
{
    /* Three statements at 8-byte spacing, lines 10/11/12, sequence ending at
       0x118.  The Makefile asserts these exact (line, address) pairs, and that
       a lookup of 0x108 resolves to line 11. */
    static const struct dw_line_row rows[] = {
        { 0x100, 10, 0, 0 },
        { 0x108, 11, 0, 0 },
        { 0x110, 12, 0, 0 },
    };
    static const char *const files[] = { "loc.c" };
    struct arena a;
    uint8_t *buf;
    size_t n;

    util_set_progname("dwline-emit");
    if (argc != 2)
        die("usage: dwline-emit <out-dir>");

    arena_init(&a);

    n = dw_line_build(&a, files, 1, rows, 3, 0x118, &buf, NULL, 0, 4);
    write_section(argv[1], "dwline.bin", buf, n);

    n = dw_abbrev_build(&a, &buf);
    write_section(argv[1], "dwabbrev.bin", buf, n);

    /* The CU spans the rows' code; stmt_list points at offset 0, the sole line
       program in .debug_line. */
    n = dw_info_build(&a, "skjegg", "loc.c", "/skjegg", 0x100, 0x118, 0, &buf,
                      NULL, NULL, 0, 4);
    write_section(argv[1], "dwinfo.bin", buf, n);

    arena_free(&a);
    return 0;
}
