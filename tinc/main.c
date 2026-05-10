/* main.c : TinC compiler driver */

#include "tinc.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
cleanup_arena(void *p)
{
    arena_free(p);
}

static void
cleanup_free(void *p)
{
    free(p);
}

int
main(int argc, char **argv)
{
    const char *inpath;
    const char *outpath;
    FILE *out;
    char *src;
    struct arena a;
    struct node *ast;
    struct ir_program *prog;
    struct ir_func *fn;
    int i;

    util_set_progname("skj-tinc");

    inpath = NULL;
    outpath = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outpath = argv[++i];
        } else if (strcmp(argv[i], "-V") == 0) {
            printf("skj-tinc %s\n", SKJ_VERSION);
            return 0;
        } else if (argv[i][0] == '-') {
            die("unknown option: %s", argv[i]);
        } else {
            inpath = argv[i];
        }
    }
    if (inpath == NULL)
        die("usage: skj-tinc [-V] [-o out.s] input.tc");

    if (setjmp(util_die_env) != 0)
        return 1;
    util_die_active = 1;

    arena_init(&a);
    util_cleanup_push(cleanup_arena, &a);

    src = slurp(inpath);
    util_cleanup_push(cleanup_free, src);

    lex_init(&a, src, inpath);
    ast = parse_program(&a);
    prog = lower_program(&a, ast);
    for (fn = prog->funcs; fn != NULL; fn = fn->next)
        regalloc(fn);

    if (outpath != NULL) {
        out = fopen(outpath, "w");
        if (out == NULL)
            die("cannot write %s", outpath);
    } else {
        out = stdout;
    }
    target_emit(out, prog);
    if (out != stdout)
        fclose(out);

    util_cleanup_run();

    return 0;
}
