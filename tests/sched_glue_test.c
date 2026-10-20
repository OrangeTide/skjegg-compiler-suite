/* sched_glue_test.c : host unit test for the Sleep glue
 * (runtime/sched_glue.c).  The glue's seam call, nanosleep, is stubbed here so
 * the timespec conversion and the call path are checked without actually
 * sleeping.  The real syscall is exercised by the skj-run sandbox. */

#include <stdio.h>
#include <stdint.h>

void kp_sleep_ms(int ms);

/* Stubbed seam: record the last timespec nanosleep saw and count the calls. */
struct kp_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

static int      g_nanosleep_calls;
static int32_t  g_last_sec, g_last_nsec;

int
nanosleep(const struct kp_timespec *req, struct kp_timespec *rem)
{
    (void)rem;
    g_nanosleep_calls++;
    g_last_sec = req->tv_sec;
    g_last_nsec = req->tv_nsec;
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
    /* 2500 ms -> {2 s, 500 ms}. */
    kp_sleep_ms(2500);
    check(g_nanosleep_calls == 1, "sleep calls nanosleep once");
    check(g_last_sec == 2, "sleep 2500ms -> 2 s");
    check(g_last_nsec == 500000000, "sleep 2500ms -> 500 ms");

    /* Sub-second: 7 ms -> {0 s, 7 ms}. */
    kp_sleep_ms(7);
    check(g_last_sec == 0 && g_last_nsec == 7000000, "sleep 7ms -> {0, 7ms}");

    /* A non-positive duration parks for no time (Sleep(0) is the bare yield). */
    kp_sleep_ms(0);
    check(g_last_sec == 0 && g_last_nsec == 0, "sleep 0 -> zero timespec");
    kp_sleep_ms(-5);
    check(g_last_sec == 0 && g_last_nsec == 0, "sleep negative -> zero timespec");

    if (failures == 0)
        printf("PASS: sched glue (sleep)\n");
    return failures ? 1 : 0;
}
