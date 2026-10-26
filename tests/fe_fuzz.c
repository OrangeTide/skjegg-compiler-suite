/* fe_fuzz.c : a fuzz harness for the IR front-end parsers.

   One harness, parameterized at compile time for a front end:
   FEFUZZ_TINC / FEFUZZ_PASCAL / FEFUZZ_MOO follow the common pipeline
   (lex_init -> parse_program -> lower_program -> regalloc -> target_emit);
   FEFUZZ_CC uses the cc_* spellings of the same pipeline. The emitted
   assembly is discarded. It is built ASan plus UBSan: the point is that no
   source, valid or hostile, drives undefined behavior through the lexer,
   recursive-descent parser, resolver, type checker, or lowering. A fatal
   front-end error longjmps through util_die_env, so each run arms that jump
   and treats it as a rejected input.

   The cc front end lexes C after preprocessing in the real driver; the
   preprocessor is fuzzed on its own (cpp/cpp_fuzz.c), so this harness feeds the
   source straight to the parser, which exercises the lex/parse/lower surface
   the cpp fuzz does not reach.

   Each run rebuilds all per-compile state (a fresh arena) so one input does not
   leak into the next; the front ends keep their AST and IR in that arena, and
   each input runs in a forked child (FZ_FORK) so file-scope compiler state
   never carries across inputs.

   Two modes, matching the other harnesses: a stdin pass (the AFL++ entry) and
   a dependency-free --mutate driver seeded with the front end's test sources. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FZ_FORK

#if defined(FEFUZZ_TINC)
#include "ir.h"
#include "tinc.h"
#define FE_LABEL "tinc"
#define FE_RUN(ap, buf, out) do {                                       \
        struct node *ast;                                               \
        struct ir_program *prog;                                        \
        struct ir_func *fn;                                             \
        lex_init((ap), (buf), "<fuzz>");                                \
        ast = parse_program(ap);                                        \
        prog = lower_program((ap), ast);                                \
        for (fn = prog->funcs; fn; fn = fn->next)                       \
            regalloc(fn);                                               \
        target_emit((out), prog);                                       \
    } while (0)

#elif defined(FEFUZZ_PASCAL)
#include "ir.h"
#include "pascal.h"
#define FE_LABEL "pascal"
#define FE_RUN(ap, buf, out) do {                                       \
        struct node *ast;                                               \
        struct ir_program *prog;                                        \
        struct ir_func *fn;                                             \
        lex_init((ap), (buf), "<fuzz>");                                \
        ast = parse_program(ap);                                        \
        prog = lower_program((ap), ast);                                \
        for (fn = prog->funcs; fn; fn = fn->next)                       \
            regalloc(fn);                                               \
        target_emit((out), prog);                                       \
    } while (0)

#elif defined(FEFUZZ_MOO)
#include "ir.h"
#include "moo.h"
#define FE_LABEL "moo"
#define FE_RUN(ap, buf, out) do {                                       \
        struct node *ast;                                               \
        struct ir_program *prog;                                        \
        struct ir_func *fn;                                             \
        lex_init((ap), (buf), "<fuzz>", 0);                             \
        ast = parse_program(ap);                                        \
        prog = lower_program((ap), ast);                                \
        for (fn = prog->funcs; fn; fn = fn->next)                       \
            regalloc(fn);                                               \
        target_emit((out), prog);                                       \
    } while (0)

#elif defined(FEFUZZ_CC)
#include "cc.h"
#define FE_LABEL "cc"
#define FE_RUN(ap, buf, out) do {                                       \
        struct cc_node *ast;                                            \
        struct ir_program *prog;                                        \
        struct ir_func *fn;                                             \
        cc_lex_init((ap), (buf), "<fuzz>");                             \
        ast = cc_parse_program(ap);                                     \
        prog = cc_lower_program((ap), ast);                             \
        for (fn = prog->funcs; fn; fn = fn->next)                       \
            regalloc(fn);                                               \
        target_emit((out), prog);                                       \
    } while (0)

#else
#error "define FEFUZZ_TINC, FEFUZZ_PASCAL, FEFUZZ_MOO, or FEFUZZ_CC"
#endif

#include "fuzz_mutate.h"

static void
run_once(const char *text, size_t len)
{
    struct arena a;
    char *buf;
    FILE *out;

    buf = malloc(len + 1);
    if (!buf)
        return;
    if (len)
        memcpy(buf, text, len);
    buf[len] = '\0';

    /* arena_init before the guard so arena_free after an early longjmp is
       always on an initialized arena. */
    arena_init(&a);
    out = fopen("/dev/null", "w");
    if (out) {
        if (setjmp(util_die_env) == 0) {
            util_die_active = 1;
            FE_RUN(&a, buf, out);
        }
        util_die_active = 0;
        fclose(out);
    }
    arena_free(&a);
    free(buf);
}

int
main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--mutate") == 0) {
        long iters;
        int argi = 2;
        char **seeds;
        size_t *seedlens;
        int nseed, s, i;

        iters = strtol(argv[argi++], NULL, 10);
        if (iters < 0)
            iters = 0;
        if (argi + 1 < argc && strcmp(argv[argi], "--rng") == 0) {
            fz_rng_seed((uint64_t)strtoull(argv[argi + 1], NULL, 10));
            argi += 2;
        }
        nseed = argc - argi;
        if (nseed <= 0) {
            fprintf(stderr, "usage: fe_fuzz --mutate N [--rng K] seed...\n");
            return 2;
        }
        seeds = calloc((size_t)nseed, sizeof *seeds);
        seedlens = calloc((size_t)nseed, sizeof *seedlens);
        if (!seeds || !seedlens)
            return 1;
        for (s = 0; s < nseed; s++) {
            seeds[s] = fz_slurp_file(argv[argi + s], &seedlens[s]);
            if (!seeds[s]) {
                fprintf(stderr, "fe_fuzz: cannot read '%s'\n", argv[argi + s]);
                return 1;
            }
            if (fz_run_isolated(run_once, seeds[s], seedlens[s])) {
                fprintf(stderr, "%s_fuzz: CRASH on seed '%s'\n",
                        FE_LABEL, argv[argi + s]);
                return 1;
            }
        }

        for (i = 0; i < iters; i++) {
            size_t si = fz_rng_below((size_t)nseed);
            size_t mlen;
            char *mbuf = fz_mutate(seeds[si], seedlens[si], &mlen);
            if (!mbuf)
                return 1;
            if (fz_run_isolated(run_once, mbuf, mlen)) {
                fprintf(stderr, "%s_fuzz: CRASH at iteration %d\n", FE_LABEL, i);
                free(mbuf);
                return 1;
            }
            free(mbuf);
        }

        for (s = 0; s < nseed; s++)
            free(seeds[s]);
        free(seeds);
        free(seedlens);
        fprintf(stderr, "%s_fuzz: %ld iterations over %d seed(s), no crash\n",
                FE_LABEL, iters, nseed);
        return 0;
    }

    {
        size_t len;
        char *text = fz_slurp_stdin(&len);
        if (!text)
            return 1;
        run_once(text, len);
        free(text);
    }
    return 0;
}
