/* parallel.c : run several HV sandboxes at once, each on its own host thread.
 *
 * This is the low-level host seam, not a runner library: a host (a shell server,
 * say) runs one sandbox per connection by calling the public run_*hv entry on
 * its own thread.  Each sandbox runs the compute-bound spin guest (tests/hv/
 * spin.c) under an instruction limit.  A compute-bound guest issues no syscalls,
 * so only the preemption timer bounds it; if the timer were process-global
 * (one shared timer, delivered to an arbitrary thread) some sandboxes would run
 * away and the join below would hang.  With a per-vcpu timer each sandbox is
 * preempted and stops at the limit (status 128 + SIGXCPU).
 */
#include "guest.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NSANDBOX   3
#define MAX_INSNS  40000u
#define WANT_STATUS (128 + 24)      /* SIGXCPU: the instruction-limit stop */

struct job {
    const char *path;
    int rc;                         /* run_*hv return (0 == ran to a stop) */
    int status;                     /* the guest's exit/stop status */
};

static void *
run_one(void *arg)
{
    struct job *j = arg;
    run_opts o;
    char *argv[2];

    memset(&o, 0, sizeof o);
    argv[0] = (char *)j->path;
    argv[1] = NULL;
    o.path = j->path;
    o.argv = argv;
    o.argc = 1;
    o.mem_limit = 256u * 1024u * 1024u;
    o.stack_size = 1u * 1024u * 1024u;
    o.heap_size = 16u * 1024u * 1024u;
    o.max_insns = MAX_INSNS;
    o.quiet = 1;
    j->rc = run_x86_64hv(&o, &j->status);
    return NULL;
}

int
main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/hv-x86_64/spin.elf";
    pthread_t th[NSANDBOX];
    struct job jobs[NSANDBOX];
    int i, fail = 0;

    for (i = 0; i < NSANDBOX; i++) {
        jobs[i].path = path;
        jobs[i].rc = -1;
        jobs[i].status = -1;
    }
    for (i = 0; i < NSANDBOX; i++)
        pthread_create(&th[i], NULL, run_one, &jobs[i]);
    for (i = 0; i < NSANDBOX; i++)
        pthread_join(th[i], NULL);

    for (i = 0; i < NSANDBOX; i++) {
        if (jobs[i].rc != 0 || jobs[i].status != WANT_STATUS) {
            fprintf(stderr, "sandbox %d: rc=%d status=%d (want rc=0 status=%d)\n",
                    i, jobs[i].rc, jobs[i].status, WANT_STATUS);
            fail = 1;
        }
    }
    if (fail) {
        printf("FAIL\n");
        return 1;
    }
    printf("parallel ok: %d HV sandboxes each hit the instruction limit\n",
           NSANDBOX);
    return 0;
}
