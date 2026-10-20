/* statio.c : exercise the guest libc's stat/fstat (served by statx on rv32).
   argv[1] is a file path.  Prints its size and type through stat(path) and
   through fstat(open fd), so a run under skj-run and one under qemu-riscv32 can
   be compared.  Returns 0, or 2 on any error. */

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>

int close(int fd);

int
main(int argc, char **argv)
{
    struct stat st;
    int fd;

    if (argc < 2) {
        printf("usage: statio path\n");
        return 2;
    }

    if (stat(argv[1], &st) != 0) {
        printf("stat failed\n");
        return 2;
    }
    printf("stat size=%ld reg=%d dir=%d\n",
           (long)st.st_size, S_ISREG(st.st_mode), S_ISDIR(st.st_mode));

    fd = open(argv[1], 0, 0);           /* O_RDONLY */
    if (fd < 0) {
        printf("open failed\n");
        return 2;
    }
    if (fstat(fd, &st) != 0) {
        printf("fstat failed\n");
        return 2;
    }
    printf("fstat size=%ld reg=%d\n", (long)st.st_size, S_ISREG(st.st_mode));
    close(fd);
    return 0;
}
