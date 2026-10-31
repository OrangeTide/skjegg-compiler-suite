/* spawn-mounts demo: the child.  It was spawned with SKJ_SPAWN_FLAG_SHARE_MOUNTS
 * set, so it inherits the parent's "data" capability mount.  It reads the seeded
 * file through that mount and echoes the content, then exits 0.  If the mount
 * did not reach it (inheritance broke), the open fails and it exits 3. */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int
main(void)
{
    char buf[128];
    int fd, n, total = 0;

    fd = open("data:seed.txt", O_RDONLY, 0);
    if (fd < 0) {
        static const char msg[] = "child: no inherited mount\n";
        write(1, msg, (int)strlen(msg));
        return 3;
    }
    while (total < (int)sizeof buf
           && (n = read(fd, buf + total, (int)sizeof buf - total)) > 0)
        total += n;
    close(fd);
    if (total <= 0)
        return 4;
    write(1, buf, total);
    return 0;
}
