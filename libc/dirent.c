/* opendir / readdir / closedir over the getdents64 syscall (the crt wrapper in
   runtime/syscall_rv.S, syscall_cf.S, and the start_*.S for the other arches).
   A DIR holds one getdents64 buffer; readdir walks it entry by entry, refilling
   from the kernel when it runs out, and returns a pointer into the buffer. */
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define DIRBUF 4096

/* buf is first so it inherits the allocation's alignment; the kernel packs
   entries at an 8-byte stride, so every entry lands aligned. */
struct __dir {
    char buf[DIRBUF];
    int  fd;
    int  pos;                   /* next unread byte in buf */
    int  len;                   /* valid bytes in buf */
};

DIR *
opendir(const char *path)
{
    DIR *dir;
    int fd = open(path, O_RDONLY, 0);

    if (fd < 0)
        return 0;
    dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        close(fd);
        return 0;
    }
    dir->fd = fd;
    dir->pos = 0;
    dir->len = 0;
    return dir;
}

struct dirent *
readdir(DIR *dir)
{
    struct dirent *de;

    if (!dir)
        return 0;
    if (dir->pos >= dir->len) {
        int n = getdents64(dir->fd, dir->buf, DIRBUF);
        if (n <= 0)              /* 0 at end of directory, <0 on error */
            return 0;
        dir->len = n;
        dir->pos = 0;
    }
    de = (struct dirent *)(dir->buf + dir->pos);
    dir->pos += de->d_reclen;
    return de;
}

int
closedir(DIR *dir)
{
    int r;

    if (!dir)
        return -1;
    r = close(dir->fd);
    free(dir);
    return r;
}
