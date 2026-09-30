/* sandbox demo: the root process.
 *
 * It spawns a child from an image embedded in its own data (the host reads
 * those bytes out with gm_read_out and loads them with the in-memory ELF
 * loader), then exercises a guarded mapping and its release. The child runs
 * on its own memory under the shared budget; this process is unaffected by
 * what happens to it.
 */

extern int write(int fd, const char *buf, int n);
extern void *sys_map(unsigned size, unsigned flags);
extern void sys_munmap(void *base, unsigned size);
extern unsigned sys_spawn(const void *img, unsigned len);

#include "child_image.h"        /* child_img[], CHILD_IMG_LEN */

#define GM_MAP_GUARD_LO     1u

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
    unsigned h;
    volatile unsigned char *base;

    emit("parent running\n");

    /* A spawn whose image range is not mapped must fail cleanly and leave this
     * process runnable, not fault it. */
    if (sys_spawn((const void *)0xf0000000u, 4096u) != 0) {
        emit("bad spawn unexpectedly succeeded\n");
        return 6;
    }

    h = sys_spawn(child_img, CHILD_IMG_LEN);
    if (!h) {
        emit("spawn failed\n");
        return 3;
    }

    base = sys_map(4096, GM_MAP_GUARD_LO);
    if (!base) {
        emit("parent map failed\n");
        return 5;
    }
    base[0] = 0x42;             /* the mapping is usable */
    sys_munmap((void *)base, 4096);

    emit("parent done\n");
    return 0;
}
