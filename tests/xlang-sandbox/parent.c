/* The root process of the cross-language sandbox demo.
 *
 * Compiled by skj-cc-rv. It spawns two children from images embedded in its own
 * data: one built from C (cchild.c) and one from Kobold-Pascal (kchild.pas).
 * The host reads each image out with gm_read_out and loads it with the
 * in-memory ELF loader; each child then runs as its own process with its own
 * memory, round-robin scheduled by skj-run under the shared budget. To the
 * sandbox the two children are indistinguishable: both are RV32 ELFs the
 * toolkit produced from the shared IR, so a C process and a Pascal process
 * coexist with nothing language-specific in the runtime.
 */

extern int write(int fd, const char *buf, int n);
extern unsigned sys_spawn(const void *img, unsigned len);

#include "images.h"     /* cchild_img[]/CCHILD_LEN, kchild_img[]/KCHILD_LEN */

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
    emit("parent[C]: running\n");

    if (!sys_spawn(cchild_img, CCHILD_LEN)) {
        emit("parent[C]: C child spawn failed\n");
        return 2;
    }
    emit("parent[C]: spawned C child\n");

    if (!sys_spawn(kchild_img, KCHILD_LEN)) {
        emit("parent[C]: Kobold child spawn failed\n");
        return 3;
    }
    emit("parent[C]: spawned Kobold child\n");

    emit("parent[C]: done\n");
    return 0;
}
