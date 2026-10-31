/* spawn-argv demo: the root process.
 *
 * It spawns a child from an image embedded in its own data, passing an argv
 * vector through the size-prefixed SKJ_SYS_SPAWNV request.  The host marshals
 * the image and the argument strings out of this process's memory into the new
 * child; the child echoes them back.  This exercises argv passing end to end.
 */

extern int write(int fd, const char *buf, int n);
extern unsigned skj_spawnv(const void *req);

#include "child_image.h"        /* child_img[], CHILD_IMG_LEN */

/* The spawn request, matching the field offsets in emu/sched.h.  All words are
 * guest addresses / counts; this is ILP32, so a pointer is one word. */
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
    static const char arg1[] = "hello";
    static const char *av[2];
    struct spawn_req req;
    unsigned h;

    av[0] = arg0;
    av[1] = arg1;

    req.size = sizeof req;
    req.image = (unsigned)(unsigned long)child_img;
    req.image_len = CHILD_IMG_LEN;
    req.argv = (unsigned)(unsigned long)av;
    req.argc = 2;

    emit("parent spawning\n");
    h = skj_spawnv(&req);
    if (h == 0) {
        emit("parent: spawn failed\n");
        return 2;
    }
    emit("parent spawned\n");
    return 0;
}
