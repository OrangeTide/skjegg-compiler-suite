/* wld_main.c : skj-ld-wasm, the in-tree WebAssembly linker driver.

   Usage: skj-ld-wasm [-o out.wasm] [--export NAME]... [--no-entry] input...

   Each input is a relocatable object skj-cc-wasm emits, or an ar archive (.a)
   of them; the linker links them into a complete module.  Inputs are processed
   left to right: an object is always linked, while an archive contributes only
   the members that resolve a symbol referenced by an input to its left and not
   yet defined (the usual ar pull), so the C runtime archive links in just the
   libc a program actually calls.  By default it synthesizes the WASI runtime (a
   _start that calls main plus the write/read/_exit/exit seam) so the result
   runs as a WASI command; --no-entry links a plain reactor module with no
   runtime, for host --invoke. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "wld.h"

static void
usage(FILE *f)
{
    fprintf(f, "usage: skj-ld-wasm [-o out.wasm] [--export NAME]... "
               "[--no-entry] input...\n");
}

int
main(int argc, char **argv)
{
    const char *outpath = "a.out.wasm";
    const char **inputs = xmalloc((size_t)argc * sizeof(char *));
    const char **exports = xmalloc((size_t)argc * sizeof(char *));
    int ninputs = 0, nexports = 0, entry = 1, i;
    struct wobjvec vec = { NULL, 0, 0 };

    util_set_progname("skj-ld-wasm");

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0) {
            if (++i >= argc)
                die("skj-ld-wasm: -o needs an argument");
            outpath = argv[i];
        } else if (strncmp(a, "-o", 2) == 0 && a[2]) {
            outpath = a + 2;
        } else if (strcmp(a, "--export") == 0) {
            if (++i >= argc)
                die("skj-ld-wasm: --export needs an argument");
            exports[nexports++] = argv[i];
        } else if (strncmp(a, "--export=", 9) == 0) {
            exports[nexports++] = a + 9;
        } else if (strcmp(a, "--no-entry") == 0) {
            entry = 0;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (a[0] == '-' && a[1]) {
            die("skj-ld-wasm: unknown option %s", a);
        } else {
            inputs[ninputs++] = a;
        }
    }

    if (ninputs == 0) {
        usage(stderr);
        return 1;
    }

    /* Process inputs left to right: link each object; pull from each archive
       the members that resolve symbols left still undefined. */
    for (i = 0; i < ninputs; i++) {
        if (warchive_is_archive(inputs[i]))
            warchive_read(&vec, inputs[i]);
        else
            wobjvec_push(&vec, wobj_read(inputs[i]));
    }

    if (vec.n == 0)
        die("skj-ld-wasm: no object members to link");

    wlink(vec.objs, vec.n, exports, nexports, entry, outpath);
    return 0;
}
