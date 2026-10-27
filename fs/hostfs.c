/* hostfs: a real host directory tree, confined to the granted root.  A proper
 * multi-instance driver: each mount pins its own root directory fd, and several
 * hostfs mounts over different host directories coexist.
 *
 * A development/admin backend.  Access to a host tree is a privilege the
 * launcher grants with an explicit --mount; it is never ambient, and a guest
 * with no hostfs mount cannot reach the host at all.
 *
 * Handles: a regular file is a host fd; a directory (mount root, cwd, or an
 * opened directory) is an index into the open-directory table below, which
 * holds the dirfd plus a getdents64 buffer.  That keeps the driver to the
 * shared POSIX syscall seam (openat / getdents64 / *at), with no libc DIR* /
 * fdopendir, so the one source builds on the host tools and in the guest alike.
 *
 * Paths arrive relative to the base with the `name:` selector and any leading
 * '/' already stripped, so a path can only name something at or below the base.
 * The remaining escape vector, a ".." component, is rejected here.
 *
 * Confinement gap (v1): a symlink inside the tree that points outside it still
 * resolves through the host.  Closing it needs openat2(RESOLVE_BENEATH); that
 * is the planned hardening and is tracked, not done here. */

#define _GNU_SOURCE
#include "hostfs.h"
#include "fs_internal.h"

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>                    /* renameat */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>             /* SYS_getdents64 */

#define HOSTFS_MAXDIRS 32
#define HDIR_BUF       4096

/* getdents64 record and the two d_type values we care about, declared here so
 * the driver needs no <dirent.h>. */
struct ldirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};
#define HDT_DIR 4
#define HDT_REG 8

struct hdir {
    int  used;
    int  fd;                          /* the directory's host fd */
    int  pos;                         /* next unread byte in buf */
    int  len;                         /* valid bytes in buf */
    char buf[HDIR_BUF];
};

static struct hdir hdirs[HOSTFS_MAXDIRS];

static int
hdir_alloc(int fd)
{
    int i;
    for (i = 0; i < HOSTFS_MAXDIRS; i++)
        if (!hdirs[i].used) {
            hdirs[i].used = 1;
            hdirs[i].fd = fd;
            hdirs[i].pos = hdirs[i].len = 0;
            return i;
        }
    return -1;
}

static long
host_getdents(int fd, void *buf, int n)
{
    return syscall(SYS_getdents64, fd, buf, (unsigned int)n);
}

static int
has_dotdot(const char *rel)
{
    const char *p = rel;
    while (*p) {
        const char *s = p;
        int len;
        while (*p && *p != '/')
            p++;
        len = (int)(p - s);
        if (len == 2 && s[0] == '.' && s[1] == '.')
            return 1;
        while (*p == '/')
            p++;
    }
    return 0;
}

static int
to_host_oflags(int f)
{
    int o;
    switch (f & FS_O_ACCMODE) {
    case FS_O_WRONLY: o = O_WRONLY; break;
    case FS_O_RDWR:   o = O_RDWR;   break;
    default:          o = O_RDONLY; break;
    }
    if (f & FS_O_CREAT)
        o |= O_CREAT;
    if (f & FS_O_TRUNC)
        o |= O_TRUNC;
    if (f & FS_O_APPEND)
        o |= O_APPEND;
    return o;
}

/* A base handle is an hdir index; its fd is what openat resolves against. */
static int
basefd(intptr_t base)
{
    return hdirs[base].fd;
}

static const char *
hp(const char *rel)
{
    return rel[0] ? rel : ".";
}

static intptr_t
host_open(struct fs_mount *m, intptr_t base, const char *rel, int flags, int mode)
{
    int fd, idx;
    (void)m;
    if (has_dotdot(rel))
        return -EACCES;
    if (flags & FS_O_DIRECTORY) {
        fd = openat(basefd(base), hp(rel), O_RDONLY | O_DIRECTORY);
        if (fd < 0)
            return -errno;
        idx = hdir_alloc(fd);
        if (idx < 0) {
            close(fd);
            return -EMFILE;
        }
        return idx;                   /* a directory handle is an hdir index */
    }
    fd = openat(basefd(base), hp(rel), to_host_oflags(flags), mode);
    return fd < 0 ? -errno : fd;
}

static int
host_read(struct fs_mount *m, intptr_t h, void *buf, int n, int64_t *pos)
{
    ssize_t r;
    (void)m;
    (void)pos;                        /* the host fd carries its own offset */
    r = read((int)h, buf, n);
    return r < 0 ? -errno : (int)r;
}

static int
host_write(struct fs_mount *m, intptr_t h, const void *buf, int n, int64_t *pos)
{
    ssize_t r;
    (void)m;
    (void)pos;
    r = write((int)h, buf, n);
    return r < 0 ? -errno : (int)r;
}

static int64_t
host_lseek(struct fs_mount *m, intptr_t h, int64_t off, int whence, int64_t *pos)
{
    off_t r = lseek((int)h, (off_t)off, whence);
    (void)m;
    if (r < 0)
        return -errno;
    *pos = (int64_t)r;
    return (int64_t)r;
}

