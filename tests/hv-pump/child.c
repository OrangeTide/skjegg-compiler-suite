/* hv-pump demo child: the large-transfer end of a pipe, selected by argv[1].
 *
 * Two guests share one host thread under the cooperative scheduler, so this
 * stresses the HV cores' cooperative park on blocking syscalls:
 *
 *   "w": write PUMP_BYTES bytes of a position-keyed pattern to fd 1 (the pipe's
 *        write end).  The payload is several pipe buffers, so write() blocks on
 *        a full pipe and the writer parks on POLLOUT until the reader drains it.
 *        Between chunks the writer also yields, and at the end sleeps a
 *        millisecond, so the sched_yield and nanosleep parks run too.
 *   "r": read fd 0 to EOF, parking on POLLIN whenever the pipe is empty, check
 *        every byte against the pattern, and report the total.
 *
 * The pattern is position-keyed (byte at global offset k is k & 0xff), so a
 * short read never desynchronizes the check: the reader recomputes from its
 * running total.  The writer always sees one full write per call (the park
 * resumes mid-buffer), so it never has to loop on a short count.
 */

#include <unistd.h>

extern int nanosleep(const void *req, void *rem);
extern int sched_yield(void);

#ifndef PUMP_BYTES
#define PUMP_BYTES 262144u              /* 256 KiB: several pipe buffers */
#endif

struct gtimespec {
    unsigned sec;
    unsigned nsec;
};

static void
emit(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    write(1, s, (int)(p - s));
}

int
main(int argc, char **argv)
{
    if (argc >= 2 && argv[1][0] == 'w') {
        unsigned char buf[4096];
        unsigned total = 0, i;
        struct gtimespec ts;
        int yielded = 0;

        while (total < PUMP_BYTES) {
            unsigned chunk = PUMP_BYTES - total;
            int n;
            if (chunk > sizeof buf)
                chunk = sizeof buf;
            for (i = 0; i < chunk; i++)
                buf[i] = (unsigned char)((total + i) & 0xff);
            n = write(1, buf, (int)chunk);
            if (n <= 0)
                return 4;
            total += (unsigned)n;
            if (!yielded) {             /* exercise the sched_yield park */
                sched_yield();
                yielded = 1;
            }
        }
        ts.sec = 0;
        ts.nsec = 1000000;              /* 1 ms: exercise the nanosleep park */
        nanosleep(&ts, 0);
        return 0;
    }

    if (argc >= 2 && argv[1][0] == 'r') {
        unsigned char buf[4096];
        unsigned total = 0, i;
        int n, bad = 0;

        while ((n = read(0, buf, (int)sizeof buf)) > 0) {
            for (i = 0; i < (unsigned)n; i++) {
                if (buf[i] != (unsigned char)((total + i) & 0xff)) {
                    bad = 1;
                    break;
                }
            }
            total += (unsigned)n;
        }
        if (bad || total != PUMP_BYTES) {
            emit("pump-bad\n");
            return 5;
        }
        emit("pump-ok\n");
        return 0;
    }
    return 7;
}
