/* wasi_file.c : the WASM file/directory/stat syscall shim.

   On the native targets the libc's low-level file calls (open, close, lseek,
   getdents64, statx, and the *at path operations) are the crt's raw syscall
   wrappers in runtime/start_*.S.  WASM has no such crt, so this file provides
   them for the skj-cc-wasm target by calling WASI preview1 directly.  Every
   __wasi_<name> extern below imports wasi_snapshot_preview1.<name> (the
   backend's naming convention; skj-ld-wasm emits the real import).

   WASI is capability based: a path opens against a preopened directory fd, not
   a global namespace, so this file enumerates the preopens the host granted
   (wasmtime run --dir ...) once and resolves each relative path against the
   matching one, the way wasi-libc does.  It also translates the two record
   layouts the kernel syscalls use but WASI does not: struct statx (from a WASI
   filestat) and struct linux_dirent64 (from a WASI directory read).

   read() and write() stay the linker's fd_write/fd_read seam; this file adds
   only the pieces beyond plain stream I/O.  Coroutine and thread support is a
   separate matter: WASM MVP cannot switch stacks, so libc/coro.c and
   libc/threads.c are not linkable here (they need Asyncify, not a seam). */

#include <stdint.h>
#include <string.h>

/* ---- WASI preview1 imports (wasi_snapshot_preview1.<field>) ---- */

/* oflags, fdflags, and lookupflags are u16/u32 in WASI but travel as i32 in the
   call ABI; rights are u64 (i64). */
extern int __wasi_path_open(int dirfd, int dirflags, const char *path, int path_len,
                            int oflags, unsigned long long rights_base,
                            unsigned long long rights_inheriting, int fdflags,
                            int *opened_fd);
extern int __wasi_fd_close(int fd);
extern int __wasi_fd_seek(int fd, long long offset, int whence, unsigned long long *newoff);
extern int __wasi_fd_readdir(int fd, void *buf, int buf_len,
                             unsigned long long cookie, int *bufused);
extern int __wasi_fd_filestat_get(int fd, void *filestat);
extern int __wasi_path_filestat_get(int fd, int flags, const char *path, int path_len,
                                    void *filestat);
extern int __wasi_path_create_directory(int fd, const char *path, int path_len);
extern int __wasi_path_unlink_file(int fd, const char *path, int path_len);
extern int __wasi_path_remove_directory(int fd, const char *path, int path_len);
extern int __wasi_path_rename(int fd, const char *old_path, int old_len,
                              int new_fd, const char *new_path, int new_len);
extern int __wasi_fd_prestat_get(int fd, void *prestat);
extern int __wasi_fd_prestat_dir_name(int fd, char *path, int path_len);

/* ---- constants shared with the libc headers ---- */

#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_CREAT      0100       /* 0x40, matches include/fcntl.h (non-MIPS) */
#define O_TRUNC      01000      /* 0x200 */
#define O_APPEND     02000      /* 0x400 */

#define AT_REMOVEDIR 0x200
#define AT_EMPTY_PATH 0x1000

/* WASI oflags */
#define WASI_O_CREAT     (1 << 0)
#define WASI_O_DIRECTORY (1 << 1)
#define WASI_O_EXCL      (1 << 2)
#define WASI_O_TRUNC     (1 << 3)
/* WASI fdflags */
#define WASI_FD_APPEND   (1 << 0)
/* WASI lookupflags: follow symlinks on the final component */
#define WASI_LOOKUP_SYMLINK_FOLLOW (1 << 0)

