/* capguest: the guest side of the confined-capability mount test.
 *
 * Built with the freestanding fs/-interpose libc (open/read/write/close route
 * through fs/) and WITHOUT FS_AMBIENT_DEFAULT, so this is the folded-out default
 * policy: the current mount is the scratch ramfs and the host is reachable only
 * through a granted mount.  Run under skj-run with `--mount data:<dir>`, so the
 * launcher opens <dir> as a capability fd and writes a "data" host mount into
 * the guest fs_mounts table; a path through that mount reaches the confined
 * hostcap backend, which resolves it with openat against the capability fd.
 *
 * The test returns 42 only if all hold:
 *   - a seeded file reached through "data:" reads back its known content
 *     (the launcher wired the granted directory to the named mount),
 *   - a file created and written through "data:" reads back byte for byte
 *     (confined read/write/close over the capability fd),
 *   - a "data:../escape.txt" open is denied (the hostcap backend's ".." guard),
 *     even though an escape target exists just outside the granted directory,
 *   - a bare path round-trips in the scratch ramfs (the default landing), and
 *   - a bare "seed.txt" does NOT find the host seed, confirming host access is
 *     capability-only with no ambient default.
 *
 * It also checks the kernel's own confinement, independent of the backend: it
 * reads the launcher's raw capability fd straight out of fs_mounts[0] (before
 * any fs/ call adopts the table) and issues a raw openat on it with a ".."
 * path.  skj-run resolves a capability dirfd with openat2(RESOLVE_BENEATH), so
 * even this hand-crafted escape is refused by the host kernel, not by the
 * cooperating backend.
 *
 * Distinct non-42 exit codes name the step that failed, so a CI log pins it. */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/* The guest fs_mounts table, enough of struct fs_mount (16 bytes: char[8],
 * int16 fd, int16 cwd, uint32 flags) to read the launcher's raw capability fd.
 * Read before any fs/ call, so fs_init has not yet adopted (rewritten) fd. */
struct cap_mount { char name[8]; short fd; short cwd; unsigned int flags; };
extern struct cap_mount fs_mounts[];

/* The crt's raw openat (not fs/-interposed): the hostile path under test. */
int openat(int dirfd, const char *path, int flags, int mode);

/* NUL-terminated so lengths come from strlen: skj-cc miscompiles sizeof of a
 * static-storage char[] with an initializer-deduced bound (it returns garbage),
 * so this test avoids sizeof on these arrays. */
static const char SEED[]  = "capability-granted\n";
static const char WROTE[] = "written-through-the-capability\n";

/* Read the whole file at `path` into buf (cap bytes); return the length, or a
 * negative value if the open or a read failed. */
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
    char buf[128];
    int seedlen = (int)strlen(SEED);
    int wrotelen = (int)strlen(WROTE);
    int n, fd;

    /* 0. Kernel confinement: a raw openat on the launcher's capability fd with
     *    a ".." path must be refused by skj-run (RESOLVE_BENEATH), bypassing the
     *    backend entirely.  Done first, while fs_mounts[0].fd is still the raw
     *    capability fd the launcher wrote. */
    fd = openat((int)fs_mounts[0].fd, "../escape.txt", O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        return 5;
    }

    /* 1. The seeded file, reached through the named mount. */
    n = slurp("data:seed.txt", buf, sizeof buf);
    if (n < 0)
        return 10;
    if (n != seedlen || memcmp(buf, SEED, n) != 0)
        return 11;

    /* 2. Create, write, and read back a file through the capability. */
    fd = open("data:out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 20;
    if (write(fd, WROTE, wrotelen) != wrotelen) {
        close(fd);
        return 20;
    }
    close(fd);
    n = slurp("data:out.txt", buf, sizeof buf);
    if (n != wrotelen || memcmp(buf, WROTE, n) != 0)
        return 21;

    /* 3. Confinement: a ".." path must not escape the granted directory, even
     *    though escape.txt exists just above it. */
    fd = open("data:../escape.txt", O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        return 30;
    }

    /* 4. The folded-out default: the current mount is the scratch ramfs, not
     *    the host grant, so a bare path round-trips in RAM. */
    fd = open("scratch.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 40;
    if (write(fd, WROTE, wrotelen) != wrotelen) {
        close(fd);
        return 40;
    }
    close(fd);
    n = slurp("scratch.txt", buf, sizeof buf);
    if (n != wrotelen || memcmp(buf, WROTE, n) != 0)
        return 41;

    /* 5. Host access is capability-only: the host seed is NOT reachable by a
     *    bare path (that hits the empty scratch mount), only through "data:".
     *    A bare open that succeeded would mean an ambient host leak. */
    fd = open("seed.txt", O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        return 32;
    }

    return 42;
}
