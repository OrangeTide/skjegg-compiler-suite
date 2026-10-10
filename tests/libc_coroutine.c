/* libc_coro.c : self-checking coroutine test. Returns 42 on success, or a
 * small nonzero code naming the first failed step. No libc dependency beyond
 * the coroutine API, so it runs freestanding and hosted alike. */

#include <coro.h>
#include <stdint.h>

/* A generator: yields start+1, start+2, start+3, then returns start+100. */
static void *
gen(void *arg)
{
    intptr_t s = (intptr_t)arg;

    co_yield((void *)(s + 1));
    co_yield((void *)(s + 2));
    co_yield((void *)(s + 3));
    return (void *)(s + 100);
}

/* An echo: yields its seed once, then returns whatever the next resume hands
 * in. This exercises the resume -> yield value plumbing in both directions. */
static void *
echo(void *arg)
{
    void *in = co_yield(arg);
    return in;
}

static char bufA[8192];
static char bufB[8192];
static char bufE[8192];

int
main(void)
{
    coro_t *a, *b, *e;
    intptr_t v;

    if (co_self() != NULL)
        return 1;                       /* no coroutine runs on the host context */

    a = co_create(bufA, sizeof bufA, gen, (void *)(intptr_t)0);
    b = co_create(bufB, sizeof bufB, gen, (void *)(intptr_t)1000);
    e = co_create(bufE, sizeof bufE, echo, (void *)(intptr_t)7);
    if (a == NULL || b == NULL || e == NULL)
        return 2;
    if (co_done(a) || co_done(b))
        return 3;                       /* fresh coroutines are not done */

    /* Interleave two independent generators: each keeps its own stack. */
    v = (intptr_t)co_resume(a, NULL);  if (v != 1)    return 4;
    v = (intptr_t)co_resume(b, NULL);  if (v != 1001) return 5;
    v = (intptr_t)co_resume(a, NULL);  if (v != 2)    return 6;
    v = (intptr_t)co_resume(b, NULL);  if (v != 1002) return 7;
    v = (intptr_t)co_resume(a, NULL);  if (v != 3)    return 8;

    /* The resume after the last yield gets the body's return value. */
    v = (intptr_t)co_resume(a, NULL);  if (v != 100)  return 9;
    if (!co_done(a))                                  return 10;
    if (co_resume(a, NULL) != NULL)                   return 11;  /* done -> NULL */

    /* b is untouched by a finishing. */
    if (co_done(b))                                   return 12;
    v = (intptr_t)co_resume(b, NULL);  if (v != 1003) return 13;
    v = (intptr_t)co_resume(b, NULL);  if (v != 1100) return 14;
    if (!co_done(b))                                  return 15;

    /* Value handoff: echo yields 7, then returns the 55 the resume passes in. */
    v = (intptr_t)co_resume(e, NULL);        if (v != 7)  return 16;
    v = (intptr_t)co_resume(e, (void *)(intptr_t)55);
    if (v != 55)                                      return 17;
    if (!co_done(e))                                  return 18;

    /* Hold each resume's value in its own local across the following resumes.
       A caller value kept across co_resume must survive the context switch, so
       co_switch has to preserve the caller's callee-saved registers. */
    {
        static char bufH[8192];
        coro_t *h = co_create(bufH, sizeof bufH, gen, (void *)(intptr_t)200);
        intptr_t r1 = (intptr_t)co_resume(h, NULL);   /* 201 */
        intptr_t r2 = (intptr_t)co_resume(h, NULL);   /* 202 */
        intptr_t r3 = (intptr_t)co_resume(h, NULL);   /* 203 */
        intptr_t r4 = (intptr_t)co_resume(h, NULL);   /* 300 (return) */
        if (r1 != 201 || r2 != 202 || r3 != 203 || r4 != 300)
            return 19;
    }

    return 42;
}
