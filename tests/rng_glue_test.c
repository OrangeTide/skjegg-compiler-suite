/* rng_glue_test.c : host unit test for the guest-facing RNG/clock glue
 * (runtime/rng_glue.c).  The glue's two seam calls, getrandom and
 * clock_gettime, are stubbed here so the seeding, reproducibility, and clock
 * conversion are checked deterministically.  The generators themselves are
 * covered by rng_test.c and the syscalls by the skj-run sandbox. */

#include <stdio.h>
#include <stdint.h>

/* The glue entry points (as a guest declares them, external cdecl). */
void      kp_random_seed(int seed);
void      kp_randomize(void);
double    kp_random_real(void);
int       kp_random_below(int bound);
void      kp_secure_bytes(void *buf, long n);
int       kp_secure_below(int bound);
long long kp_now_ms(void);

/* Stubbed seam.  getrandom yields all-zero entropy (so the CSPRNG seeds with a
   zero key and nonce, giving the known ChaCha20 keystream); clock_gettime
   returns a fixed 32-bit {tv_sec, tv_nsec}. */
int
getrandom(void *buf, int len, unsigned flags)
{
    int i;
    (void)flags;
    for (i = 0; i < len; i++)
        ((uint8_t *)buf)[i] = 0;
    return len;
}

static int32_t g_sec, g_nsec;

int
clock_gettime(int clk_id, void *ts)
{
    int32_t *p = ts;
    (void)clk_id;
    p[0] = g_sec;
    p[1] = g_nsec;
    return 0;
}

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
    int i;

    /* Lazy auto-seed: a draw before any seed call still runs and stays in range. */
    {
        double d = kp_random_real();
        check(d >= 0.0 && d < 1.0, "auto-seeded real in [0,1)");
    }

    /* kp_random_seed pins a reproducible sequence. */
    {
        double a[16], b[16];
        int same = 1;

        kp_random_seed(123);
        for (i = 0; i < 16; i++) a[i] = kp_random_real();
        kp_random_seed(123);
        for (i = 0; i < 16; i++) b[i] = kp_random_real();
        for (i = 0; i < 16; i++) if (a[i] != b[i]) same = 0;
        check(same, "kp_random_seed reproduces its sequence");
    }

    /* kp_randomize reseeds (from the stub's entropy, != seed 123), so the
       sequence changes. */
    {
        double a[16], b[16];
        int diff = 0;

        kp_random_seed(123);
        for (i = 0; i < 16; i++) a[i] = kp_random_real();
        kp_randomize();
        for (i = 0; i < 16; i++) b[i] = kp_random_real();
        for (i = 0; i < 16; i++) if (a[i] != b[i]) diff = 1;
        check(diff, "kp_randomize changes the sequence");
    }

    /* kp_random_below stays in range and 0/negative bounds give 0. */
    {
        int ok = 1;
        kp_random_seed(7);
        for (i = 0; i < 10000; i++) {
            long v = kp_random_below(10);
            if (v < 0 || v >= 10) ok = 0;
        }
        check(ok, "kp_random_below in [0,10)");
        check(kp_random_below(0) == 0, "kp_random_below(0) is 0");
        check(kp_random_below(-5) == 0, "kp_random_below(negative) is 0");
    }

    /* kp_now_ms converts the 32-bit timespec to milliseconds. */
    {
        g_sec = 5; g_nsec = 250000000;          /* 5.25 s */
        check(kp_now_ms() == 5250, "kp_now_ms converts to ms");
        g_sec = 0; g_nsec = 0;
        check(kp_now_ms() == 0, "kp_now_ms zero");
    }

    /* kp_secure_bytes, seeded from the zero stub entropy, gives the known
       ChaCha20 zero-key keystream. */
    {
        uint8_t out[8];
        static const uint8_t want[8] =
            { 0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90 };
        int ok = 1;

        kp_secure_bytes(out, 8);
        for (i = 0; i < 8; i++) if (out[i] != want[i]) ok = 0;
        check(ok, "kp_secure_bytes zero-entropy keystream");
    }

    /* kp_secure_below stays in range and 0/negative bounds give 0. */
    {
        int ok = 1;
        for (i = 0; i < 10000; i++) {
            int v = kp_secure_below(10);
            if (v < 0 || v >= 10) ok = 0;
        }
        check(ok, "kp_secure_below in [0,10)");
        check(kp_secure_below(0) == 0, "kp_secure_below(0) is 0");
        check(kp_secure_below(-5) == 0, "kp_secure_below(negative) is 0");
    }

    if (failures == 0)
        printf("PASS: rng glue (seed, randomize, below, now_ms, secure)\n");
    return failures ? 1 : 0;
}