static void
host_fill_stat(const struct stat *hs, struct fs_stat *st)
{
    st->dev = (uint64_t)hs->st_dev;
    st->ino = (uint64_t)hs->st_ino;
    st->size = (int64_t)hs->st_size;
    st->mode = (uint32_t)hs->st_mode;
    st->nlink = (uint32_t)hs->st_nlink;
}

static int
host_stat(struct fs_mount *m, intptr_t base, const char *rel, struct fs_stat *st)
{
    struct stat hs;
    (void)m;
    if (has_dotdot(rel))
        return -EACCES;
    if (fstatat(basefd(base), hp(rel), &hs, 0) < 0)
        return -errno;
    host_fill_stat(&hs, st);
    return 0;
}

static int
host_fstat(struct fs_mount *m, intptr_t h, int is_dir, struct fs_stat *st)
{
    struct stat hs;
    int fd = is_dir ? hdirs[h].fd : (int)h;
    (void)m;
    if (fstat(fd, &hs) < 0)
        return -errno;
    host_fill_stat(&hs, st);
    return 0;
}

static int
host_readdir(struct fs_mount *m, intptr_t h, int64_t *cursor, struct fs_dirent *de)
{
    struct hdir *d = &hdirs[h];
    (void)m;
    (void)cursor;                     /* the hdir buffer carries the cursor */
    for (;;) {
        struct ldirent64 *e;
        if (d->pos >= d->len) {
            long n = host_getdents(d->fd, d->buf, HDIR_BUF);
            if (n < 0)
                return -errno;
            if (n == 0)
                return 0;
            d->len = (int)n;
            d->pos = 0;
        }
        e = (struct ldirent64 *)(d->buf + d->pos);
        d->pos += e->d_reclen;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        de->ino = (uint32_t)e->d_ino;
        de->type = (e->d_type == HDT_DIR) ? FS_DT_DIR
                 : (e->d_type == HDT_REG) ? FS_DT_REG
                 : FS_DT_UNKNOWN;
        de->pad[0] = de->pad[1] = de->pad[2] = 0;
        strncpy(de->name, e->d_name, FS_DIRENT_NAMEMAX - 1);
        de->name[FS_DIRENT_NAMEMAX - 1] = 0;
        return 1;
    }
}

static int
host_close(struct fs_mount *m, intptr_t h, int is_dir)
{
    (void)m;
    if (is_dir) {
        int fd = hdirs[h].fd;
        hdirs[h].used = 0;
        return close(fd) < 0 ? -errno : 0;
    }
    return close((int)h) < 0 ? -errno : 0;
}

static int
host_exists(struct fs_mount *m, intptr_t base, const char *rel)
{
    struct stat st;
    (void)m;
    if (has_dotdot(rel))
        return -EACCES;
    return fstatat(basefd(base), hp(rel), &st, 0) == 0 ? 1 : 0;
}

static int64_t
host_size(struct fs_mount *m, intptr_t base, const char *rel)
{
    struct stat st;
    (void)m;
    if (has_dotdot(rel))
        return -EACCES;
    if (fstatat(basefd(base), hp(rel), &st, 0) < 0)
        return -errno;
    return (int64_t)st.st_size;
}

static int
host_isdir(struct fs_mount *m, intptr_t base, const char *rel)
{
    struct stat st;
    (void)m;
    if (has_dotdot(rel))
        return -EACCES;
    if (fstatat(basefd(base), hp(rel), &st, 0) < 0)
        return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

static int
host_unlink(struct fs_mount *m, intptr_t base, const char *rel)
{
    (void)m;
    if (has_dotdot(rel))
        return -EACCES;
    return unlinkat(basefd(base), hp(rel), 0) < 0 ? -errno : 0;
}

static int
host_mkdir(struct fs_mount *m, intptr_t base, const char *rel)
{
    (void)m;
    if (has_dotdot(rel))
        return -EACCES;
    return mkdirat(basefd(base), hp(rel), 0777) < 0 ? -errno : 0;
}

static int
host_rmdir(struct fs_mount *m, intptr_t base, const char *rel)
{
    (void)m;
    if (has_dotdot(rel))
        return -EACCES;
    return unlinkat(basefd(base), hp(rel), AT_REMOVEDIR) < 0 ? -errno : 0;
}

static int
host_rename(struct fs_mount *m, intptr_t base, const char *from, const char *to)
{
    int bfd = basefd(base);
    (void)m;
    if (has_dotdot(from) || has_dotdot(to))
        return -EACCES;
    return renameat(bfd, hp(from), bfd, hp(to)) < 0 ? -errno : 0;
}

const struct fs_backend hostfs_backend = {
    FS_KIND_HOST, "hostfs",
    host_open, host_read, host_write, host_lseek, host_readdir, host_close,
    host_exists, host_size, host_isdir, host_stat, host_fstat,
    host_unlink, host_rename, host_mkdir, host_rmdir,
};

int
hostfs_mount(const char *name, const char *hostdir, uint32_t flags)
{
    int fd = open(hostdir, O_RDONLY | O_DIRECTORY);
    int idx;
    if (fd < 0)
        return -errno;
    idx = hdir_alloc(fd);
    if (idx < 0) {
        close(fd);
        return -EMFILE;
    }
    return fs_install_mount(name, FS_KIND_HOST, (intptr_t)idx, flags);
}
