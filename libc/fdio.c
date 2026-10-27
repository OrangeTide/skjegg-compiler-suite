/* fdio.c: the fd primitives that route a path and an fd through the shared fs/
   layer.  Built only when the fs/ interposition is selected (SKJ_FS_INTERPOSE,
   the freestanding VM tiers rv and cf); otherwise this file is empty and the
   raw crt wrappers (runtime/syscall_*.S) stand.

   The crt wrappers are weak, so these strong definitions override open / read /
   write / lseek / close for a program linked against this libc.  Descriptors
   0, 1 and 2 are the console and are not fs-managed, so they go straight to the
   raw __sys_* seam; every other fd comes from fs_open and is an fs fd.  open()
   installs the default mount policy (fs_init) on first use. */

#ifdef SKJ_FS_INTERPOSE

#include "fs.h"
#include <fcntl.h>

/* The raw seam the console path and fs/ itself use. */
int  __sys_read(int fd, void *buf, int n);
int  __sys_write(int fd, const void *buf, int n);
int  __sys_close(int fd);
long __sys_lseek(int fd, long off, int whence);

static int fs_ready;

static void
ensure_fs(void)
{
    if (!fs_ready) {
        fs_ready = 1;
        fs_init();
    }
}

static int
to_fsflags(int f)
{
    int r;
    switch (f & 3) {                    /* access mode: O_RDONLY/WRONLY/RDWR */
    case O_WRONLY: r = FS_O_WRONLY; break;
    case O_RDWR:   r = FS_O_RDWR;   break;
    default:       r = FS_O_RDONLY; break;
    }
    if (f & O_CREAT)
        r |= FS_O_CREAT;
    if (f & O_TRUNC)
        r |= FS_O_TRUNC;
    if (f & O_APPEND)
        r |= FS_O_APPEND;
    return r;
}

int
open(const char *path, int flags, int mode)
{
    ensure_fs();
    return fs_open(path, to_fsflags(flags), mode);
}

int
read(int fd, void *buf, int n)
{
    return fd < FS_FD_BASE ? __sys_read(fd, buf, n) : fs_read(fd, buf, n);
}

int
write(int fd, const void *buf, int n)
{
    return fd < FS_FD_BASE ? __sys_write(fd, buf, n) : fs_write(fd, buf, n);
}

long
lseek(int fd, long off, int whence)
{
    return fd < FS_FD_BASE ? __sys_lseek(fd, off, whence)
                           : (long)fs_lseek(fd, off, whence);
}

int
close(int fd)
{
    return fd < FS_FD_BASE ? __sys_close(fd) : fs_close(fd);
}

#endif /* SKJ_FS_INTERPOSE */
