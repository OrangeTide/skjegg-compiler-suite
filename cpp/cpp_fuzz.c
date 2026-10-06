/* cpp_fuzz.c : a fuzz harness for the C preprocessor (skj-cpp).

   It drives the untrusted-input path of the preprocessor library: cpp_process_file
   over a mutated source, the output discarded to /dev/null. The harness is a
   memory-safety probe, not an output check, so it is built ASan plus UBSan and
   the point is that no input, valid or hostile, drives an out-of-bounds access,
   an overflow, or any other undefined behavior through the macro expander, the
   token paster, the conditional stack, or the directive parser.

   The library reads from a file path (cpp_open -> slurp), so each run writes the
   candidate bytes to a scratch file and preprocesses that. A fatal library error
   (an allocation failure, a read failure) longjmps through util_die_env, so each
   run arms that jump and treats the jump as an ordinary rejected input. No
   include path is configured, so a #include can only name the scratch directory;
   that keeps the run hermetic.

   Two modes, matching kpm_fuzz:

     (default)  Read all of stdin and preprocess it once, then exit 0 whatever
                the preprocessor decides. This is the AFL++ / external
                byte-mutation entry.

     --mutate N [--rng K] seed...   A dependency-free in-tree driver that mutates
                copies of the seeds with a deterministic PRNG. `make
                test-cpp-fuzz` runs this over the cpp test fixtures. */

#include "cpp.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fuzz_mutate.h"

/* The scratch file every run preprocesses; created once by mkstemp. */
static char tmppath[] = "/tmp/skj-cpp-fuzz-XXXXXX";
static int tmp_ready;

static void
run_once(const char *text, size_t len)
{
    struct cpp *p;
    FILE *tf, *out;

    if (!tmp_ready)
        return;

    tf = fopen(tmppath, "wb");
    if (!tf)
        return;
    if (len)
        fwrite(text, 1, len, tf);
    fclose(tf);

    p = cpp_new();
    out = fopen("/dev/null", "w");
    if (out) {
        /* A fatal library error longjmps here instead of exiting; treat it as a
           rejected input and carry on. */
        if (setjmp(util_die_env) == 0) {
            util_die_active = 1;
            cpp_process_file(p, tmppath, out);
        }
        util_die_active = 0;
        fclose(out);
    }
    cpp_free(p);
}

int
main(int argc, char **argv)
{
    int fd = mkstemp(tmppath);
    if (fd >= 0) {
        close(fd);
        tmp_ready = 1;
    }

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
            fprintf(stderr, "usage: cpp_fuzz --mutate N [--rng K] seed.c...\n");
            return 2;
        }
        seeds = calloc((size_t)nseed, sizeof *seeds);
        seedlens = calloc((size_t)nseed, sizeof *seedlens);
        if (!seeds || !seedlens)
            return 1;
        for (s = 0; s < nseed; s++) {
            seeds[s] = fz_slurp_file(argv[argi + s], &seedlens[s]);
            if (!seeds[s]) {
                fprintf(stderr, "cpp_fuzz: cannot read '%s'\n", argv[argi + s]);
                return 1;
            }
            run_once(seeds[s], seedlens[s]);   /* a seed must itself be clean */
        }

        for (i = 0; i < iters; i++) {
            size_t si = fz_rng_below((size_t)nseed);
            size_t mlen;
            char *mbuf = fz_mutate(seeds[si], seedlens[si], &mlen);
            if (!mbuf)
                return 1;
            run_once(mbuf, mlen);
            free(mbuf);
        }

        for (s = 0; s < nseed; s++)
            free(seeds[s]);
        free(seeds);
        free(seedlens);
        if (tmp_ready)
            remove(tmppath);
        fprintf(stderr, "cpp_fuzz: %ld iterations over %d seed(s), no crash\n",
                iters, nseed);
        return 0;
    }

    /* default: one pass over stdin (AFL++ / external byte-mutation entry) */
    {
        size_t len;
        char *text = fz_slurp_stdin(&len);
        if (!text)
            return 1;
        run_once(text, len);
        free(text);
    }
    if (tmp_ready)
        remove(tmppath);
    return 0;
}
