/* fs/ core: the mount table, the one shared fd space, path resolution, the DOS
 * current-mount + per-mount-cwd state, and dispatch to the backends.
 *
 * Errno-style negatives here use <errno.h>; when this moves into the
 * freestanding guest build those map to the guest libc's errno codes. */

#include "fs.h"
#include "fs_internal.h"
#include "ramfs.h"
#ifdef FS_WITH_HOSTFS
#include "hostfs.h"       /* hosted/dev only; the guest has no direct host fs */
#endif
#ifdef FS_WITH_AMBIENT
#include "ambientfs.h"    /* freestanding guest: the temporary ambient host    */
#endif
#ifdef FS_WITH_HOSTCAP
#include "hostcap.h"      /* freestanding guest: a launcher-granted capability  */
#endif

#include <string.h>
#include <errno.h>

/* strchr/strrchr are not in every tier's string set (the Kobold freestanding
   runtime provides memcpy, memcmp, strlen and strcmp but not these), so fs.c
   carries its own; the rest of its string use is universal. */
static const char *
fs_strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return s;
    return 0;
}

static char *
fs_strrchr(char *s, int c)
{
    char *last = 0;
    for (; *s; s++)
        if (*s == (char)c)
            last = s;
    return last;
}

struct fs_mount fs_mounts[FS_MAX_MOUNTS];
int             fs_mount_count = 0;
int             fs_cur_mount = 0;

/* The one fd space.  A slot is a file, a directory, or a mount root; `h` is the
 * owning backend's opaque handle and `pos` is the file offset / dir cursor that
 * fs.c keeps on the backend's behalf. */
enum { FD_FREE = 0, FD_FILE, FD_DIR, FD_ROOT };
struct fd_slot {
    int      kind;
    int      mount;
    intptr_t h;
    int64_t  pos;
};
static struct fd_slot fd_tab[FS_MAX_FDS];

/* Per-mount current directory as a normalized component path with no leading
 * slash ("" is the root), kept only so fs_getcwd can report it. */
static char cwd_path[FS_MAX_MOUNTS][FS_PATH_MAX];

/* A host mount routes to one of three backends by build and by flag.  The
 * hosted tools link hostfs (direct host libc).  The freestanding guest links
 * both the confined hostcap backend (a launcher-granted capability fd, the
 * default for a host mount) and the temporary ambient backend (AT_FDCWD), which
 * a host mount selects by carrying FS_RAWPATH; fs_init installs exactly that one
 * ambient mount when no --mount was given. */
static const struct fs_backend *
backend_for(const struct fs_mount *mt)
{
    unsigned kind = mt->flags & FS_KIND_MASK;
    switch (kind) {
    case FS_KIND_RAM:
        return &ramfs_backend;
    case FS_KIND_HOST:
#if defined(FS_WITH_HOSTFS)
        return &hostfs_backend;
#else
# if defined(FS_WITH_HOSTCAP)
        if (!(mt->flags & FS_RAWPATH))
            return &hostcap_backend;
# endif
# if defined(FS_WITH_AMBIENT)
        return &ambient_backend;
# endif
        return 0;
#endif
    default:
        return 0;
    }
}

static int
fd_alloc(int kind, int mount, intptr_t h)
{
    int i;
    for (i = FS_FD_BASE; i < FS_MAX_FDS; i++) {
        if (fd_tab[i].kind == FD_FREE) {
            fd_tab[i].kind = kind;
            fd_tab[i].mount = mount;
            fd_tab[i].h = h;
            fd_tab[i].pos = 0;
            return i;
        }
    }
    return -EMFILE;
}

static struct fd_slot *
fd_get(int fd)
{
    if (fd < FS_FD_BASE || fd >= FS_MAX_FDS || fd_tab[fd].kind == FD_FREE)
        return 0;
    return &fd_tab[fd];
}

static void
pad_name(char out[FS_NAME_MAX], const char *name)
{
    int i = 0;
    for (; i < FS_NAME_MAX && name[i]; i++)
        out[i] = name[i];
    for (; i < FS_NAME_MAX; i++)
        out[i] = 0;
}

