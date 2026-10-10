/* Phase-G libc check: exit-time flushing and atexit.  main leaves a partial
   line in stdout's buffer (no newline, so line buffering does not flush it)
   and registers two atexit handlers.  Returning from main must run the
   handlers in reverse order and flush the buffer, so the expected stdout is
   "main-end[bye2][bye1]".  Without the crt finalizer the buffer would be lost
   and the handlers would never run.  The exit code is 42 as usual. */

#include <stdio.h>
#include <stdlib.h>

static void
bye1(void)
{
    printf("[bye1]");
}

static void
bye2(void)
{
    printf("[bye2]");
}

int
main(void)
{
    atexit(bye1);
    atexit(bye2);
    printf("main-end");         /* buffered: no newline to trigger a flush */
    return 42;
}
