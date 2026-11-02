/* spawn-backtrace parent: the root process spawns a child from an embedded
 * image and waits for it.  The child faults; skj-run prints a backtrace for it
 * ("[proc 2]"), which this test checks is DWARF-symbolized rather than raw
 * addresses.  The parent itself exits cleanly after reading the child status. */

extern int write(int fd, const char *buf, int n);
extern unsigned skj_spawnv(const void *req);
extern unsigned skj_wait(unsigned handle);

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
    unsigned h;

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
    skj_wait(h);
    emit("parent done\n");
    return 0;
}
