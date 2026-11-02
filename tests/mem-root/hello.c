/* A trivial guest for the --mem root test: write a known line and exit 0.  Run
   once from its file and once with --mem (the root loaded from an in-memory
   image via sched_root.image); both must behave identically. */
#include <unistd.h>

int
main(void)
{
    write(1, "mem-root ok\n", 12);
    return 0;
}
