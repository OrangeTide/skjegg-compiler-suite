/* spawn-kill demo: the root process.
 *
 * It spawns a child, immediately kills it with a chosen status, then waits and
 * reads that status back.  All three calls run in the one slice before the
 * scheduler gives the child a turn, so the child never runs: this exercises
 * SKJ_SYS_KILL setting a wait status and the kill/wait/reap flow.
 */

extern int write(int fd, const char *buf, int n);
extern unsigned skj_spawnv(const void *req);
extern unsigned skj_wait(unsigned handle);
extern unsigned skj_kill(unsigned handle, unsigned status);

#include "child_image.h"        /* child_img[], CHILD_IMG_LEN */

struct spawn_req {
    unsigned size;
    unsigned image;
    unsigned image_len;
    unsigned argv;
    unsigned argc;
};

static void
emit(const char *s)
{
    int n = 0;

    while (s[n])
        n++;
    write(1, s, n);
}

int
main(void)
{
    static const char arg0[] = "child";
    static const char *av[1];
    struct spawn_req req;
    unsigned h, st;
    char d[2];

    av[0] = arg0;
    req.size = sizeof req;
    req.image = (unsigned)(unsigned long)child_img;
    req.image_len = CHILD_IMG_LEN;
    req.argv = (unsigned)(unsigned long)av;
    req.argc = 1;

    emit("parent spawning\n");
    h = skj_spawnv(&req);
    if (h == 0) {
        emit("parent: spawn failed\n");
        return 2;
    }

    skj_kill(h, 9);
    st = skj_wait(h);
    emit("parent killed status=");
    d[0] = (char)('0' + (st % 10));
    d[1] = 0;
    emit(d);
    emit("\n");
    return 0;
}
