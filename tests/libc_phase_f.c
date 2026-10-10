/* Phase-F libc self-check: the self-contained additions strerror, rand/srand,
   strtod/atof, and sscanf/vsscanf.  All arithmetic is deterministic (fixed
   LCG, IEEE double), so the result is identical on every target.  Returns 42
   when every case holds. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
    /* strerror: the known codes map to their messages */
    expect(strcmp(strerror(0), "Success") == 0);
    expect(strcmp(strerror(ENOMEM), "Cannot allocate memory") == 0);
    expect(strcmp(strerror(ERANGE), "Numerical result out of range") == 0);
    expect(strcmp(strerror(999), "Unknown error") == 0);

    /* rand/srand: the LCG is deterministic, so a fixed seed gives a fixed
       first value, and reseeding repeats it */
    {
        int a, b;
        srand(1);
        a = rand();
        srand(1);
        b = rand();
        expect(a == b);
        expect(a >= 0 && a <= RAND_MAX);
        /* a different seed gives a different first draw here */
        srand(12345);
        expect(rand() != a);
    }

    /* strtod / atof: parse and check via an exact small value and the end
       pointer */
    {
        char *end;
        double d = strtod("  12.5rest", &end);
        expect(d == 12.5);
        expect(strcmp(end, "rest") == 0);
        expect(strtod("-0.25", NULL) == -0.25);
        expect(atof("100") == 100.0);
        expect(strtod("2.5e3", NULL) == 2500.0);
    }

    /* sscanf: integers, a suppressed field, a width, a string, a char, and a
       float; and the EOF / no-match return values */
    {
        int a, b, c;
        unsigned x;
        char s[16], cbuf[2];
        double d;

        expect(sscanf("12 34 56", "%d %d %d", &a, &b, &c) == 3);
        expect(a == 12 && b == 34 && c == 56);

        expect(sscanf("0xFF", "%x", &x) == 1 && x == 255);
        expect(sscanf("0755", "%i", &a) == 1 && a == 493);

        a = b = 0;
        expect(sscanf("5 9 7", "%d %*d %d", &a, &b) == 2);
        expect(a == 5 && b == 7);

        expect(sscanf("12345", "%3d", &a) == 1 && a == 123);

        expect(sscanf("hello 7", "%s %d", s, &a) == 2);
        expect(strcmp(s, "hello") == 0 && a == 7);

        cbuf[0] = 0;
        expect(sscanf("Q", "%c", cbuf) == 1 && cbuf[0] == 'Q');

        expect(sscanf("3.5", "%lf", &d) == 1 && d == 3.5);

        expect(sscanf("x", "%d", &a) == 0);     /* no match, not EOF */
        expect(sscanf("", "%d", &a) == -1);     /* EOF before first conv */
    }

    return fails == 0 ? 42 : 1;
}
