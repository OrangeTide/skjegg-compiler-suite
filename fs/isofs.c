/* isofs.c - read-only ISO 9660 backend for the shared fs/ layer.
 *
 * Reads a CD-ROM filesystem the way romfs.c reads a packed image: it fits the
 * fs_backend vtable and holds the image in memory, so it mounts beside romfs in
 * the one mount table and a program reaches it through the same fs_open /
 * fs_read. It parses the primary volume descriptor, walks the directory tree by
 * directory records, and reads file extents.
 *
 * The image comes from one of two sources behind a single sector accessor. In
 * the RAM form it is a byte buffer (an embedded .iso, or a whole small disc read
 * into RAM) and a sector is a pointer into it. In the streaming form it is a
 * sector-read callback (on the PlayStation, libpsx/cd.c reading the disc), and a
 * sector is read on demand into a one-sector cache, so a disc-sized game need not
 * hold the image in RAM. The parser and the file reader are the same either way;
 * they only call sector(). Both forms are tested: the host isotest exercises RAM
 * and streaming (over a callback) and compares a multi-sector file byte for byte,
 * and on the console make test-psx-cd-redux streams off a booted disc.
 *
 * All multi-byte fields are read little-endian through explicit byte assembly
 * (ISO 9660 stores them both-endian; the little half is first), so one image is
 * valid on every target regardless of host byte order. The reader only walks
 * sectors, so it is freestanding-safe.
 */

#include <stddef.h>
#include <stdint.h>

#include "fs.h"
#include "fs_internal.h"
#include <errno.h>

#define ISO_SECTOR    2048
#define ISO_PVD_LBA   16            /* the primary volume descriptor */
#define ISO_FLAG_DIR  0x02          /* directory-record file-flags bit */

struct iso_img {
    const uint8_t *img;             /* RAM image, or NULL in streaming mode */
    size_t         len;             /* RAM image length */
    int          (*read_sector)(uint32_t lba, void *buf);  /* streaming source */
    uint32_t       root_lba;        /* root directory extent */
    uint32_t       root_size;       /* root directory length in bytes */
    int            valid;
};

/* One-sector cache for the streaming form: holds the last sector read through a
 * callback, so re-reading the same sector (and the whole RAM-style access
 * pattern) does not hit the disc twice. The RAM form does not use it. */
static uint8_t         sec_cache[ISO_SECTOR];
static struct iso_img *sec_owner;       /* which mount owns the cached sector */
static uint32_t        sec_lba = 0xFFFFFFFFu;

/* One open file: the extent and size resolved at open, so read is a copy. */
struct iso_open {
    uint32_t lba;
    uint32_t size;
    int      inuse;
};

static struct iso_img  iso_images[FS_MAX_MOUNTS];
static struct iso_open iso_opens[FS_MAX_FDS];

static uint16_t
rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t
rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static struct iso_img *
img_of(struct fs_mount *m)
{
    return &iso_images[(int)(m - fs_mounts)];
}

/* Pointer to a logical sector, valid until the next sector() call. In the RAM
 * form it points into the image; in the streaming form it points into the cache,
 * filled from the callback on a miss. Returns NULL past the end of the image or
 * on a read error. */
static const uint8_t *
sector(struct iso_img *r, uint32_t lba)
{
    if (r->img) {
        size_t off = (size_t)lba * ISO_SECTOR;
        if (off + ISO_SECTOR > r->len)
            return NULL;
        return r->img + off;
    }
    if (sec_owner != r || sec_lba != lba) {
        if (r->read_sector(lba, sec_cache) != 0) {
            sec_owner = NULL;           /* cache no longer holds a known sector */
            sec_lba = 0xFFFFFFFFu;
            return NULL;
        }
        sec_owner = r;
        sec_lba = lba;
    }
    return sec_cache;
}

/* Compare an ISO directory-record name to a path component, case-insensitively,
 * ignoring a trailing ";1" version on a file name. Returns 1 on match. */
static int
name_eq(const uint8_t *id, int idlen, const char *comp, int complen)
{
    int n = idlen;
    if (n >= 2 && id[n - 2] == ';')     /* strip ";1" version suffix */
        n -= 2;
    if (n == 1 && (id[0] == 0 || id[0] == 1))   /* "." / ".." records */
        return 0;
    if (n != complen)
        return 0;
    for (int i = 0; i < n; i++) {
        int a = id[i], b = comp[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b)
            return 0;
    }
    return 1;
}

/* Find a child named comp[0..complen) in the directory at (lba,size). Fills
 * the out parameters and returns 1, or returns 0 if not found. */