/* Rights are capability bits on an fd.  A directory fd and a regular-file fd
   accept different sets: requesting fd_read on a directory is ENOTCAPABLE, so
   open() asks for the file set first and falls back to the directory set.  The
   sock rights (28, 29) are left out so a request stays within a --dir preopen.
   Bit positions: fd_datasync 0, fd_read 1, fd_seek 2, fd_fdstat_set_flags 3,
   fd_sync 4, fd_tell 5, fd_write 6, fd_advise 7, fd_allocate 8,
   path_create_directory 9, path_create_file 10, path_link_source 11,
   path_link_target 12, path_open 13, fd_readdir 14, path_readlink 15,
   path_rename_source 16, path_rename_target 17, path_filestat_get 18,
   path_filestat_set_size 19, path_filestat_set_times 20, fd_filestat_get 21,
   fd_filestat_set_size 22, fd_filestat_set_times 23, path_symlink 24,
   path_remove_directory 25, path_unlink_file 26, poll_fd_readwrite 27. */
#define R_FILE ((1ULL<<0)|(1ULL<<1)|(1ULL<<2)|(1ULL<<3)|(1ULL<<4)|(1ULL<<5)| \
                (1ULL<<6)|(1ULL<<7)|(1ULL<<8)|(1ULL<<21)|(1ULL<<22)|(1ULL<<23)| \
                (1ULL<<27))
#define R_DIR  ((1ULL<<9)|(1ULL<<10)|(1ULL<<11)|(1ULL<<12)|(1ULL<<13)|(1ULL<<14)| \
                (1ULL<<15)|(1ULL<<16)|(1ULL<<17)|(1ULL<<18)|(1ULL<<19)|(1ULL<<20)| \
                (1ULL<<21)|(1ULL<<24)|(1ULL<<25)|(1ULL<<26))

/* WASI filetype -> S_IFMT bits and DT_* directory types */
#define WASI_FT_BLOCK_DEVICE 1
#define WASI_FT_CHAR_DEVICE  2
#define WASI_FT_DIRECTORY    3
#define WASI_FT_REGULAR_FILE 4
#define WASI_FT_SOCKET_DGRAM 5
#define WASI_FT_SOCKET_STREAM 6
#define WASI_FT_SYMBOLIC_LINK 7

/* ---- the preopened directories the host granted ---- */

#define MAX_PREOPENS 16
#define NAME_MAX_LEN 256

struct preopen {
    int  fd;
    int  len;
    char name[NAME_MAX_LEN];
};

static struct preopen g_pre[MAX_PREOPENS];
static int g_npre;
static int g_pre_ready;

/* The WASI prestat record: a tagged union; tag 0 is a directory and the only
   kind that carries a name length. */
struct prestat {
    uint8_t  tag;
    uint32_t name_len;
};

static void
preopen_init(void)
{
    int fd;

    if (g_pre_ready)
        return;
    g_pre_ready = 1;

    /* fds 0..2 are the standard streams; preopens start at 3 and run until the
       first fd that is not preopened (fd_prestat_get then fails). */
    for (fd = 3; g_npre < MAX_PREOPENS; fd++) {
        struct prestat ps;
        int len;

        if (__wasi_fd_prestat_get(fd, &ps) != 0)
            break;
        if (ps.tag != 0)                /* not a directory preopen */
            continue;
        len = (int)ps.name_len;
        if (len >= NAME_MAX_LEN)
            len = NAME_MAX_LEN - 1;
        if (__wasi_fd_prestat_dir_name(fd, g_pre[g_npre].name, len) != 0)
            continue;
        g_pre[g_npre].name[len] = '\0';
        g_pre[g_npre].fd = fd;
        g_pre[g_npre].len = len;
        g_npre++;
    }
}

/* Strip any leading "./" and a leading '/' so a path compares against the
   preopen names the host reports (typically "." or a mounted directory). */
static const char *
normalize(const char *path)
{
    while (path[0] == '.' && path[1] == '/')
        path += 2;
    return path;
}

/* Resolve a path to a (preopen fd, relative path).  The relative path points
   into the original string.  Returns the dir fd, or -1 when nothing matches. */