int
fs_install_mount(const char *name, enum fs_kind kind, intptr_t root_handle,
                 uint32_t flags)
{
    struct fs_mount *mt;
    int m, rfd;

    if (fs_mount_count >= FS_MAX_MOUNTS)
        return -ENOMEM;
    m = fs_mount_count;
    rfd = fd_alloc(FD_ROOT, m, root_handle);
    if (rfd < 0)
        return rfd;

    mt = &fs_mounts[m];
    memset(mt, 0, sizeof *mt);
    pad_name(mt->name, name);
    mt->fd = (int16_t)rfd;
    mt->cwd = -1;                                   /* -1 == use the root */
    mt->flags = (flags & ~(uint32_t)FS_KIND_MASK) | (kind & FS_KIND_MASK);
    cwd_path[m][0] = 0;
    fs_mount_count++;
    return m;
}

/* Split a path into (mount index, subpath after the `name:` selector) and
 * report whether the subpath is mount-absolute (a leading '/').  A ':' counts
 * as a selector only in the first segment (before any '/'). */
static int
split_path(const char *path, const char **sub)
{
    const char *colon = fs_strchr(path, ':');
    const char *slash = fs_strchr(path, '/');

    if (colon && (!slash || colon < slash)) {
        char q[FS_NAME_MAX];
        int nlen = (int)(colon - path), m;
        if (nlen > FS_NAME_MAX)
            return -EINVAL;
        pad_name(q, "");
        memcpy(q, path, nlen);
        m = -1;
        {
            int i;
            for (i = 0; i < fs_mount_count; i++)
                if (fs_mounts[i].fd != -1 &&
                    memcmp(fs_mounts[i].name, q, FS_NAME_MAX) == 0) {
                    m = i;
                    break;
                }
        }
        if (m < 0)
            return -ENOENT;
        *sub = colon + 1;
        return m;
    }
    if (fs_cur_mount < 0 || fs_cur_mount >= fs_mount_count ||
        fs_mounts[fs_cur_mount].fd == -1)
        return -ENOENT;
    *sub = path;
    return fs_cur_mount;
}

/* Resolve a path for a backend op: the mount, its backend, the base-directory
 * handle (root for an absolute subpath, else the mount's cwd), and the
 * backend-relative remainder.  *absout (optional) gets the absolute flag. */
static int
resolve_op(const char *path, struct fs_mount **mp, const struct fs_backend **bp,
           intptr_t *base, const char **rel, int *absout)
{
    const char *sub;
    struct fs_mount *mt;
    const struct fs_backend *b;
    int m, absolute, basefd;

    fs_init();                          /* idempotent: a no-op once mounted */
    m = split_path(path, &sub);
    if (m < 0)
        return m;
    mt = &fs_mounts[m];
    b = backend_for(mt);
    if (!b)
        return -ENODEV;

    if (mt->flags & FS_RAWPATH) {
        /* Verbatim path: no anchoring, no leading-'/' strip.  base is the
         * mount root handle (the backend ignores it and anchors the path
         * itself, e.g. ambient against AT_FDCWD). */
        *mp = mt;
        *bp = b;
        *base = fd_tab[mt->fd].h;
        *rel = sub;
        if (absout)
            *absout = 0;
        return 0;
    }

    absolute = (sub[0] == '/');
    basefd = absolute ? mt->fd : (mt->cwd >= 0 ? mt->cwd : mt->fd);
    if (!fd_get(basefd))
        return -EBADF;
    while (*sub == '/')
        sub++;

    *mp = mt;
    *bp = b;
    *base = fd_tab[basefd].h;
    *rel = sub;
    if (absout)
        *absout = absolute;
    return 0;
}

int
fs_open(const char *path, int flags, int mode)
{
    struct fs_mount *mt;
    const struct fs_backend *b;
    intptr_t base, h;
    const char *rel;
    int rc, fd, isdir;

    rc = resolve_op(path, &mt, &b, &base, &rel, 0);
    if (rc < 0)
        return rc;
    if ((mt->flags & FS_RO) &&
        ((flags & FS_O_ACCMODE) != FS_O_RDONLY || (flags & FS_O_CREAT)))
        return -EROFS;

    h = b->open(mt, base, rel, flags, mode);
    if (h < 0)
        return (int)h;
    isdir = (flags & FS_O_DIRECTORY) != 0;
    fd = fd_alloc(isdir ? FD_DIR : FD_FILE, (int)(mt - fs_mounts), h);
    if (fd < 0)
        b->close(mt, h, isdir);
    return fd;
}

