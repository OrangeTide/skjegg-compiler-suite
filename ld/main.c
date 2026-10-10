/* main.c : skj-ld linker driver */

#include "ld.h"
#include "version.h"

#include <stdlib.h>
#include <string.h>

static void
cleanup_ld(void *p)
{
    ld_free(p);
}

static void
usage(void)
{
    fprintf(stderr,
            "usage: skj-ld [-V] [-T script.ld] [-e entry] -o output"
            " input.o [...]\n");
    exit(1);
}

int
main(int argc, char **argv)
{
    const char *script_path;
    const char *entry_override;
    const char *output_path;
    const char **inputs;
    int ninputs;
    struct linker ld;

    util_set_progname("skj-ld");

    script_path = NULL;
    entry_override = NULL;
    output_path = NULL;
    inputs = NULL;
    ninputs = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-V") == 0) {
            printf("skj-ld %s\n", SKJ_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-T") == 0) {
            if (++i >= argc) usage();
            script_path = argv[i];
        } else if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) usage();
            entry_override = argv[i];
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) usage();
            output_path = argv[i];
        } else if (argv[i][0] == '-') {
            die("unknown option: %s", argv[i]);
        } else {
            inputs = realloc(inputs,
                             (size_t)(ninputs + 1) * sizeof(char *));
            inputs[ninputs++] = argv[i];
        }
    }

    if (!output_path || ninputs == 0)
        usage();

    if (setjmp(util_die_env) != 0) {
        free(inputs);
        return 1;
    }
    util_die_active = 1;

    memset(&ld, 0, sizeof(ld));
    arena_init(&ld.arena);
    util_cleanup_push(cleanup_ld, &ld);

    /* parse linker script */
    if (script_path) {
        char *src = slurp(script_path);
        ld_parse_script(&ld.arena, &ld.script, src);
        free(src);
    } else {
        ld_default_script(&ld.arena, &ld.script);
    }

    if (entry_override)
        ld.script.entry = entry_override;

    /* Read the explicit objects; stash archives for symbol-driven pulls.  An
       archive contributes only the members that resolve a still-undefined
       symbol, so it is processed after the objects that reference into it. */
    const char **archives = NULL;
    int narchives = 0;
    for (int i = 0; i < ninputs; i++) {
        if (ld_is_archive(inputs[i])) {
            archives = realloc(archives,
                               (size_t)(narchives + 1) * sizeof(char *));
            archives[narchives++] = inputs[i];
        } else {
            ld_read_object(&ld.arena, ld_add_object(&ld), inputs[i]);
        }
    }

    /* pull archive members until a full pass adds nothing (a pulled member may
       reference symbols only another archive provides, so loop over them all).
       The loaded-member set makes each member load at most once. */
    ld_archive_reset();
    int pulled = 1;
    while (pulled) {
        pulled = 0;
        for (int i = 0; i < narchives; i++)
            pulled += ld_read_archive(&ld, archives[i]);
    }
    free(archives);

    /* link */
    ld_link(&ld);

    /* write executable */
    ld_write_exec(&ld, output_path);

    util_cleanup_run();
    free(inputs);
    return 0;
}
