/* libc_threads.c : self-checking cooperative threads test. Returns 42 on
 * success, or a small nonzero code naming the first failed step.
 *
 * It runs a bounded-buffer producer/consumer over one mutex and two condition
 * variables, with yields perturbed by a small LCG so the interleaving varies,
 * and checks that the consumed total equals the produced total. Alongside it
 * exercises a recursive mutex, call_once run exactly once across threads, and
 * thread-specific storage whose destructor fires once per thread at exit. */

#include <threads.h>
#include <stdint.h>

#define NPROD 3
#define NCONS 3
#define ITEMS 10                 /* items produced per producer */
#define CAP   4                  /* ring capacity */
#define TOTAL (NPROD * ITEMS)

static mtx_t lock;
static cnd_t not_full;
static cnd_t not_empty;

static int ring[CAP];
static int head, tail, count;
static int to_consume = TOTAL;   /* items still to be taken by a consumer */

static long produced_sum;
static long consumed_sum;
static int consumed_items;

/* A shared pseudo-random source deciding where to drop a cooperative yield.
 * Single-threaded scheduling means only one thread touches it at a time. */
static unsigned lcg = 1;

static void
maybe_yield(void)
{
    lcg = lcg * 1103515245u + 12345u;
    if ((lcg >> 16) & 1u)
        thrd_yield();
}

/* call_once target: bumped once, with a yield to exercise the path where a
 * second caller observes the init in progress and blocks. */
static once_flag once = ONCE_FLAG_INIT;
static int once_counter;

static void
init_once(void)
{
    once_counter++;
    thrd_yield();
}

/* A TSS key whose destructor counts the threads that exit with a value set. */
static tss_t key;
static int dtor_count;

static void
tss_dtor(void *v)
{
    (void)v;
    dtor_count++;
}

static void
thread_prologue(void)
{
    call_once(&once, init_once);
    tss_set(key, (void *)(intptr_t)1);
}

static int
producer(void *arg)
{
    long id = (long)(intptr_t)arg;
    int k;

    thread_prologue();
    for (k = 0; k < ITEMS; k++) {
        int v = (int)(id * 100 + k + 1);        /* nonzero, distinct */

        mtx_lock(&lock);
        while (count == CAP)
            cnd_wait(&not_full, &lock);
        ring[tail] = v;
        tail = (tail + 1) % CAP;
        count++;
        produced_sum += v;
        cnd_signal(&not_empty);
        mtx_unlock(&lock);

        maybe_yield();
    }
    return 0;
}

static int
consumer(void *arg)
{
    (void)arg;
    thread_prologue();
    for (;;) {
        int v;

        mtx_lock(&lock);
        while (count == 0 && to_consume > 0)
            cnd_wait(&not_empty, &lock);
        if (count == 0 && to_consume == 0) {
            mtx_unlock(&lock);
            break;
        }
        v = ring[head];
        head = (head + 1) % CAP;
        count--;
        to_consume--;
        consumed_sum += v;
        consumed_items++;
        if (to_consume == 0)
            cnd_broadcast(&not_empty);          /* wake idle consumers to exit */
        cnd_signal(&not_full);
        mtx_unlock(&lock);

        maybe_yield();
    }
    return 0;
}

/* A recursive mutex locked to depth three and released, on the main thread. */
static int
check_recursive(void)
{
    mtx_t rm;

    if (mtx_init(&rm, mtx_recursive) != thrd_success)
        return 1;
    if (mtx_lock(&rm) != thrd_success)
        return 2;
    if (mtx_lock(&rm) != thrd_success)          /* owner re-locks */
        return 3;
    if (mtx_trylock(&rm) != thrd_success)       /* owner re-locks via trylock */
        return 4;
    if (mtx_unlock(&rm) != thrd_success)
        return 5;
    if (mtx_unlock(&rm) != thrd_success)
        return 6;
    if (mtx_unlock(&rm) != thrd_success)        /* now fully released */
        return 7;
    if (mtx_trylock(&rm) != thrd_success)       /* acquirable again */
        return 8;
    mtx_unlock(&rm);
    mtx_destroy(&rm);
    return 0;
}

int
main(void)
{
    thrd_t prod[NPROD];
    thrd_t cons[NCONS];
    int i, r, res;

    r = check_recursive();
    if (r != 0)
        return 10 + r;                          /* 11..18 */

    if (tss_create(&key, tss_dtor) != thrd_success)
        return 3;
    mtx_init(&lock, mtx_plain);
    cnd_init(&not_full);
    cnd_init(&not_empty);

    for (i = 0; i < NPROD; i++)
        if (thrd_create(&prod[i], producer, (void *)(intptr_t)(i + 1)) != thrd_success)
            return 4;
    for (i = 0; i < NCONS; i++)
        if (thrd_create(&cons[i], consumer, NULL) != thrd_success)
            return 5;

    for (i = 0; i < NPROD; i++) {
        res = -1;
        if (thrd_join(prod[i], &res) != thrd_success || res != 0)
            return 6;
    }
    for (i = 0; i < NCONS; i++) {
        res = -1;
        if (thrd_join(cons[i], &res) != thrd_success || res != 0)
            return 7;
    }

    if (consumed_items != TOTAL)
        return 8;
    if (produced_sum != consumed_sum)
        return 9;
    if (count != 0 || to_consume != 0)
        return 30;
    if (once_counter != 1)                      /* init ran exactly once */
        return 31;
    if (dtor_count != NPROD + NCONS)            /* one destructor per thread */
        return 32;

    mtx_destroy(&lock);
    cnd_destroy(&not_full);
    cnd_destroy(&not_empty);
    tss_delete(key);
    return 42;
}
