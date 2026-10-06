/* fuzz_mutate.h : shared byte-mutation driver for the in-tree fuzz harnesses.

   A harness seeds this with valid inputs and runs N iterations, each mutating a
   fresh copy of a randomly chosen seed with a deterministic PRNG and running the
   target pipeline on it. The PRNG is seeded explicitly so a run reproduces. The
   functions are static (one harness is one translation unit), so including this
   header gives each harness its own copy with no link coupling. */

#ifndef FUZZ_MUTATE_H
#define FUZZ_MUTATE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* A small xorshift64 PRNG so a run is deterministic and reproducible. */
static uint64_t fz_rng_state = 1;

static void
fz_rng_seed(uint64_t s)
{
    fz_rng_state = s ? s : 1;
}

static uint64_t
fz_rng_next(void)
{
    uint64_t x = fz_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    fz_rng_state = x;
    return x;
}

static size_t
fz_rng_below(size_t n)
{
    return n ? (size_t)(fz_rng_next() % n) : 0;
}

/* Produce one mutated copy of a seed into a freshly allocated buffer. The result
   length is returned through *outlen (it may grow or shrink). The mutations are
   the usual byte-level set: flip bits, replace a byte, truncate, delete a run,
   and duplicate a run, each bounded so the buffer stays reasonable. */
static char *
fz_mutate(const char *seed, size_t seedlen, size_t *outlen)
{
    size_t cap = seedlen + 64;
    char *buf = malloc(cap ? cap : 1);
    size_t len = seedlen;
    int rounds;

    if (!buf)
        return NULL;
    if (seedlen)
        memcpy(buf, seed, seedlen);

    rounds = 1 + (int)fz_rng_below(6);
    while (rounds--) {
        switch (fz_rng_below(5)) {
        case 0:                 /* flip a bit */
            if (len)
                buf[fz_rng_below(len)] ^= (char)(1u << fz_rng_below(8));
            break;
        case 1:                 /* set a random byte */
            if (len)
                buf[fz_rng_below(len)] = (char)fz_rng_below(256);
            break;
        case 2:                 /* truncate */
            if (len)
                len = fz_rng_below(len);
            break;
        case 3: {               /* delete a run */
            if (len > 1) {
                size_t at = fz_rng_below(len);
                size_t run = 1 + fz_rng_below(len - at);
                memmove(buf + at, buf + at + run, len - at - run);
                len -= run;
            }
            break;
        }
        case 4: {               /* duplicate a run (bounded growth) */
            if (len && len + 32 <= cap) {
                size_t at = fz_rng_below(len);
                size_t run = 1 + fz_rng_below(len - at);
                if (run > 32)
                    run = 32;
                if (len + run <= cap) {
                    memmove(buf + at + run, buf + at, len - at);
                    len += run;
                }
            }
            break;
        }
        }
    }
    *outlen = len;
    return buf;
}

static char *
fz_slurp_file(const char *path, size_t *lenp)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *lenp = (size_t)sz;
    return buf;
}

static char *
fz_slurp_stdin(size_t *lenp)
{
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);

    if (!buf)
        return NULL;
    for (;;) {
        size_t room = cap - n;
        size_t got;
        if (room == 0) {
            char *nb;
            cap *= 2;
            nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
            room = cap - n;
        }
        got = fread(buf + n, 1, room, stdin);
        n += got;
        if (got < room)
            break;
    }
    *lenp = n;
    return buf;
}

/* Per-input process isolation. A harness that drives a whole compiler or
   assembler (which keep per-compile state in file-scope statics, freed at the
   end of a compile and not meant to run twice in one process) defines FZ_FORK
   before including this header and runs each input through fz_run_isolated: the
   work happens in a short-lived child, so one input never inherits another's
   global state and a crash reflects that single input. Harnesses whose target
   builds fresh state per call (the KPM reader, the preprocessor) do not need it
   and leave FZ_FORK undefined. Requires a feature macro (e.g. _GNU_SOURCE) for
   fork/waitpid under -std=c99. */
#ifdef FZ_FORK
#include <unistd.h>
#include <sys/wait.h>

/* A per-input wall-clock cap (seconds). A normal compile is milliseconds, so a
   child still running after this long is looping on its input; the alarm's
   default disposition kills it, the parent sees the signal, and the hang is
   reported like any other crash instead of stalling the run forever. */
#ifndef FZ_CHILD_TIMEOUT
#define FZ_CHILD_TIMEOUT 10
#endif

/* Run fn(text, len) in a forked child. Return 1 if the child did not exit
   cleanly (a sanitizer abort exits nonzero or dies by signal), else 0. A failed
   fork falls back to running inline, losing isolation but not coverage. */
static int
fz_run_isolated(void (*fn)(const char *, size_t), const char *text, size_t len)
{
    pid_t pid;
    int status;

    /* FZ_NOFORK runs inline for diagnosis: a sanitizer report then prints to
       the process's own stderr (and aborts it) instead of being confined to a
       child, at the cost of cross-run isolation. */
    if (getenv("FZ_NOFORK")) {
        fn(text, len);
        return 0;
    }

    pid = fork();
    if (pid < 0) {
        fn(text, len);
        return 0;
    }
    if (pid == 0) {
        alarm(FZ_CHILD_TIMEOUT);        /* a hung input self-terminates */
        fn(text, len);
        _exit(0);
    }
    while (waitpid(pid, &status, 0) < 0)
        ;
    return WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
}
#endif /* FZ_FORK */

#endif /* FUZZ_MUTATE_H */