static int
resolve(const char *path, const char **relpath)
{
    const char *p;
    int best = -1, best_len = -1, i;

    preopen_init();
    p = normalize(path);

    /* An absolute path matches a preopen whose name is a prefix; a relative
       path matches the "." preopen (len 1).  Pick the longest matching name. */
    for (i = 0; i < g_npre; i++) {
        const char *name = g_pre[i].name;
        int nlen = g_pre[i].len;

        if (p[0] == '/') {
            if (strncmp(p, name, (size_t)nlen) == 0 &&
                (p[nlen] == '/' || p[nlen] == '\0')) {
                if (nlen > best_len) {
                    best = i;
                    best_len = nlen;
                }
            }
        } else if (name[0] == '.' && nlen == 1) {
            best = i;
            best_len = nlen;
        }
    }
    if (best < 0)
        return -1;

    if (p[0] == '/') {
        p += best_len;
        while (p[0] == '/')
            p++;
    }
    if (p[0] == '\0')
        p = ".";
    *relpath = p;
    return g_pre[best].fd;
}

/* ---- open / close / lseek ---- */

int
open(const char *path, int flags, int mode)
{
    const char *rel;
    int dirfd, oflags = 0, fdflags = 0, fd = -1, acc, plen;
    unsigned long long rights = R_FILE;

    (void)mode;                         /* WASI path_open has no mode bits */
    dirfd = resolve(path, &rel);
    if (dirfd < 0)
        return -1;
    plen = (int)strlen(rel);

    if (flags & O_CREAT)
        oflags |= WASI_O_CREAT;
    if (flags & O_TRUNC)
        oflags |= WASI_O_TRUNC;
    if (flags & O_APPEND)
        fdflags |= WASI_FD_APPEND;

    /* Drop the read right from a write-only open so the request stays within
       the preopen's file rights. */
    acc = flags & 3;
    if (acc == O_WRONLY)
        rights &= ~(1ULL << 1);         /* clear fd_read */

    /* First try as a regular file. */
    if (__wasi_path_open(dirfd, WASI_LOOKUP_SYMLINK_FOLLOW, rel, plen,
                         oflags, rights, R_FILE, fdflags, &fd) == 0)
        return fd;

    /* A read-only open with no create/truncate may be a directory (opendir
       opens with O_RDONLY); retry with the directory rights.  Requesting the
       file rights on a directory fd is ENOTCAPABLE, hence the separate try. */
    if (acc == O_RDONLY && !(flags & (O_CREAT | O_TRUNC)) &&
        __wasi_path_open(dirfd, WASI_LOOKUP_SYMLINK_FOLLOW, rel, plen,
                         oflags | WASI_O_DIRECTORY, R_DIR, R_FILE, fdflags, &fd) == 0)
        return fd;

    return -1;
}

int
close(int fd)
{
    return __wasi_fd_close(fd) == 0 ? 0 : -1;
}

long
lseek(int fd, long offset, int whence)
{
    unsigned long long newoff = 0;

    /* POSIX SEEK_SET/CUR/END are 0/1/2; WASI whence is also 0/1/2. */
    if (__wasi_fd_seek(fd, (long long)offset, whence, &newoff) != 0)
        return -1;
    return (long)newoff;
}

/* ---- statx (over a WASI filestat) ---- */

/* The WASI filestat record (64 bytes): dev, ino, filetype(+pad), nlink, size,
   atim, mtim, ctim.  Timestamps are nanoseconds since the epoch. */
struct wasi_filestat {
    uint64_t dev;
    uint64_t ino;
    uint8_t  filetype;
    uint8_t  pad[7];
    uint64_t nlink;
    uint64_t size;
    uint64_t atim;
    uint64_t mtim;
    uint64_t ctim;
};

/* The subset of the kernel statx layout the libc's from_statx() reads; the
   offsets must match libc/stat.c (sizeof == 256). */
struct statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    int64_t  stx_atime_sec;  uint32_t stx_atime_nsec; int32_t __r_atime;
    int64_t  stx_btime_sec;  uint32_t stx_btime_nsec; int32_t __r_btime;
    int64_t  stx_ctime_sec;  uint32_t stx_ctime_nsec; int32_t __r_ctime;
    int64_t  stx_mtime_sec;  uint32_t stx_mtime_nsec; int32_t __r_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;
    uint32_t stx_dio_offset_align;
    uint64_t __spare3[12];
};

