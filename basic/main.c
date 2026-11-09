/* main.c : the BASIC compiler driver (skj-bc).

   The standard front-end pipeline: slurp -> parse_program -> lower_program ->
   regalloc per function -> target_emit. Mirrors moo/main.c and pascal/main.c. */

#include "basic.h"
#include "util.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int basic_debug;

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

static void
dump_tokens(const char *src, size_t len)
{
    Lexer lx;
    lexer_init(&lx, src, len);
    for (;;) {
        Token t = lexer_next(&lx);
        printf("%4d  %-3d", t.line, t.kind);
        if (t.kind == TK_KEYWORD)
            printf("  kw %s", keyword_name(t.keyword));
        else if (t.kind == TK_IDENT || t.kind == TK_STRING_LIT)
            printf("  %.*s", (int)t.length, t.lexeme);
        else if (t.kind == TK_INT_LIT)
            printf("  %lld", (long long)t.int_val);
        else if (t.kind == TK_FLOAT_LIT)
            printf("  %g", t.float_val);
        putchar('\n');
        if (t.kind == TK_EOF || t.kind == TK_ERROR)
            break;
    }
}

static void
usage(const char *prog, FILE *out, int code)
{
    fprintf(out, "usage: %s [options] <file.bas>\n", prog);
    fprintf(out, "  -o <file>       output assembly file\n");
    fprintf(out, "  --meta <file>   write the module interface (.bmi) here\n");
    fprintf(out, "  --emit-module   require the source to be a MODULE\n");
    fprintf(out, "  -I <dir>        add a module import search directory\n");
    fprintf(out, "  -V              show version\n");
    fprintf(out, "  --tokens        dump token stream and exit\n");
    exit(code);
}

int
main(int argc, char **argv)
{
    const char *filename;
    const char *outfile;
    const char *metafile;
    int tokens_only;
    int emit_module;
    const char **import_dirs;
    int nimport_dirs;
    char *src;

    util_set_progname("skj-bc");

    if (setjmp(util_die_env) != 0)
        return 1;
    util_die_active = 1;

    filename = NULL;
    outfile = NULL;
    metafile = NULL;
    tokens_only = 0;
    emit_module = 0;
    import_dirs = malloc((argc > 0 ? argc : 1) * sizeof(*import_dirs));
    if (!import_dirs)
        die("out of memory");
    util_cleanup_push(cleanup_free, import_dirs);
    nimport_dirs = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc)
                usage(argv[0], stderr, 1);
            outfile = argv[i];
        } else if (strcmp(argv[i], "--meta") == 0) {
            if (++i >= argc)
                usage(argv[0], stderr, 1);
            metafile = argv[i];
        } else if (strcmp(argv[i], "--emit-module") == 0) {
            emit_module = 1;
        } else if (strcmp(argv[i], "-I") == 0) {
            if (++i >= argc)
                usage(argv[0], stderr, 1);
            import_dirs[nimport_dirs++] = argv[i];
        } else if (strcmp(argv[i], "-V") == 0) {
            printf("skj-bc %s\n", SKJ_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0], stdout, 0);
        } else if (strcmp(argv[i], "--tokens") == 0) {
            tokens_only = 1;
        } else if (strcmp(argv[i], "-g") == 0) {
            basic_debug = 1;
        } else if (argv[i][0] == '-') {
            usage(argv[0], stderr, 1);
        } else {
            if (filename)
                usage(argv[0], stderr, 1);
            filename = argv[i];
        }
    }
    if (!filename)
        usage(argv[0], stderr, 1);

    src = slurp(filename);
    util_cleanup_push(cleanup_free, src);

    if (tokens_only) {
        dump_tokens(src, strlen(src));
        util_cleanup_run();
        return 0;
    }

    {
        struct arena a;
        struct node *ast;
        struct ir_program *prog;
        struct ir_func *fn;
        FILE *out;

        arena_init(&a);
        util_cleanup_push(cleanup_arena, &a);

        basic_set_import_dirs(import_dirs, nimport_dirs);
        ast = parse_program(&a, src, strlen(src), filename);

        /* Module expectations: --emit-module requires a MODULE source, --meta is
           for a module only, and a module needs --meta to carry its interface. */
        if (emit_module && !ast->name)
            die("--emit-module given but the source has no MODULE statement");
        if (metafile && !ast->name)
            die("--meta given but the source is not a MODULE");
        if (ast->name && !metafile)
            die("a MODULE needs --meta <file> to write its interface");

        prog = lower_program(&a, ast);

        if (metafile) {
            FILE *mf = fopen(metafile, "w");
            if (!mf)
                die("cannot open '%s' for writing", metafile);
            bmi_write(mf, ast);
            fclose(mf);
        }

        for (fn = prog->funcs; fn; fn = fn->next)
            regalloc(fn);

        if (outfile) {
            out = fopen(outfile, "w");
            if (!out)
                die("cannot open '%s' for writing", outfile);
        } else {
            out = stdout;
        }

        target_emit(out, prog);

        if (outfile)
            fclose(out);

        util_cleanup_run();
    }

    return 0;
}
