/* macho_main.c : skj-ld-aarch64 driver.  Links MH_OBJECT inputs into a PIE
 * MH_EXECUTE for arm64 macOS, hosted on libSystem.
 *
 *   skj-ld-aarch64 -o out [-e entry] obj1.o obj2.o ...
 *
 * The entry defaults to _main; dyld calls it as main() through LC_MAIN. */

#include "macho_ld.h"

#include <stdlib.h>
#include <string.h>

static void
usage(FILE *out, int code)
{
    fprintf(out, "usage: skj-ld-aarch64 [-e entry] -o output obj...\n");
    exit(code);
}

int
main(int argc, char **argv)
{
    struct macho_link ld;
    const char *outpath = NULL;
    const char *entry = "_main";
    int i;

    util_set_progname("skj-ld-aarch64");
    util_die_active = 1;

    memset(&ld, 0, sizeof(ld));
    arena_init(&ld.arena);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) usage(stderr, 1);
            outpath = argv[i];
        } else if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) usage(stderr, 1);
            entry = argv[i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout, 0);
        } else if (argv[i][0] == '-') {
            /* ignore toolchain-style flags a driver might pass */
        } else {
            macho_read_obj(&ld, argv[i]);
        }
    }

    if (!outpath)
        usage(stderr, 1);
    if (ld.nobj == 0)
        die("no input objects");

    macho_link_and_write(&ld, outpath, entry);

    for (i = 0; i < ld.nobj; i++)
        free(ld.objs[i].file);
    free(ld.objs);
    free(ld.gsyms);
    arena_free(&ld.arena);
    return 0;
}
