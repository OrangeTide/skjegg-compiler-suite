/* Phase H libc self-check: the segregated-fit allocator under a randomized
   churn of malloc/realloc/free across a mix of sizes that crosses the exact-bin
   and large-block boundary.  Each live block is filled with a byte pattern
   keyed to its slot, and every pattern is re-verified as the churn proceeds, so
   any overlap, bad split, or botched coalesce corrupts a pattern and fails the
   run.  A final full free plus a live-bytes check catches a leak.  Returns 42.

   Deterministic (a fixed PRNG seed), and bounded well under the 1 MiB seam. */

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern size_t __libc_malloc_live(void);
extern size_t __libc_malloc_peak(void);

#define SLOTS   32
#define ITERS   4000
#define BUDGET  300000u         /* keep concurrent live bytes under the arena */

static uint32_t rng = 0x1234abcdu;

static uint32_t
next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* A size mostly small, occasionally large enough to leave the bins (> 8 KiB). */
static size_t
rand_size(void)
{
    uint32_t r = next_rand();

    if ((r & 15u) == 0u)
        return 1u + (r >> 4) % 9000u;   /* crosses the 8 KiB bin/large boundary */
    return 1u + (r >> 4) % 256u;
}

static unsigned char *slot[SLOTS];
static size_t         slen[SLOTS];
static unsigned char  stag[SLOTS];
static size_t         total;            /* sum of live slot sizes */

static void
fill(int i)
{
    size_t k;

    for (k = 0; k < slen[i]; k++)
        slot[i][k] = (unsigned char)(stag[i] + (unsigned char)k);
}

/* Verify slot i still holds its pattern; return 0 on mismatch. */
static int
verify(int i)
{
    size_t k;

    for (k = 0; k < slen[i]; k++)
        if (slot[i][k] != (unsigned char)(stag[i] + (unsigned char)k))
            return 0;
    return 1;
}

static void
drop(int i)
{
    free(slot[i]);
    total -= slen[i];
    slot[i] = NULL;
    slen[i] = 0;
}

int
main(void)
{
    int i, iter;
    unsigned char *p;
    size_t osz, k;

    /* --- documented edge cases --- */
    if (malloc(0) != NULL)
        return 1;
    free(NULL);
    p = realloc(NULL, 8);               /* realloc(NULL, n) is malloc */
    if (p == NULL)
        return 2;
    free(p);
    p = malloc(8);
    if (p == NULL || realloc(p, 0) != NULL)     /* realloc(p, 0) frees, yields NULL */
        return 3;
    {
        unsigned char *c = calloc(64, 1);
        if (c == NULL)
            return 4;
        for (k = 0; k < 64; k++)
            if (c[k] != 0)
                return 5;
        free(c);
    }
    if (calloc((size_t)-1, 2) != NULL)          /* overflowing product */
        return 6;

    /* --- randomized churn --- */
    for (iter = 0; iter < ITERS; iter++) {
        i = (int)(next_rand() % SLOTS);

        if (slot[i] == NULL) {
            size_t n = rand_size();
            if (total + n > BUDGET)
                continue;               /* stay under the arena */
            p = malloc(n);
            if (p == NULL)
                return 10;              /* a reasonable request must succeed */
            if (((uintptr_t)p & 15u) != 0u)
                return 11;              /* 16-byte alignment */
            slot[i] = p;
            slen[i] = n;
            stag[i] = (unsigned char)(iter * 7 + i);
            total += n;
            fill(i);
        } else if ((next_rand() & 1u) == 0u) {
            if (!verify(i))
                return 12;
            drop(i);
        } else {
            size_t n = rand_size();
            if (!verify(i))
                return 13;
            osz = slen[i];
            if (total - osz + n > BUDGET)
                continue;
            p = realloc(slot[i], n);
            if (p == NULL)
                return 14;
            if (((uintptr_t)p & 15u) != 0u)
                return 15;
            /* the preserved prefix must survive the move */
            for (k = 0; k < (osz < n ? osz : n); k++)
                if (p[k] != (unsigned char)(stag[i] + (unsigned char)k))
                    return 16;
            slot[i] = p;
            slen[i] = n;
            total = total - osz + n;
            fill(i);                    /* restore the whole-block invariant */
        }

        if ((iter & 63) == 0) {         /* periodically re-check every live slot */
            int j;
            for (j = 0; j < SLOTS; j++)
                if (slot[j] != NULL && !verify(j))
                    return 20;
        }
    }

    if (__libc_malloc_peak() == 0)      /* the churn must have allocated */
        return 30;

    /* --- free everything and confirm no bytes remain live --- */
    for (i = 0; i < SLOTS; i++)
        if (slot[i] != NULL)
            drop(i);
    if (__libc_malloc_live() != 0)
        return 31;

    /* the arena is reusable after a full drain */
    p = malloc(1000);
    if (p == NULL)
        return 32;
    free(p);

    return 42;
}
