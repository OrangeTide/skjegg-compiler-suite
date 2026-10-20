/* libc mkdir/rmdir/unlink/rename/remove self-check: exit 42 on success.
   Exercises the path file operations end to end: make a directory, create a
   file in it, rename it, stat it by both names, remove it, and remove the
   directory, checking stat() agrees at each step.  Every call routes through
   the crt's *at syscall wrappers (mkdirat/unlinkat/renameat2).

   The working directory name is tagged with the target arch (the __skj_*__
   predefines), so the five arches' binaries use distinct paths and a parallel
   `make check-libc` cannot collide.  qemu-user does not pass argv[0], so the
   name cannot be derived from the binary path. */
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

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

#define DIR "libc_pathops_" ARCHTAG ".d"
#define FA  DIR "/a"
#define FB  DIR "/b"

int
main(void)
{
    struct stat st;
    FILE *f;

    /* Leave no residue from an earlier run (errors ignored). */
    remove(FA);
    remove(FB);
    rmdir(DIR);

    if (mkdir(DIR, 0777) != 0)
        return 1;
    if (stat(DIR, &st) != 0 || !S_ISDIR(st.st_mode))
        return 2;

    f = fopen(FA, "w");
    if (!f)
        return 3;
    fputs("hi\n", f);          /* 3 bytes */
    fclose(f);

    if (stat(FA, &st) != 0)
        return 4;
    if (!S_ISREG(st.st_mode) || st.st_size != 3)
        return 5;

    if (rename(FA, FB) != 0)
        return 6;
    if (stat(FA, &st) == 0)    /* the old name must be gone */
        return 7;
    if (stat(FB, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 3)
        return 8;

    if (remove(FB) != 0)
        return 9;
    if (stat(FB, &st) == 0)
        return 10;

    if (rmdir(DIR) != 0)
        return 11;
    if (stat(DIR, &st) == 0)
        return 12;

    return 42;
}
