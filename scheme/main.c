/* main.c : TinScheme compiler driver */

#include "scheme.h"
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
cleanup_gc(void *p)
{
    gc_destroy(p);
}

static void
cleanup_free(void *p)
{
    free(p);
}

static void
usage(void)
{
    die("usage: skj-sc [-p] [-o out.s] input.scm");
}

int
main(int argc, char **argv)
{
    const char *inpath;
    const char *outpath;
    char *src;
    struct arena a;
    struct gc_heap heap;
    val_t program = VAL_NIL;
    int print_only;
    int i;

    util_set_progname("skj-sc");

    inpath = NULL;
    outpath = NULL;
    print_only = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outpath = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0) {
            print_only = 1;
        } else if (strcmp(argv[i], "-V") == 0) {
            printf("skj-sc %s\n", SKJ_VERSION);
            return 0;
        } else if (argv[i][0] == '-') {
            die("unknown option: %s", argv[i]);
        } else {
            inpath = argv[i];
        }
    }
    if (inpath == NULL)
        usage();

    if (setjmp(util_die_env) != 0)
        return 1;
    util_die_active = 1;

    arena_init(&a);
    util_cleanup_push(cleanup_arena, &a);

    gc_init(&heap);
    util_cleanup_push(cleanup_gc, &heap);

    src = slurp(inpath);
    util_cleanup_push(cleanup_free, src);

    gc_push(&heap, &program);
    lex_init(&a, src, inpath);
    program = scm_read_all(&heap);

    if (print_only) {
        val_t cur;
        for (cur = program; !IS_NIL(cur); cur = gc_cdr(cur))
            scm_println(gc_car(cur));
        util_cleanup_run();
        return 0;
    }

    {
        struct ir_program *ir;
        struct ir_func *fn;
        FILE *out;

        ir = scm_lower(&a, &heap, program);

        for (fn = ir->funcs; fn; fn = fn->next)
            regalloc(fn);

        out = fopen(outpath ? outpath : "a.out.s", "w");
        if (!out)
            die("cannot open output file");
        target_emit(out, ir);
        if (out != stdout)
            fclose(out);
    }

    util_cleanup_run();
    return 0;
}
