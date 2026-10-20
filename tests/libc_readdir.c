/* libc opendir/readdir/closedir self-check: exit 42 on success.
   Make a directory with three known files, read it back, and confirm the three
   names appear exactly once with no extras (the "." and ".." entries skipped).
   Exercises getdents64 and the DIR buffer walk.  The working directory is
   tagged by target arch, since qemu-user passes no argv[0] and a parallel
   check-libc must not collide. */
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#if defined(__skj_mips__)
#define ARCHTAG "mips"
#elif defined(__skj_riscv__)
#define ARCHTAG "rv"
#elif defined(__skj_m68k__)
#define ARCHTAG "cf"
#elif defined(__skj_arm64__)
#define ARCHTAG "arm64"
#elif defined(__skj_x86_64__)
#define ARCHTAG "x86_64"
#else
#define ARCHTAG "host"
#endif

#define DIR_PATH "libc_readdir_" ARCHTAG ".d"

static int
touch(const char *path)
{
    FILE *f = fopen(path, "w");

    if (!f)
        return -1;
    fclose(f);
    return 0;
}

int
main(void)
{
    char x[300], y[300], z[300];
    int sx = 0, sy = 0, sz = 0, extra = 0;
    struct dirent *e;
    DIR *dir;

    snprintf(x, sizeof(x), "%s/x", DIR_PATH);
    snprintf(y, sizeof(y), "%s/y", DIR_PATH);
    snprintf(z, sizeof(z), "%s/z", DIR_PATH);

    /* Clean any residue from an earlier run. */
    remove(x);
    remove(y);
    remove(z);
    rmdir(DIR_PATH);

    if (mkdir(DIR_PATH, 0777) != 0)
        return 1;
    if (touch(x) != 0 || touch(y) != 0 || touch(z) != 0)
        return 2;

    dir = opendir(DIR_PATH);
    if (!dir)
        return 3;
    while ((e = readdir(dir)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (strcmp(e->d_name, "x") == 0)
            sx++;
        else if (strcmp(e->d_name, "y") == 0)
            sy++;
        else if (strcmp(e->d_name, "z") == 0)
            sz++;
        else
            extra++;
    }
    closedir(dir);

    remove(x);
    remove(y);
    remove(z);
    rmdir(DIR_PATH);

    if (sx == 1 && sy == 1 && sz == 1 && extra == 0)
        return 42;
    return 4;
}
