/* romguest: the guest side of the romfs launcher-mount test.
 *
 * Built with the freestanding fs/-interpose libc, run under skj-run with
 * `--rom rom:<image>`.  The launcher loads the packed image into guest memory,
 * writes an FS_KIND_ROM entry into the fs_mounts table and records the image
 * address and length in fs_mount_aux; fs_init adopts the mount by parsing that
 * image in place.  A "rom:" path then reads straight out of the image with no
 * host interaction.
 *
 * The image (built by skj-mkromfs from the test seed dir) holds greet="hello
 * romfs" and sub/inner="nested".  The test returns 42 only if both read back
 * and a write is refused (the mount is read-only).  Distinct non-42 codes name
 * the step that failed. */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

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

int
main(void)
{
    char buf[64];
    int n, fd;

    /* 1. A top-level file reads back its packed content. */
    n = slurp("rom:greet", buf, sizeof buf);
    if (n < 0)
        return 10;
    if (n != 11 || memcmp(buf, "hello romfs", 11) != 0)
        return 11;

    /* 2. A file in a subdirectory reads back too. */
    n = slurp("rom:sub/inner", buf, sizeof buf);
    if (n < 0)
        return 20;
    if (n != 6 || memcmp(buf, "nested", 6) != 0)
        return 21;

    /* 3. The mount is read-only: a write open is refused. */
    fd = open("rom:greet", O_WRONLY, 0);
    if (fd >= 0) {
        close(fd);
        return 30;
    }

    /* 4. A missing path is not found. */
    fd = open("rom:absent", O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        return 40;
    }

    return 42;
}
