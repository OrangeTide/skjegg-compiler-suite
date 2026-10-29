/* p9guest: the guest side of the 9pfs launcher-mount test.
 *
 * Built with the freestanding fs/-interpose libc, run under skj-run with
 * `--9p data:<dir>`.  The launcher serves <dir> with its in-process 9P2000
 * server behind a shared doorbell channel, writes an FS_KIND_9P entry into the
 * fs_mounts table and records the channel address in fs_mount_aux; fs_init
 * builds the channel transport and does the 9p version+attach.  A "data:" path
 * then reaches real host files over the protocol.
 *
 * The test returns 42 only if a seeded file reads back, a created file writes
 * and reads back, and a made subdirectory holds a nested file.  Distinct
 * non-42 codes name the step that failed. */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

static int
slurp(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY, 0);
    int n, total = 0;
    if (fd < 0)
        return -1;
    while (total < cap && (n = read(fd, buf + total, cap - total)) > 0)
        total += n;
    close(fd);
    return (n < 0) ? -1 : total;
}

static int
put(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int n, len = (int)strlen(data);
    if (fd < 0)
        return -1;
    n = write(fd, data, len);
    close(fd);
    return n == len ? 0 : -1;
}

int
main(void)
{
    char buf[64];
    int n;

    /* 1. The seeded file, served over 9p. */
    n = slurp("data:seed.txt", buf, sizeof buf);
    if (n < 0)
        return 10;
    if (n != 8 || memcmp(buf, "9p-seed\n", 8) != 0)
        return 11;

    /* 2. Create, write and read back a file. */
    if (put("data:out.txt", "written-over-9p") != 0)
        return 20;
    n = slurp("data:out.txt", buf, sizeof buf);
    if (n != 15 || memcmp(buf, "written-over-9p", 15) != 0)
        return 21;

    /* 3. Make a directory and a nested file inside it. */
    if (mkdir("data:sub", 0755) != 0)
        return 30;
    if (put("data:sub/inner", "nested-9p") != 0)
        return 31;
    n = slurp("data:sub/inner", buf, sizeof buf);
    if (n != 9 || memcmp(buf, "nested-9p", 9) != 0)
        return 32;

    return 42;
}
