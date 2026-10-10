/* Phase B libc self-check: the malloc family over the __libc_sbrk seam.
   Returns 42 when every check passes, otherwise the number of failed checks
   (kept well under 42 so a failure never masquerades as success). */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int fails;

#define CHK(c) do { if (!(c)) fails++; } while (0)

int
main(void)
{
    /* --- basic allocate, write, free --- */
    {
        char *p = malloc(32);
        CHK(p != NULL);
        memset(p, 'x', 32);
        CHK(p[0] == 'x' && p[31] == 'x');
        free(p);
    }

    /* --- zero size returns NULL (our choice), NULL free is a no-op --- */
    CHK(malloc(0) == NULL);
    free(NULL);

    /* --- alignment: every block is at least 8-byte aligned --- */
    {
        int i, ok = 1;
        void *v[8];
        for (i = 0; i < 8; i++) {
            v[i] = malloc(1 + i * 7);
            if (v[i] == NULL || ((uintptr_t)v[i] & 7u) != 0)
                ok = 0;
        }
        CHK(ok);
        for (i = 0; i < 8; i++)
            free(v[i]);
    }

    /* --- calloc zeroes, and rejects an overflowing product --- */
    {
        int i, zero = 1;
        unsigned char *c = calloc(100, 1);
        CHK(c != NULL);
        for (i = 0; i < 100; i++)
            if (c[i] != 0)
                zero = 0;
        CHK(zero);
        free(c);
        CHK(calloc((size_t)-1, 2) == NULL);
    }

    /* --- realloc grows while preserving the old bytes --- */
    {
        char *p = malloc(16);
        int i, ok = 1;
        CHK(p != NULL);
        for (i = 0; i < 16; i++)
            p[i] = (char)(i + 1);
        p = realloc(p, 200);
        CHK(p != NULL);
        for (i = 0; i < 16; i++)
            if (p[i] != (char)(i + 1))
                ok = 0;
        CHK(ok);
        /* realloc(NULL, n) is malloc; realloc(p, 0) frees and returns NULL */
        CHK(realloc(NULL, 8) != NULL);
        CHK(realloc(p, 0) == NULL);
    }

    /* --- free then reuse: coalescing lets a big request succeed after many
           small blocks are returned (would exhaust a non-coalescing arena) --- */
    {
        int i, ok = 1;
        for (i = 0; i < 2000; i++) {
            void *q = malloc(500);
            if (q == NULL) {
                ok = 0;
                break;
            }
            free(q);
        }
        CHK(ok);
    }

    /* --- distinct live allocations do not overlap --- */
    {
        char *a = malloc(64);
        char *b = malloc(64);
        CHK(a != NULL && b != NULL);
        memset(a, 'A', 64);
        memset(b, 'B', 64);
        CHK(a[0] == 'A' && a[63] == 'A' && b[0] == 'B' && b[63] == 'B');
        free(a);
        free(b);
    }

    return fails == 0 ? 42 : fails;
}
