/* env_test.c : prove the host passes an environment to the guest.
 *
 * The scheduler lays argv and envp on the initial stack and the crt points
 * environ at the envp array; getenv walks it. Run under skj-run with
 * `--env SKJ_TEST=hello`, this prints the value and exits 0; a missing or wrong
 * value is a small nonzero exit.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(void)
{
    const char *v = getenv("SKJ_TEST");
    const char *w = getenv("SKJ_OTHER");

    if (!v) {
        write(2, "no SKJ_TEST\n", 12);
        return 1;
    }
    if (strcmp(v, "hello") != 0) {
        write(2, "bad SKJ_TEST\n", 13);
        return 2;
    }
    if (!w || strcmp(w, "world") != 0) {
        write(2, "bad SKJ_OTHER\n", 14);
        return 3;
    }
    write(1, "env ok\n", 7);
    return 0;
}
