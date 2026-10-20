#include <sys/stat.h>
#include <stdint.h>

/* rv32 (like the other "new" architectures) has no stat or fstat syscall; the
   stat family is served by statx(2).  statx fills a fixed, architecture-
   independent record, so the struct below is pinned to the kernel ABI and the
   fields POSIX stat needs are copied out of it. */

#define AT_FDCWD          (-100)
#define AT_EMPTY_PATH     0x1000
#define STATX_BASIC_STATS 0x000007ffU

/* The four statx timestamps are flattened into their { s64 sec; u32 nsec; s32 }
   fields rather than a nested struct, keeping the 16-byte-per-timestamp layout
   while avoiding a nested-struct member read. */
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

/* Pin the overall size to the kernel ABI: a wrong 64-bit packing would change
   it.  (The member offsets are not asserted because skj-cc does not fold
   offsetof in a constant expression; the fields are laid out naturally aligned,
   and the round-trip test checks the values against the real kernel.) */
_Static_assert(sizeof(struct statx) == 256, "statx layout");

extern int statx(int dirfd, const char *path, int flags, unsigned mask,
                 struct statx *buf);

static void
from_statx(const struct statx *sx, struct stat *buf)
{
    buf->st_dev     = ((uint64_t)sx->stx_dev_major << 8) | sx->stx_dev_minor;
    buf->st_ino     = sx->stx_ino;
    buf->st_mode    = sx->stx_mode;
    buf->st_nlink   = sx->stx_nlink;
    buf->st_uid     = sx->stx_uid;
    buf->st_gid     = sx->stx_gid;
    buf->st_rdev    = ((uint64_t)sx->stx_rdev_major << 8) | sx->stx_rdev_minor;
    buf->st_size    = (int64_t)sx->stx_size;
    buf->st_blksize = (int32_t)sx->stx_blksize;
    buf->st_blocks  = (int64_t)sx->stx_blocks;
    buf->st_atime   = sx->stx_atime_sec;
    buf->st_mtime   = sx->stx_mtime_sec;
    buf->st_ctime   = sx->stx_ctime_sec;
}

int
fstat(int fd, struct stat *buf)
{
    struct statx sx;
    int r = statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx);

    if (r < 0)
        return r;
    from_statx(&sx, buf);
    return 0;
}

int
stat(const char *path, struct stat *buf)
{
    struct statx sx;
    int r = statx(AT_FDCWD, path, 0, STATX_BASIC_STATS, &sx);

    if (r < 0)
        return r;
    from_statx(&sx, buf);
    return 0;
}