static int
find_child(struct iso_img *r, uint32_t lba, uint32_t size,
           const char *comp, int complen,
           uint32_t *out_lba, uint32_t *out_size, int *out_isdir)
{
    uint32_t remaining = size;
    while (remaining > 0) {
        const uint8_t *sec = sector(r, lba);
        if (!sec)
            return 0;
        uint32_t off = 0;
        while (off < ISO_SECTOR) {
            const uint8_t *rec = sec + off;
            int reclen = rec[0];
            if (reclen == 0)
                break;                      /* rest of the sector is padding */
            int idlen = rec[32];
            if (name_eq(rec + 33, idlen, comp, complen)) {
                *out_lba = rd_u32(rec + 2);
                *out_size = rd_u32(rec + 10);
                *out_isdir = (rec[25] & ISO_FLAG_DIR) ? 1 : 0;
                return 1;
            }
            off += (uint32_t)reclen;
        }
        lba++;
        remaining = (remaining > ISO_SECTOR) ? remaining - ISO_SECTOR : 0;
    }
    return 0;
}

/* Resolve a '/'-separated path from the root. Returns 1 with the file/dir
 * extent, or 0 if a component is missing. */
static int
walk(struct iso_img *r, const char *path,
     uint32_t *out_lba, uint32_t *out_size, int *out_isdir)
{
    uint32_t lba = r->root_lba, size = r->root_size;
    int isdir = 1;

    while (*path == '/')
        path++;
    if (*path == '\0') {
        *out_lba = r->root_lba;
        *out_size = r->root_size;
        *out_isdir = 1;
        return 1;
    }

    while (*path) {
        const char *start = path;
        while (*path && *path != '/')
            path++;
        int complen = (int)(path - start);
        if (!find_child(r, lba, size, start, complen, &lba, &size, &isdir))
            return 0;
        while (*path == '/')
            path++;
        if (*path && !isdir)
            return 0;                       /* a file cannot have children */
    }
    *out_lba = lba;
    *out_size = size;
    *out_isdir = isdir;
    return 1;
}

static int
alloc_open(void)
{
    for (int i = 0; i < FS_MAX_FDS; i++) {
        if (!iso_opens[i].inuse)
            return i;
    }
    return -1;
}

static intptr_t
iso_open(struct fs_mount *m, intptr_t base, const char *rel, int flags, int mode)
{
    struct iso_img *r = img_of(m);
    uint32_t lba, size;
    int isdir;
    (void)base;
    (void)mode;

    if ((flags & FS_O_ACCMODE) != FS_O_RDONLY || (flags & FS_O_CREAT))
        return -EROFS;
    if (!r->valid)
        return -EIO;
    if (!walk(r, rel, &lba, &size, &isdir))
        return -ENOENT;
    if (flags & FS_O_DIRECTORY) {
        if (!isdir)
            return -ENOTDIR;
    } else if (isdir) {
        return -EISDIR;
    }

    int h = alloc_open();
    if (h < 0)
        return -EMFILE;
    iso_opens[h].lba = lba;
    iso_opens[h].size = size;
    iso_opens[h].inuse = 1;
    return h;
}

static int
iso_read(struct fs_mount *m, intptr_t h, void *buf, int n, int64_t *pos)
{
    struct iso_img *r = img_of(m);
    struct iso_open *of = &iso_opens[(int)h];
    if (*pos < 0 || *pos >= (int64_t)of->size)
        return 0;
    int64_t avail = (int64_t)of->size - *pos;
    if ((int64_t)n > avail)
        n = (int)avail;

    /* A file is a contiguous run of sectors from of->lba. Walk it one sector at
     * a time so the read works the same from RAM or from the streaming cache,
     * and so a read spanning a sector boundary is handled. */
    unsigned char *dst = buf;
    int done = 0;
    while (done < n) {
        uint32_t fileoff = (uint32_t)(*pos) + (uint32_t)done;
        uint32_t lba = of->lba + fileoff / ISO_SECTOR;
        uint32_t soff = fileoff % ISO_SECTOR;
        const uint8_t *sec = sector(r, lba);
        if (!sec)
            return done ? done : -EIO;
        int chunk = ISO_SECTOR - (int)soff;
        if (chunk > n - done)
            chunk = n - done;
        for (int i = 0; i < chunk; i++)
            dst[done + i] = sec[soff + i];
        done += chunk;
    }
    *pos += done;
    return done;
}

static int
iso_write(struct fs_mount *m, intptr_t h, const void *buf, int n, int64_t *pos)
{
    (void)m; (void)h; (void)buf; (void)n; (void)pos;
    return -EROFS;
}

static int64_t
iso_lseek(struct fs_mount *m, intptr_t h, int64_t off, int whence, int64_t *pos)
{
    struct iso_open *of = &iso_opens[(int)h];
    int64_t base = (whence == 1) ? *pos : (whence == 2) ? (int64_t)of->size : 0;
    (void)m;
    *pos = base + off;
    return *pos;
}

