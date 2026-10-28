/* as_fuzz.c : a fuzz harness for the in-tree assemblers.

   One harness, parameterized at compile time for the ColdFire, RISC-V, or MIPS
   assembler (ASFUZZ_CF / ASFUZZ_RV / ASFUZZ_MIPS). Each assembler reads a source
   buffer and runs its sizing/encode passes to an ELF object, which the harness
   discards. It is built ASan plus UBSan: the point is that no assembly text,
   valid or hostile, drives an out-of-bounds access, an overflow, or other
   undefined behavior through the shared GAS lexer, the object bookkeeping, or
   the per-target parser and encoder. A fatal assembler error longjmps through
   util_die_env, so each run arms that jump and treats it as a rejected input.

   Two modes, matching the reader and preprocessor harnesses:

     (default)  Assemble one source read from stdin, once (the AFL++ entry).
     --mutate N [--rng K] seed...   Mutate copies of the seeds with a
                deterministic PRNG. `make test-as*-fuzz` runs this over the
                committed .s fixtures (or a generated one for ColdFire). */

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FZ_FORK
#include "fuzz_mutate.h"

#if defined(ASFUZZ_CF)
#include "as.h"
typedef struct assembler as_state;
#define AS_LABEL "as"
static void as_run(as_state *a) { as_pass1(a); as_pass2(a); }
#define AS_INIT(a, s) as_init((a), (s))
#define AS_RUN(a)     as_run(a)
#define AS_WRITE(a,f) elf_write((a), (f))
#define AS_FREE(a)    as_free(a)

#elif defined(ASFUZZ_RV)
#include "rv.h"
typedef struct rv_asm as_state;
#define AS_LABEL "as-rv"
#define AS_INIT(a, s) rv_init((a), (s))
#define AS_RUN(a)     rv_assemble(a)
#define AS_WRITE(a,f) rv_elf_write((a), (f))
#define AS_FREE(a)    rv_free(a)

#elif defined(ASFUZZ_MIPS)
#include "mips.h"
typedef struct mips_asm as_state;
#define AS_LABEL "as-mips"
#define AS_INIT(a, s) mips_init((a), (s))
#define AS_RUN(a)     mips_assemble(a)
#define AS_WRITE(a,f) mips_elf_write((a), (f))
#define AS_FREE(a)    mips_free(a)

#elif defined(ASFUZZ_X86_64)
#include "x86_64.h"
typedef struct x86_64_asm as_state;
#define AS_LABEL "as-x86-64"
#define AS_INIT(a, s) x86_64_init((a), (s))
#define AS_RUN(a)     x86_64_assemble(a)
#define AS_WRITE(a,f) x86_64_elf_write((a), (f))
#define AS_FREE(a)    x86_64_free(a)

#elif defined(ASFUZZ_AARCH64)
#include "aarch64.h"
typedef struct a64_asm as_state;
#define AS_LABEL "as-aarch64"
#define AS_INIT(a, s) a64_init((a), (s))
#define AS_RUN(a)     a64_assemble(a)
#define AS_WRITE(a,f) a64_elf_write((a), (f))
#define AS_FREE(a)    a64_free(a)

#else
#error "define ASFUZZ_CF, ASFUZZ_RV, ASFUZZ_MIPS, ASFUZZ_X86_64, or ASFUZZ_AARCH64"
#endif

/* Assemble one NUL-terminated source. The assemblers keep pointers into the
   source buffer, so it must outlive the passes; the harness owns it. */
static void
run_once(const char *text, size_t len)
{
    as_state a;
    char *buf;
    FILE *out;

    buf = malloc(len + 1);
    if (!buf)
        return;
    if (len)
        memcpy(buf, text, len);
    buf[len] = '\0';

    /* Zero the state so an AS_FREE after an early longjmp (before AS_INIT set
       its fields) frees nothing wild. */
    memset(&a, 0, sizeof a);

    out = fopen("/dev/null", "wb");
    if (out) {
        if (setjmp(util_die_env) == 0) {
            util_die_active = 1;
            AS_INIT(&a, buf);
            AS_RUN(&a);
            AS_WRITE(&a, out);
        }
        util_die_active = 0;
        fclose(out);
    }
    AS_FREE(&a);
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
            fprintf(stderr, "usage: as_fuzz --mutate N [--rng K] seed.s...\n");
            return 2;
        }
        seeds = calloc((size_t)nseed, sizeof *seeds);
        seedlens = calloc((size_t)nseed, sizeof *seedlens);
        if (!seeds || !seedlens)
            return 1;
        for (s = 0; s < nseed; s++) {
            seeds[s] = fz_slurp_file(argv[argi + s], &seedlens[s]);
            if (!seeds[s]) {
                fprintf(stderr, "as_fuzz: cannot read '%s'\n", argv[argi + s]);
                return 1;
            }
            if (fz_run_isolated(run_once, seeds[s], seedlens[s])) {
                fprintf(stderr, "%s_fuzz: CRASH on seed '%s'\n",
                        AS_LABEL, argv[argi + s]);
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
                fprintf(stderr, "%s_fuzz: CRASH at iteration %d\n", AS_LABEL, i);
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
                AS_LABEL, iters, nseed);
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