static uint32_t
filetype_mode(uint8_t ft)
{
    switch (ft) {
    case WASI_FT_DIRECTORY:     return 0040000;      /* S_IFDIR */
    case WASI_FT_REGULAR_FILE:  return 0100000;      /* S_IFREG */
    case WASI_FT_CHAR_DEVICE:   return 0020000;      /* S_IFCHR */
    case WASI_FT_BLOCK_DEVICE:  return 0060000;      /* S_IFBLK */
    case WASI_FT_SYMBOLIC_LINK: return 0120000;      /* S_IFLNK */
    default:                    return 0;
    }
}

static void
fill_statx(struct statx *sx, const struct wasi_filestat *fs)
{
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask = 0x000007ffU;         /* STATX_BASIC_STATS */
    sx->stx_blksize = 512;
    sx->stx_nlink = (uint32_t)fs->nlink;
    sx->stx_mode = (uint16_t)(filetype_mode(fs->filetype) | 0644);
    sx->stx_ino = fs->ino;
    sx->stx_size = fs->size;
    sx->stx_blocks = (fs->size + 511) / 512;
    sx->stx_atime_sec = (int64_t)(fs->atim / 1000000000ULL);
    sx->stx_mtime_sec = (int64_t)(fs->mtim / 1000000000ULL);
    sx->stx_ctime_sec = (int64_t)(fs->ctim / 1000000000ULL);
}

int
statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *buf)
{
    struct wasi_filestat fs;

    (void)mask;
    if (flags & AT_EMPTY_PATH) {
        /* fstat form: the descriptor itself, path ignored. */
        if (__wasi_fd_filestat_get(dirfd, &fs) != 0)
            return -1;
    } else {
        const char *rel;
        int pfd = resolve(path, &rel);
        if (pfd < 0)
            return -1;
        if (__wasi_path_filestat_get(pfd, WASI_LOOKUP_SYMLINK_FOLLOW, rel,
                                     (int)strlen(rel), &fs) != 0)
            return -1;
    }
    fill_statx(buf, &fs);
    return 0;
}

/* ---- getdents64 (over a WASI directory read) ---- */

/* The WASI dirent header (24 bytes) precedes each name in the fd_readdir
   buffer: d_next (resume cookie), d_ino, d_namlen, d_type (+pad). */
struct wasi_dirent {
    uint64_t d_next;
    uint64_t d_ino;
    uint32_t d_namlen;
    uint8_t  d_type;
    uint8_t  pad[3];
};

/* Per-fd resume cookie, so repeated getdents64 calls walk the directory the
   way the kernel does with its implicit offset. */
#define MAX_DIRS 16
static int g_dir_fd[MAX_DIRS];
static unsigned long long g_dir_cookie[MAX_DIRS];
static int g_ndirs;

static unsigned long long *
dir_cookie(int fd)
{
    int i;
    for (i = 0; i < g_ndirs; i++)
        if (g_dir_fd[i] == fd)
            return &g_dir_cookie[i];
    if (g_ndirs < MAX_DIRS) {
        g_dir_fd[g_ndirs] = fd;
        g_dir_cookie[g_ndirs] = 0;
        return &g_dir_cookie[g_ndirs++];
    }
    return 0;
}

static uint8_t
filetype_dt(uint8_t ft)
{
    switch (ft) {
    case WASI_FT_DIRECTORY:      return 4;   /* DT_DIR */
    case WASI_FT_REGULAR_FILE:   return 8;   /* DT_REG */
    case WASI_FT_CHAR_DEVICE:    return 2;   /* DT_CHR */
    case WASI_FT_BLOCK_DEVICE:   return 6;   /* DT_BLK */
    case WASI_FT_SYMBOLIC_LINK:  return 10;  /* DT_LNK */
    case WASI_FT_SOCKET_DGRAM:
    case WASI_FT_SOCKET_STREAM:  return 12;  /* DT_SOCK */
    default:                     return 0;   /* DT_UNKNOWN */
    }
}

