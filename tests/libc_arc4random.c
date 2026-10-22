/* libc arc4random self-check: the CSPRNG surface over the shared ChaCha20
   generator (runtime/rng.c), seeded from the getrandom seam.  The output is
   not reproducible (it seeds from host entropy), so the checks are statistical
   and structural, not value pins: a non-degenerate stream, uniform bounds, and
   the arc4random_uniform contract.  Runs only where the getrandom wrapper lives
   (the RV32 and ColdFire crts).  Returns 42 when every case holds. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int fails;

static void
expect(int cond)
{
    if (!cond)
        fails++;
}

int
main(void)
{
    /* The stream is not stuck: 64 draws are not all equal, and a 32-byte buffer
       is not left all zero.  (A seeded ChaCha20 keystream makes a constant run
       astronomically unlikely; a stuck generator is the real failure this
       catches.) */
    {
        uint32_t first, differ = 0;
        int i;

        first = arc4random();
        for (i = 0; i < 64; i++)
            if (arc4random() != first)
                differ = 1;
        expect(differ);
    }

    {
        uint8_t buf[32];
        int i, nonzero = 0;

        memset(buf, 0, sizeof buf);
        arc4random_buf(buf, sizeof buf);
        for (i = 0; i < (int)sizeof buf; i++)
            if (buf[i] != 0)
                nonzero = 1;
        expect(nonzero);
    }

    /* arc4random_buf of zero length and a null buffer are no-ops (no crash). */
    {
        uint8_t guard = 0xa5;

        arc4random_buf(&guard, 0);
        expect(guard == 0xa5);
        arc4random_buf(NULL, 0);
    }

    /* arc4random_uniform: every draw is in range, a bound of 0 or 1 is 0, and a
       10-bucket histogram over many draws hits every bucket (no gross bias). */
    {
        int counts[10];
        int i;

        expect(arc4random_uniform(0) == 0);
        expect(arc4random_uniform(1) == 0);

        for (i = 0; i < 10; i++)
            counts[i] = 0;
        for (i = 0; i < 10000; i++) {
            uint32_t r = arc4random_uniform(10);
            expect(r < 10);
            counts[r]++;
        }
        for (i = 0; i < 10; i++)
            expect(counts[i] > 0);
    }

    return fails == 0 ? 42 : 1;
}
