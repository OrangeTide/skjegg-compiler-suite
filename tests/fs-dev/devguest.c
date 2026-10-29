/* devguest: the guest side of the devfs launcher-mount test.
 *
 * Built with the freestanding fs/-interpose libc, run under skj-run with
 * `--dev dev`.  The launcher writes a devfs mount named "dev" into the guest
 * fs_mounts table (no backing fd); fs_init adopts it with the fixed devfs root
 * handle, so a "dev:" path reaches the in-guest device backend with no host
 * interaction at all.
 *
 * The test returns 42 only if the three classic devices behave: null reads as
 * end-of-file and swallows writes, zero yields an endless run of zero bytes and
 * swallows writes, and full fails every write.  Distinct non-42 codes name the
 * step that failed. */

#include <fcntl.h>
#include <unistd.h>

static int
read_one(const char *path, char *buf, int n)
{
    int fd = open(path, O_RDONLY, 0);
    int r;
    if (fd < 0)
        return -1;
    r = read(fd, buf, n);
    close(fd);
    return r;
}

int
main(void)
{
    char buf[16];
    int fd, i, n;

    /* 1. dev:null reads as end-of-file. */
    n = read_one("dev:null", buf, 8);
    if (n != 0)
        return 10;

    /* 2. dev:zero yields a run of zero bytes over a pre-dirtied buffer. */
    for (i = 0; i < 8; i++)
        buf[i] = 0x55;
    n = read_one("dev:zero", buf, 8);
    if (n != 8)
        return 11;
    for (i = 0; i < 8; i++)
        if (buf[i] != 0)
            return 12;

    /* 3. A write to dev:null is swallowed (reports the full count). */
    fd = open("dev:null", O_WRONLY, 0);
    if (fd < 0)
        return 20;
    if (write(fd, "hello", 5) != 5) {
        close(fd);
        return 21;
    }
    close(fd);

    /* 4. dev:zero also swallows writes. */
    fd = open("dev:zero", O_WRONLY, 0);
    if (fd < 0)
        return 30;
    if (write(fd, "abc", 3) != 3) {
        close(fd);
        return 31;
    }
    close(fd);

    /* 5. Every write to dev:full fails (the device is always full). */
    fd = open("dev:full", O_WRONLY, 0);
    if (fd < 0)
        return 40;
    if (write(fd, "x", 1) >= 0) {
        close(fd);
        return 41;
    }
    close(fd);

    return 42;
}
