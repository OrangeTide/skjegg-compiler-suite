/* spawn-pipe demo: one image used as either end of a pipe, selected by argv[1].
 *
 *   "w": write a known message to fd 1, which the parent redirected to the
 *        pipe's write end.
 *   "r": read fd 0 to EOF, which the parent redirected to the pipe's read end,
 *        and echo what came through to fd 1 (its own terminal), so the host
 *        sees the piped message.
 */

#include <unistd.h>
#include <string.h>

int
main(int argc, char **argv)
{
    if (argc >= 2 && argv[1][0] == 'w') {
        static const char msg[] = "piped-hello\n";
        write(1, msg, (int)strlen(msg));
        return 0;
    }
    if (argc >= 2 && argv[1][0] == 'r') {
        char buf[64];
        int n, total = 0;
        while (total < (int)sizeof buf
               && (n = read(0, buf + total, (int)sizeof buf - total)) > 0)
            total += n;
        if (total <= 0)
            return 4;
        write(1, buf, total);
        return 0;
    }
    return 7;
}
