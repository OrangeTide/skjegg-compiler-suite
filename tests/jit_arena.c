/* jit_arena.c : the guest runtime arena the JIT hands Excelsior programs.
 *
 * The arena must sit below 4GB (a guest pointer is 32 bits), map lazily (a
 * C-only guest never allocates from it), align every allocation to a word,
 * and fail cleanly when exhausted rather than hand back an out-of-range or
 * overlapping address. Host-only: no cross toolchain, no qemu. Built and run
 * by `make test-jit-arena`.
 */

#include "jit_arena.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures;

static void
check(const char *what, int ok)
{
    if (!ok) {
        printf("FAIL  %s\n", what);
        failures++;
    } else {
        printf("ok    %s\n", what);
    }
}

int
main(void)
{
    struct kp_arena a;
    void *p1, *p2, *p3;

    memset(&a, 0, sizeof a);
    check("a fresh arena is unmapped", a.base == NULL);

    p1 = kp_arena_alloc(&a, 4);
    check("first alloc maps the arena", a.base != NULL);
    check("first alloc succeeds", p1 != NULL);
    check("the arena is low (< 4GB)", (uintptr_t)a.base < 0x100000000ull);
    check("the alloc is low (< 4GB)", (uintptr_t)p1 < 0x100000000ull);

    /* a 4-byte word request advances the bump by 8, keeping the next word
       8-byte aligned (as the guest's i64 and pointer values expect) */
    p2 = kp_arena_alloc(&a, 4);
    check("second alloc is distinct", p2 != NULL && p2 != p1);
    check("allocations are 8-byte strided",
          (uintptr_t)p2 - (uintptr_t)p1 == 8);
    check("an allocation is 8-byte aligned", ((uintptr_t)p2 & 7u) == 0);

    /* the returned memory is writable and reads back what was written */
    *(int32_t *)p1 = 0x11223344;
    *(int32_t *)p2 = 0x55667788;
    check("allocations round-trip a word",
          *(int32_t *)p1 == 0x11223344 && *(int32_t *)p2 == 0x55667788);
    check("distinct allocations do not alias", *(int32_t *)p1 == 0x11223344);

    /* a request larger than the region fails without disturbing the arena */
    p3 = kp_arena_alloc(&a, (int)(64 << 20));
    check("an over-large request returns NULL", p3 == NULL);
    check("a failed alloc leaves the arena usable",
          kp_arena_alloc(&a, 4) != NULL);

    kp_arena_free(&a);
    check("free resets the arena", a.base == NULL && a.off == 0);

    printf("\n%s\n", failures ? "FAILED" : "all arena checks passed");
    return failures ? 1 : 0;
}
