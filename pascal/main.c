/* main.c : Pascal compiler driver */

#include "pascal.h"
#include "arena.h"
#include "target.h"
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
    const char *metapath;
    const char *defines[64];
    const char *updirs[64];
    int nupdirs = 0;
    int ndefines = 0;
    int emit_unit = 0;
    char srcdir[1024];
    FILE *out;
    char *src;
    struct arena a;
    struct node *ast;
    struct ir_program *prog;
    struct ir_func *fn;
    int i;

    util_set_progname("skj-pc");

    inpath = NULL;
    outpath = NULL;
    metapath = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outpath = argv[++i];
        } else if (strcmp(argv[i], "-V") == 0) {
            printf("skj-pc %s\n", SKJ_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: skj-pc [-V] [-d SYM] [--emit-unit --meta U.ppm] "
                   "[-o out.s] input.pas\n");
            return 0;
        } else if (strcmp(argv[i], "-g") == 0) {
            pascal_debug = 1;
        } else if (strcmp(argv[i], "-ffunction-sections") == 0) {
            target_function_sections = 1;
        } else if (strcmp(argv[i], "--emit-unit") == 0) {
            emit_unit = 1;
        } else if (strcmp(argv[i], "--meta") == 0 && i + 1 < argc) {
            metapath = argv[++i];
        } else if (strncmp(argv[i], "-U", 2) == 0) {
            /* -U<dir> or -U <dir>: a unit search directory */
            const char *dir = argv[i][2] ? argv[i] + 2
                            : (i + 1 < argc ? argv[++i] : NULL);
            if (dir == NULL)
                die("-U requires a directory");
            if (nupdirs < 64)
                updirs[nupdirs++] = dir;
            else
                die("too many -U directories");
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            if (ndefines < 64)
                defines[ndefines++] = argv[++i];
            else
                die("too many -d defines");
        } else if (argv[i][0] == '-') {
            die("unknown option: %s", argv[i]);
        } else {
            inpath = argv[i];
        }
    }
    if (inpath == NULL)
        die("usage: skj-pc [-V] [-o out.s] input.pas");
    if (emit_unit && metapath == NULL)
        die("--emit-unit requires --meta <file> for the interface member");

    if (setjmp(util_die_env) != 0)
        return 1;
    util_die_active = 1;

    arena_init(&a);
    util_cleanup_push(cleanup_arena, &a);

    src = slurp(inpath);
    util_cleanup_push(cleanup_free, src);

    lex_init(&a, src, inpath);
    for (i = 0; i < ndefines; i++)
        lex_define(defines[i]);
    ast = parse_program(&a);

    if (emit_unit) {
        FILE *mf;

        if (!ast->is_unit)
            die("--emit-unit: %s is a program, not a unit", inpath);
        mf = fopen(metapath, "w");
        if (mf == NULL)
            die("cannot write %s", metapath);
        /* Header line: magic, format version, unit name, the target
           architecture, and the interface dependency list (the first ast->ival
           entries of the uses chain ast->c, the interface-level uses).  A
           consumer refuses a unit built for a different arch: the object member
           is native code bound to one ISA, and record layouts are recomputed
           from the re-parsed interface source (which the matching arch, hence
           pointer width and alignment, keeps consistent).  It also loads the
           listed dependencies, which the interface source may reference.  The
           body is the verbatim interface-section declaration source. */
        fprintf(mf, "(* skj-pascal-unit 1 %s %s", ast->name, SKJ_KPM_ARCH);
        if (ast->ival > 0) {
            struct node *u = ast->c;
            int k;
            fputs(" uses:", mf);
            for (k = 0; k < ast->ival && u; k++, u = u->next)
                fprintf(mf, "%s%s", k ? "," : "",
                        u->raw_name ? u->raw_name : u->name);
        }
        fputs(" *)\n", mf);
        if (ast->sval)
            fputs(ast->sval, mf);
        fputc('\n', mf);
        if (fclose(mf) != 0)
            die("error writing %s", metapath);
    }

    /* The source file's directory is searched first for a used unit. */
    {
        const char *slash = strrchr(inpath, '/');
        if (slash) {
            size_t dl = (size_t)(slash - inpath);
            if (dl >= sizeof(srcdir))
                dl = sizeof(srcdir) - 1;
            memcpy(srcdir, inpath, dl);
            srcdir[dl] = '\0';
        } else {
            srcdir[0] = '\0';
        }
    }
    parse_resolve_uses(&a, ast, srcdir, updirs, nupdirs);

    if (pascal_debug)
        pascal_src_file = inpath;
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
#ifdef CC_WASM
    /* The WASM backend emits WAT text by default, a complete binary module for a
       .wasm output, and a relocatable object for a .o output, matching the other
       WASM drivers. */
    wasm_set_output_format(outpath);
#endif
    target_emit(out, prog);
    if (out != stdout)
        fclose(out);

    util_cleanup_run();

    return 0;
}
