/* Directory and path file operations: mkdir, rmdir, unlink, rename, remove.
   Each wraps one of the crt's *at syscall wrappers (runtime/start_*.S) with
   AT_FDCWD, the same way libc/stat.c wraps statx.  The *at forms are the only
   ones the no-bare-syscall arches (rv32, arm64) have, so one implementation
   serves every target. */
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int
mkdir(const char *path, int mode)
{
    return mkdirat(AT_FDCWD, path, mode);
}

int
unlink(const char *path)
{
    return unlinkat(AT_FDCWD, path, 0);
}

int
rmdir(const char *path)
{
    return unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}

int
chmod(const char *path, int mode)
{
    return fchmodat(AT_FDCWD, path, mode, 0);
}

int
rename(const char *oldpath, const char *newpath)
{
    return renameat2(AT_FDCWD, oldpath, AT_FDCWD, newpath, 0);
}

/* ISO C remove(): a file or an empty directory.  Try the file form first, then
   the directory form, so a caller need not know which it is removing. */
int
remove(const char *path)
{
    if (unlinkat(AT_FDCWD, path, 0) == 0)
        return 0;
    return unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}
