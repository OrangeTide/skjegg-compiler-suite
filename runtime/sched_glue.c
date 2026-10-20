/* sched_glue.c : the guest-facing Sleep entry point.
 *
 * Thin glue over the syscall seam (syscall_rv.S): a guest parks for a duration
 * rather than busy-spinning.  A guest reaches it through the FFI (`external
 * cdecl`), the way ReadFileStr is reached.  This is a genuine host service: on
 * skj-run it is the raw nanosleep syscall below, and on boris the host binds
 * kp_sleep_ms by name to its sys_wait(timeout) host-call, so the park policy
 * lives with the scheduler that owns the task.  A bare yield (give up the turn
 * without sleeping) is spelled Sleep(0) until coroutine scheduling lands as its
 * own namespaced unit. */

#include <stdint.h>

/* The syscall seam (syscall_rv.S), the same wrappers libc and the Kobold
   freestanding runtime link.  nanosleep parks for a {tv_sec, tv_nsec} timespec
   (the 32-bit convention this seam and boris share). */
struct kp_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

extern int nanosleep(const struct kp_timespec *req, struct kp_timespec *rem);

/* Park the task for ms milliseconds.  A non-positive duration parks for no time,
   which is the plain give-up-the-turn yield. */
void
kp_sleep_ms(int ms)
{
    struct kp_timespec req;

    if (ms < 0)
        ms = 0;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000;
    (void)nanosleep(&req, (struct kp_timespec *)0);
}