int
fs_read(int fd, void *buf, int n)
{
    struct fd_slot *s = fd_get(fd);
    const struct fs_backend *b;
    struct fs_dirent de;
    int rc;

    if (!s)
        return -EBADF;
    b = backend_for(&fs_mounts[s->mount]);
    if (s->kind == FD_FILE)
        return b->read(&fs_mounts[s->mount], s->h, buf, n, &s->pos);

    /* A directory or mount-root fd: fs_read yields fixed fs_dirent records. */
    if (n < (int)sizeof de)
        return -EINVAL;
    rc = b->readdir(&fs_mounts[s->mount], s->h, &s->pos, &de);
    if (rc <= 0)
        return rc;                    /* 0 at end, negative on error */
    memcpy(buf, &de, sizeof de);
    return (int)sizeof de;
}

int
fs_write(int fd, const void *buf, int n)
{
    struct fd_slot *s = fd_get(fd);
    const struct fs_backend *b;
    if (!s || s->kind != FD_FILE)
        return -EBADF;
    if (fs_mounts[s->mount].flags & FS_RO)
        return -EROFS;
    b = backend_for(&fs_mounts[s->mount]);
    return b->write(&fs_mounts[s->mount], s->h, buf, n, &s->pos);
}

int64_t
fs_lseek(int fd, int64_t off, int whence)
{
    struct fd_slot *s = fd_get(fd);
    const struct fs_backend *b;
    int64_t r;
    if (!s || s->kind != FD_FILE)
        return -EBADF;
    b = backend_for(&fs_mounts[s->mount]);
    if (!b->lseek)
        return -ESPIPE;
    r = b->lseek(&fs_mounts[s->mount], s->h, off, whence, &s->pos);
    if (r >= 0)
        s->pos = r;
    return r;
}

int
fs_close(int fd)
{
    struct fd_slot *s = fd_get(fd);
    const struct fs_backend *b;
    int rc;
    if (!s)
        return -EBADF;
    b = backend_for(&fs_mounts[s->mount]);
    rc = b->close(&fs_mounts[s->mount], s->h, s->kind != FD_FILE);
    s->kind = FD_FREE;
    return rc;
}

int
fs_exists(const char *path)
{
    struct fs_mount *mt;
    const struct fs_backend *b;
    intptr_t base;
    const char *rel;
    int rc = resolve_op(path, &mt, &b, &base, &rel, 0);
    if (rc < 0)
        return rc;
    return b->exists(mt, base, rel);
}

int64_t
fs_size(const char *path)
{
    struct fs_mount *mt;
    const struct fs_backend *b;
    intptr_t base;
    const char *rel;
    int rc = resolve_op(path, &mt, &b, &base, &rel, 0);
    if (rc < 0)
        return rc;
    return b->size(mt, base, rel);
}

int
fs_isdir(const char *path)
{
    struct fs_mount *mt;
    const struct fs_backend *b;
    intptr_t base;
    const char *rel;
    int rc = resolve_op(path, &mt, &b, &base, &rel, 0);
    if (rc < 0)
        return rc;
    return b->isdir(mt, base, rel);
}

int
fs_stat(const char *path, struct fs_stat *st)
{
    struct fs_mount *mt;
    const struct fs_backend *b;
    intptr_t base;
    const char *rel;
    int rc = resolve_op(path, &mt, &b, &base, &rel, 0);
    if (rc < 0)
        return rc;
    return b->stat(mt, base, rel, st);
}

int
fs_fstat(int fd, struct fs_stat *st)
{
    struct fd_slot *s = fd_get(fd);
    const struct fs_backend *b;
    if (!s)
        return -EBADF;
    b = backend_for(&fs_mounts[s->mount]);
    return b->fstat(&fs_mounts[s->mount], s->h, s->kind != FD_FILE, st);
}

