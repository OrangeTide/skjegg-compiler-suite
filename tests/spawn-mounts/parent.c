/* spawn-mounts demo: the root process.
 *
 * It is run under skj-run with `--mount data:<dir>`, so the launcher grants it a
 * "data" capability mount.  It spawns a child with SKJ_SPAWN_FLAG_SHARE_MOUNTS,
 * which hands that mount to the child, then waits and reports the child's exit
 * status.  The child reads a seeded file through the inherited mount and echoes
 * it, so the host sees the file content followed by "parent: child status=0".
 *
 * The parent links the fs/ libc only so the root image exports the fs_mounts
 * symbol the launcher fills; it does not itself touch the filesystem.
 */

#include <unistd.h>
#include <string.h>

extern unsigned skj_spawnv(const void *req);
extern unsigned skj_wait(unsigned handle);

#include "child_image.h"        /* child_img[], CHILD_IMG_LEN */

/* The spawn request through SKJ_SPAWN_WITH_FDS: image + argv + flags + fd map. */
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

#define SKJ_SPAWN_FLAG_SHARE_MOUNTS 0x1u

static void
emit(const char *s)
{
    write(1, s, (int)strlen(s));
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
    req.flags = SKJ_SPAWN_FLAG_SHARE_MOUNTS;
    req.fdmap = 0;
    req.fdmap_n = 0;

    emit("parent spawning\n");
    h = skj_spawnv(&req);
    if (h == 0) {
        emit("parent: spawn failed\n");
        return 2;
    }

    st = skj_wait(h);
    emit("parent: child status=");
    d[0] = (char)('0' + (st % 10));
    d[1] = 0;
    emit(d);
    emit("\n");
    return 0;
}
