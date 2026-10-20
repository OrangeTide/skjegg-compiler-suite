/* pathops.c : exercise the guest libc's mkdir/rename/remove/rmdir (served by
   the *at syscalls through skj-run's GSYS_MKDIRAT/UNLINKAT/RENAMEAT2).  argv[1]
   is an existing scratch directory.  The sequence and its printed results are
   compared between a run under skj-run and one under qemu, so the emulator's
   handlers are checked against the real kernel.  Returns 0, or 2 on a usage
   error. */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
    char dir[256], fa[300], fb[300];
    struct stat st;
    FILE *f;

    if (argc < 2) {
        printf("usage: pathops scratchdir\n");
        return 2;
    }

    snprintf(dir, sizeof(dir), "%s/pod", argv[1]);
    snprintf(fa, sizeof(fa), "%s/a", dir);
    snprintf(fb, sizeof(fb), "%s/b", dir);

    remove(fa);
    remove(fb);
    rmdir(dir);

    printf("mkdir=%d\n", mkdir(dir, 0777));

    f = fopen(fa, "w");
    printf("create=%d\n", f ? 0 : -1);
    if (f) {
        fputs("hi\n", f);
        fclose(f);
    }

    printf("rename=%d\n", rename(fa, fb));
    printf("statold=%d\n", stat(fa, &st) == 0 ? 1 : 0);   /* 0: gone, as wanted */
    printf("statnew size=%ld\n", stat(fb, &st) == 0 ? (long)st.st_size : -1L);
    printf("remove=%d\n", remove(fb));
    printf("rmdir=%d\n", rmdir(dir));
    printf("statdir=%d\n", stat(dir, &st) == 0 ? 1 : 0);  /* 0: gone, as wanted */
    return 0;
}
