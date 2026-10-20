/* libc stat/fstat self-check: exit 42 on success.
   stat() a path known to be a directory, then open it and fstat the
   descriptor, checking both report the same directory inode.  Both calls go
   through statx under the hood (the newer syscall arches have no bare
   stat/fstat), so this exercises the per-arch crt statx wrapper and
   libc/stat.c's struct-statx translation.  (Named libc_statx so its object
   does not collide with libc/stat.c's own libc_stat.o in the shared build
   directory.) */
#include <sys/stat.h>
#include <fcntl.h>

int
main(void)
{
    struct stat sp, sf;
    int fd;

    if (stat("/", &sp) != 0)
        return 1;
    if (!S_ISDIR(sp.st_mode))
        return 2;

    fd = open("/", O_RDONLY, 0);
    if (fd < 0)
        return 3;
    if (fstat(fd, &sf) != 0)
        return 4;
    if (!S_ISDIR(sf.st_mode))
        return 5;

    /* The same object by two routes: same device and inode. */
    if (sp.st_dev != sf.st_dev || sp.st_ino != sf.st_ino)
        return 6;

    return 42;
}