static int
iso_readdir(struct fs_mount *m, intptr_t h, int64_t *cursor, struct fs_dirent *de)
{
    (void)m; (void)h; (void)cursor; (void)de;
    return 0;                               /* directory iteration: later */
}

static int
iso_close(struct fs_mount *m, intptr_t h, int is_dir)
{
    (void)m; (void)is_dir;
    if (h >= 0 && h < FS_MAX_FDS)
        iso_opens[(int)h].inuse = 0;
    return 0;
}

static int
iso_exists(struct fs_mount *m, intptr_t base, const char *rel)
{
    struct iso_img *r = img_of(m);
    uint32_t lba, size;
    int isdir;
    (void)base;
    return r->valid && walk(r, rel, &lba, &size, &isdir);
}

static int64_t
iso_size(struct fs_mount *m, intptr_t base, const char *rel)
{
    struct iso_img *r = img_of(m);
    uint32_t lba, size;
    int isdir;
    (void)base;
    if (!r->valid || !walk(r, rel, &lba, &size, &isdir) || isdir)
        return -ENOENT;
    return (int64_t)size;
}

static int
iso_isdir(struct fs_mount *m, intptr_t base, const char *rel)
{
    struct iso_img *r = img_of(m);
    uint32_t lba, size;
    int isdir;
    (void)base;
    return r->valid && walk(r, rel, &lba, &size, &isdir) && isdir;
}

static int
iso_ro(struct fs_mount *m, intptr_t base, const char *rel)
{
    (void)m; (void)base; (void)rel;
    return -EROFS;
}

static int
iso_ro2(struct fs_mount *m, intptr_t base, const char *from, const char *to)
{
    (void)m; (void)base; (void)from; (void)to;
    return -EROFS;
}

const struct fs_backend isofs_backend = {
    FS_KIND_ISO, "isofs",
    iso_open, iso_read, iso_write, iso_lseek, iso_readdir, iso_close,
    iso_exists, iso_size, iso_isdir, 0, 0, iso_ro, iso_ro2, iso_ro, iso_ro,
};

/* Parse a set-up image (its source already filled in): read the PVD through
 * sector() and take the root directory record. Returns 0, or a negative
 * errno. */
static int
isofs_parse(struct iso_img *out)
{
    out->valid = 0;
    const uint8_t *pvd = sector(out, ISO_PVD_LBA);
    if (!pvd)
        return -EINVAL;
    if (pvd[0] != 1 || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0'
        || pvd[4] != '0' || pvd[5] != '1')
        return -EINVAL;
    const uint8_t *root = pvd + 156;        /* root directory record */
    out->root_lba = rd_u32(root + 2);
    out->root_size = rd_u32(root + 10);
    out->valid = 1;
    (void)rd_u16;
    return 0;
}

/* Drop any cached streaming sector (a mount's parse used a stack-local image). */
static void
cache_reset(void)
{
    sec_owner = NULL;
    sec_lba = 0xFFFFFFFFu;
}

int
isofs_mount(const char *name, const void *image, size_t len, uint32_t flags)
{
    struct iso_img parsed;
    parsed.img = image;
    parsed.len = len;
    parsed.read_sector = 0;
    int m, rc;
    cache_reset();
    rc = isofs_parse(&parsed);
    if (rc < 0)
        return rc;
    m = fs_install_mount(name, FS_KIND_ISO, (intptr_t)0, flags | FS_RO);
    if (m < 0)
        return m;
    iso_images[m] = parsed;
    cache_reset();
    return m;
}

int
isofs_mount_stream(const char *name, int (*read_sector)(uint32_t, void *),
                   uint32_t flags)
{
    struct iso_img parsed;
    parsed.img = 0;
    parsed.len = 0;
    parsed.read_sector = read_sector;
    int m, rc;
    cache_reset();
    rc = isofs_parse(&parsed);
    if (rc < 0)
        return rc;
    m = fs_install_mount(name, FS_KIND_ISO, (intptr_t)0, flags | FS_RO);
    if (m < 0)
        return m;
    iso_images[m] = parsed;
    cache_reset();
    return m;
}

intptr_t
isofs_adopt(int idx, const void *image, size_t len)
{
    struct iso_img parsed;
    parsed.img = image;
    parsed.len = len;
    parsed.read_sector = 0;
    int rc;
    cache_reset();
    rc = isofs_parse(&parsed);
    if (rc < 0)
        return rc;
    fs_mounts[idx].flags |= FS_RO;
    iso_images[idx] = parsed;
    cache_reset();
    return 0;
}
