/* rng_test.c : host unit test for the guest generators (runtime/rng.c).
 *
 * xoshiro128** is pinned by a golden sequence from a fixed state, computed from
 * the Blackman/Vigna reference transition, so a bug in the rotate/shift
 * structure is caught; ChaCha20 is pinned against the RFC 8439 known-answer
 * vector.  splitmix32 (the seed expander, which has no single canonical
 * published vector) is covered through the determinism, divergence, and range
 * checks that drive the full seed-then-draw path. */

#include "rng.h"

#include <stdio.h>
#include <stdint.h>

static int failures;

static void
check(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int
main(void)
{
    /* xoshiro128** from the fixed state {1, 2, 3, 4}: the first outputs are
       11520, 0, 5927040, derived by hand from the reference next() transition
       (result = rotl(s1 * 5, 7) * 9).  This pins the core generator
       independently of the seeder. */
    {
        struct kp_rng r;
        r.s[0] = 1; r.s[1] = 2; r.s[2] = 3; r.s[3] = 4;
        check(kp_rng_u32(&r) == 11520u,   "xoshiro128**[0]");
        check(kp_rng_u32(&r) == 0u,       "xoshiro128**[1]");
        check(kp_rng_u32(&r) == 5927040u, "xoshiro128**[2]");
    }

    /* ChaCha20 keystream with an all-zero key, nonce and counter: the first
       output words are 0xade0b876, 0x903df1a0 (RFC 8439 reference). */
    {
        uint8_t key[32] = {0};
        uint8_t nonce[12] = {0};
        struct kp_csprng c;
        uint8_t out[8];
        static const uint8_t want[8] =
            { 0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90 };
        int i, ok = 1;

        kp_csprng_seed(&c, key, nonce);
        kp_csprng_bytes(&c, out, sizeof out);
        for (i = 0; i < 8; i++)
            if (out[i] != want[i])
                ok = 0;
        check(ok, "chacha20 zero-key keystream");
    }

    /* ChaCha20 drawn one byte at a time equals drawn in one call (block
       boundary handling). */
    {
        uint8_t key[32] = {0};
        uint8_t nonce[12] = {0};
        struct kp_csprng a, b;
        uint8_t ba[200], bb[200];
        int i, ok = 1;

        kp_csprng_seed(&a, key, nonce);
        kp_csprng_seed(&b, key, nonce);
        kp_csprng_bytes(&a, ba, sizeof ba);     /* one call spanning 4 blocks */
        for (i = 0; i < (int)sizeof bb; i++)
            kp_csprng_bytes(&b, bb + i, 1);     /* byte at a time */
        for (i = 0; i < (int)sizeof ba; i++)
            if (ba[i] != bb[i])
                ok = 0;
        check(ok, "chacha20 chunked == byte-at-a-time");
    }

    /* xoshiro128**: the same seed reproduces the same sequence; different
       seeds diverge. */
    {
        struct kp_rng x, y, z;
        int i, same_xy = 1, same_xz = 1;

        kp_rng_seed(&x, 12345);
        kp_rng_seed(&y, 12345);
        kp_rng_seed(&z, 12346);
        for (i = 0; i < 100; i++) {
            uint32_t vx = kp_rng_u32(&x);
            if (vx != kp_rng_u32(&y))
                same_xy = 0;
            if (vx != kp_rng_u32(&z))
                same_xz = 0;
        }
        check(same_xy, "xoshiro determinism (same seed)");
        check(!same_xz, "xoshiro divergence (different seed)");
    }

    /* kp_rng_real in [0, 1); kp_rng_below in [0, bound). */
    {
        struct kp_rng x;
        int i, ok_real = 1, ok_below = 1, saw_hi = 0, saw_lo = 0;

        kp_rng_seed(&x, 99);
        for (i = 0; i < 100000; i++) {
            double d = kp_rng_real(&x);
            uint32_t b = kp_rng_below(&x, 7);
            if (d < 0.0 || d >= 1.0)
                ok_real = 0;
            if (b >= 7)
                ok_below = 0;
            if (b == 6)
                saw_hi = 1;
            if (b == 0)
                saw_lo = 1;
        }
        check(ok_real, "kp_rng_real in [0,1)");
        check(ok_below, "kp_rng_below in [0,bound)");
        check(saw_hi && saw_lo, "kp_rng_below covers its range");
        check(kp_rng_below(&x, 0) == 0, "kp_rng_below(0) is 0");
    }

    if (failures == 0)
        printf("PASS: rng (splitmix32, xoshiro128**, chacha20)\n");
    return failures ? 1 : 0;
}