int
fs_unlink(const char *path)
{
    struct fs_mount *mt;
    const struct fs_backend *b;
    intptr_t base;
    const char *rel;
    int rc = resolve_op(path, &mt, &b, &base, &rel, 0);
    if (rc < 0)
        return rc;
    if (mt->flags & FS_RO)
        return -EROFS;
    return b->unlink(mt, base, rel);
}

int
fs_mkdir(const char *path)
{
    struct fs_mount *mt;
    const struct fs_backend *b;
    intptr_t base;
    const char *rel;
    int rc = resolve_op(path, &mt, &b, &base, &rel, 0);
    if (rc < 0)
        return rc;
    if (mt->flags & FS_RO)
        return -EROFS;
    return b->mkdir(mt, base, rel);
}

int
fs_rmdir(const char *path)
{
    struct fs_mount *mt;
    const struct fs_backend *b;
    intptr_t base;
    const char *rel;
    int rc = resolve_op(path, &mt, &b, &base, &rel, 0);
    if (rc < 0)
        return rc;
    if (mt->flags & FS_RO)
        return -EROFS;
    return b->rmdir(mt, base, rel);
}

int
fs_rename(const char *from, const char *to)
{
    struct fs_mount *m1, *m2;
    const struct fs_backend *b1, *b2;
    intptr_t base1, base2;
    const char *r1, *r2;
    int rc;

    rc = resolve_op(from, &m1, &b1, &base1, &r1, 0);
    if (rc < 0)
        return rc;
    rc = resolve_op(to, &m2, &b2, &base2, &r2, 0);
    if (rc < 0)
        return rc;
    if (m1 != m2)
        return -EXDEV;              /* each mount is its own namespace */
    if (base1 != base2)
        return -EINVAL;             /* v1: both ends share one base (root/cwd) */
    if (m1->flags & FS_RO)
        return -EROFS;
    return b1->rename(m1, base1, r1, r2);
}

int
fs_mount_set(int index)
{
    if (index < 0 || index >= fs_mount_count || fs_mounts[index].fd == -1)
        return -EINVAL;
    fs_cur_mount = index;
    return 0;
}

int
fs_mount_current(void)
{
    return fs_cur_mount;
}

int
fs_mount_find(const char *name)
{
    char q[FS_NAME_MAX];
    int i;
    pad_name(q, name);
    for (i = 0; i < fs_mount_count; i++)
        if (fs_mounts[i].fd != -1 &&
            memcmp(fs_mounts[i].name, q, FS_NAME_MAX) == 0)
            return i;
    return -1;
}

/* Fold `rel` (a backend-relative path, already selector-stripped) onto `out`,
 * which starts as the base component-path ("" for root).  Handles "." and
 * "..".  Produces a normalized component path with no leading slash. */
static void
normalize_cwd(char *out, const char *rel)
{
    char comp[FS_PATH_MAX];
    const char *p = rel;

    while (*p) {
        int n = 0;
        while (*p == '/')
            p++;
        while (*p && *p != '/' && n < (int)sizeof comp - 1)
            comp[n++] = *p++;
        comp[n] = 0;
        if (n == 0 || strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0) {
            char *slash = fs_strrchr(out, '/');
            if (slash)
                *slash = 0;
            else
                out[0] = 0;
            continue;
        }
        if (out[0]) {
            size_t len = strlen(out);
            if (len + 1 + n < FS_PATH_MAX) {
                out[len] = '/';
                memcpy(out + len + 1, comp, n + 1);
            }
        } else if (n < FS_PATH_MAX) {
            memcpy(out, comp, n + 1);
        }
    }
}

