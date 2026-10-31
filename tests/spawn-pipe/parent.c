/* spawn-pipe demo: the root wires a pipe between two children.
 *
 * It creates a pipe, spawns a writer with the pipe's write end as its fd 1 and
 * a reader with the pipe's read end as its fd 0 (through the spawn fd map),
 * then drops its own ends so the reader sees EOF once the writer exits.  The
 * reader echoes the piped message to the terminal; the host sees it followed by
 * "parent done".  No fork/dup2: the descriptors are placed at spawn.
 */

#include <unistd.h>
#include <string.h>

extern unsigned skj_spawnv(const void *req);
extern unsigned skj_wait(unsigned handle);
int pipe(int fd[2]);
int fs_fd_backing(int fd, int *gfd);

#include "child_image.h"        /* child_img[], CHILD_IMG_LEN */

/* One fd-map entry: the capability index backing a parent fd, and the id the
 * child should see it as.  Matches the host's {gfd, dst} wire layout. */
struct fdent {
    unsigned gfd;
    unsigned dst;
};

struct spawn_req {
    unsigned size;
    unsigned image;
    unsigned image_len;
    unsigned argv;
    unsigned argc;
    unsigned flags;
    unsigned fdmap;
    unsigned fdmap_n;
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
    req.image = (unsigned)(unsigned long)child_img;
    req.image_len = CHILD_IMG_LEN;
    req.argv = (unsigned)(unsigned long)av;
    req.argc = 2;
    req.flags = 0;
    req.fdmap = (unsigned)(unsigned long)map;
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

    /* Drop the parent's own ends so the only write end left is the writer's;
     * when it exits the reader sees EOF. */
    close(pfd[0]);
    close(pfd[1]);

    skj_wait(hw);
    skj_wait(hr);
    emit("parent done\n");
    return 0;
}
