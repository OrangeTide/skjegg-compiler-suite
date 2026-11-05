/* Echo stdin to stdout until EOF, one read() at a time.  Under skj-run a read
   on a pipe with no data yet parks the process (the cooperative blocking-syscall
   yield): the scheduler waits on the fd and re-runs the read when data arrives,
   so the echo is correct whether the input is already buffered or delayed.
   Driven by tests/run-coop-read.sh on every sandbox core. */

#include <unistd.h>

static char buf[256];

int
main(void)
{
    int n;

    n = read(0, buf, sizeof buf);
    while (n > 0) {
        write(1, buf, n);
        n = read(0, buf, sizeof buf);
    }
    return 0;
}
