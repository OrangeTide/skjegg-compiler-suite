#include <stdlib.h>

/* The linear congruential generator from the C standard's own example.  The
   state is a plain unsigned long; rand() returns the middle bits so the low
   bit is not a short-period toggle.  Single threaded, so no locking. */
static unsigned long state = 1;

int
rand(void)
{
    state = state * 1103515245UL + 12345UL;
    return (int)((state >> 16) & 0x7fffUL);     /* 0 .. RAND_MAX (32767) */
}

void
srand(unsigned seed)
{
    state = seed;
}