static void
st_u64(unsigned char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void
st_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

int
getdents64(int fd, void *buf, int count)
{
    /* Read one WASI batch, then translate as many whole entries as fit into the
       caller's linux_dirent64 buffer, advancing the cookie only past entries
       actually emitted so a truncated tail is re-read on the next call. */
    static unsigned char wbuf[4096];
    unsigned long long *ck = dir_cookie(fd);
    unsigned char *out = (unsigned char *)buf;
    int used = 0, woff = 0, outoff = 0;

    if (!ck)
        return -1;
    if (__wasi_fd_readdir(fd, wbuf, (int)sizeof wbuf, *ck, &used) != 0)
        return -1;

    while (woff + (int)sizeof(struct wasi_dirent) <= used) {
        struct wasi_dirent de;
        int namlen, reclen;
        const unsigned char *name;

        memcpy(&de, wbuf + woff, sizeof de);
        namlen = (int)de.d_namlen;
        name = wbuf + woff + sizeof(struct wasi_dirent);
        if (woff + (int)sizeof(struct wasi_dirent) + namlen > used)
            break;                      /* name truncated at the batch end */

        /* linux_dirent64: d_ino(8) d_off(8) d_reclen(2) d_type(1) name[]+NUL,
           padded to an 8-byte stride. */
        reclen = (8 + 8 + 2 + 1 + namlen + 1 + 7) & ~7;
        if (outoff + reclen > count)
            break;                      /* caller buffer full */

        st_u64(out + outoff + 0, de.d_ino);
        st_u64(out + outoff + 8, de.d_next);        /* d_off = resume cookie */
        st_u16(out + outoff + 16, (uint16_t)reclen);
        out[outoff + 18] = filetype_dt(de.d_type);
        memcpy(out + outoff + 19, name, (size_t)namlen);
        out[outoff + 19 + namlen] = '\0';

        outoff += reclen;
        woff += (int)sizeof(struct wasi_dirent) + namlen;
        *ck = de.d_next;
    }
    return outoff;
}

/* ---- path operations: mkdir / unlink / rmdir / rename / chmod ---- */

int
mkdirat(int dirfd, const char *path, int mode)
{
    const char *rel;
    int pfd;

    (void)dirfd;
    (void)mode;
    pfd = resolve(path, &rel);
    if (pfd < 0)
        return -1;
    return __wasi_path_create_directory(pfd, rel, (int)strlen(rel)) == 0 ? 0 : -1;
}

int
unlinkat(int dirfd, const char *path, int flags)
{
    const char *rel;
    int pfd;

    (void)dirfd;
    pfd = resolve(path, &rel);
    if (pfd < 0)
        return -1;
    if (flags & AT_REMOVEDIR)
        return __wasi_path_remove_directory(pfd, rel, (int)strlen(rel)) == 0 ? 0 : -1;
    return __wasi_path_unlink_file(pfd, rel, (int)strlen(rel)) == 0 ? 0 : -1;
}

int
renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath,
          unsigned flags)
{
    const char *orel, *nrel;
    int ofd, nfd;

    (void)olddirfd;
    (void)newdirfd;
    (void)flags;
    ofd = resolve(oldpath, &orel);
    nfd = resolve(newpath, &nrel);
    if (ofd < 0 || nfd < 0)
        return -1;
    return __wasi_path_rename(ofd, orel, (int)strlen(orel),
                              nfd, nrel, (int)strlen(nrel)) == 0 ? 0 : -1;
}

int
fchmodat(int dirfd, const char *path, int mode, int flags)
{
    /* WASI has no permission bits; report success so a chmod is a harmless
       no-op rather than a hard failure. */
    (void)dirfd;
    (void)path;
    (void)mode;
    (void)flags;
    return 0;
}
