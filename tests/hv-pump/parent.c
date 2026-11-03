/* hv-pump demo parent: wire a pipe between two spawned children, LP64-safe.
 *
 * The same shape as the spawn-pipe parent (create a pipe, spawn a writer with
 * the write end as fd 1 and a reader with the read end as fd 0, drop our own
 * ends, wait both), but the spawn descriptor holds its guest pointers in
 * `unsigned long` so it is correct on an LP64 target.  The spawn-pipe parent
 * uses `unsigned`, which only spans a pointer on the ILP32 cores; this test
 * runs on the LP64 HV tier, where that would truncate the image address.
 */

#include <unistd.h>
#include <string.h>

extern unsigned skj_spawnv(const void *req);
extern unsigned skj_wait(unsigned handle);
int pipe(int fd[2]);
int fs_fd_backing(int fd, int *gfd);

#include "child_image.h"        /* child_img[], CHILD_IMG_LEN */

/* One fd-map entry: the capability index backing a parent fd, and the id the
 * child should see it as.  The host reads these as a {gfd, dst} pair of 32-bit
 * words, as jsh does. */
struct fdent {
    unsigned gfd;
    unsigned dst;
};

/* The spawn descriptor.  Each pointer-bearing field is `unsigned long`, a guest
 * pointer's width on both ILP32 and LP64 (mirrors examples/jsh/jsh.c). */
struct spawn_req {
    unsigned long size;
    unsigned long image;
    unsigned long image_len;
    unsigned long argv;
    unsigned long argc;
    unsigned long flags;
    unsigned long fdmap;
    unsigned long fdmap_n;
};

static void
emit(const char *s)
{
    write(1, s, (int)strlen(s));
}

static unsigned
spawn_with(const char *role, unsigned gfd, unsigned dst)
{
    const char *av[2];
    struct fdent map[1];
    struct spawn_req req;

    av[0] = "child";
    av[1] = role;
    map[0].gfd = gfd;
    map[0].dst = dst;
    req.size = sizeof req;
    req.image = (unsigned long)child_img;
    req.image_len = CHILD_IMG_LEN;
    req.argv = (unsigned long)av;
    req.argc = 2;
    req.flags = 0;
    req.fdmap = (unsigned long)map;
    req.fdmap_n = 1;
    return skj_spawnv(&req);
}

int
main(void)
{
    int pfd[2], gr = -1, gw = -1;
    unsigned hw, hr;

    if (pipe(pfd) != 0) {
        emit("parent: pipe failed\n");
        return 2;
    }
    if (fs_fd_backing(pfd[0], &gr) != 0 || fs_fd_backing(pfd[1], &gw) != 0) {
        emit("parent: fd backing failed\n");
        return 3;
    }

    hw = spawn_with("w", (unsigned)gw, 1);   /* writer: fd 1 -> pipe write */
    hr = spawn_with("r", (unsigned)gr, 0);   /* reader: fd 0 -> pipe read  */
    if (hw == 0 || hr == 0) {
        emit("parent: spawn failed\n");
        return 4;
    }

    /* Drop our own ends so the reader sees EOF once the writer exits. */
    close(pfd[0]);
    close(pfd[1]);

    skj_wait(hw);
    skj_wait(hr);
    emit("parent done\n");
    return 0;
}