int
fs_chdir(const char *path)
{
    struct fs_mount *mt;
    const struct fs_backend *b;
    intptr_t base, h;
    const char *rel;
    int rc, absolute, m, fd;
    char newcwd[FS_PATH_MAX];

    rc = resolve_op(path, &mt, &b, &base, &rel, &absolute);
    if (rc < 0)
        return rc;
    m = (int)(mt - fs_mounts);

    h = b->open(mt, base, rel, FS_O_DIRECTORY | FS_O_RDONLY, 0);  /* dir only */
    if (h < 0)
        return (int)h;

    if (absolute)
        newcwd[0] = 0;
    else {
        size_t len = strlen(cwd_path[m]);
        memcpy(newcwd, cwd_path[m], len + 1);
    }
    normalize_cwd(newcwd, rel);

    /* Replace the old cwd fd (if any; -1 is the root sentinel). */
    if (mt->cwd >= 0) {
        struct fd_slot *old = fd_get(mt->cwd);
        if (old) {
            b->close(mt, old->h, 1);
            old->kind = FD_FREE;
        }
    }
    fd = fd_alloc(FD_DIR, m, h);
    if (fd < 0) {
        b->close(mt, h, 1);
        mt->cwd = -1;
        return fd;
    }
    mt->cwd = (int16_t)fd;
    memcpy(cwd_path[m], newcwd, strlen(newcwd) + 1);
    return 0;
}

int
fs_getcwd(int index, char *buf, int cap)
{
    const char *p;
    int n;
    if (index < 0 || index >= fs_mount_count || fs_mounts[index].fd == -1)
        return -EINVAL;
    p = cwd_path[index];
    n = (int)strlen(p);
    if (n + 2 > cap)                   /* leading '/' + NUL */
        return -ERANGE;
    buf[0] = '/';
    memcpy(buf + 1, p, n + 1);
    return n + 1;
}

void
fs_init(void)
{
    static int started;

    if (started)
        return;
    started = 1;

    if (fs_mount_count > 0) {
        /* The table is already populated: either the launcher filled it (the
         * guest --mount path) or a hosted caller installed its own mounts
         * through fs_install_mount (the fs_test harness).  Only the launcher
         * path needs adoption, and only the guest build links it, so the pass
         * is gated on FS_WITH_HOSTCAP; a hosted table is left untouched. */
#ifdef FS_WITH_HOSTCAP
        /* The launcher wrote each entry's `fd` as a sandbox capability fd: a raw
         * directory fd its kernel resolves paths against, not an fs.c fd-space
         * slot.  Adopt each into the fd space by allocating an FD_ROOT slot
         * whose backend handle is that capability fd, then repointing mt->fd at
         * the slot, so resolve_op hands the confined hostcap backend the
         * capability fd as its base.  A host mount here carries no FS_RAWPATH,
         * so backend_for routes it to the confined backend, not the ambient
         * stop-gap.  (A table that fs_install_mount built would already have its
         * root slots allocated; the guest never installs mounts before this
         * runs, so the pass only ever sees a launcher table.) */
        int ngrants = fs_mount_count, i, scratch;
        for (i = 0; i < ngrants; i++) {
            struct fs_mount *mt = &fs_mounts[i];
            int capfd, slot;
            if (mt->fd < 0)
                continue;
            capfd = mt->fd;
            slot = fd_alloc(FD_ROOT, i, (intptr_t)capfd);
            if (slot < 0)
                break;                  /* fd space full: leave the rest unusable */
            mt->fd = (int16_t)slot;
            mt->cwd = -1;               /* -1 == use the root */
            cwd_path[i][0] = 0;
        }
        /* Append the safe scratch ramfs and land on it: a --mount guest starts
         * on the empty RAM disk and reaches the host only through a named grant.
         * There is no ambient default here; host access is capability-only. */
        scratch = ramfs_mount("scratch", 0);
        if (scratch >= 0)
            fs_cur_mount = scratch;
#endif
        return;
    }

    /* No table was handed in.  Install the safe empty scratch ramfs at index 0
     * and land on it: that is the default policy, so a bare path stays in RAM
     * and the host is not reachable unless a mount grants it.
     *
     * FS_AMBIENT_DEFAULT is the documented, temporary override that restores the
     * pre-fs behaviour (a bare path reaching the host cwd through an unconfined
     * AT_FDCWD mount).  It is kept only for the test harnesses that run an
     * fs/-interpose guest under bare qemu, where no launcher can grant a
     * --mount; it needs FS_WITH_AMBIENT and goes away once those migrate to a
     * capability mount. */
    ramfs_mount("scratch", 0);         /* index 0: the safe empty scratch disk */
    fs_cur_mount = 0;
#ifdef FS_AMBIENT_DEFAULT
    {
        int h = ambient_mount("host", 0);
        if (h >= 0)
            fs_cur_mount = h;
    }
#endif
}
