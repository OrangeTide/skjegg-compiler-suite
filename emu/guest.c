/* guest.c : sparse guest memory, the syscall layer, and the initial stack */

#define _POSIX_C_SOURCE 200809L         /* openat (and AT_FDCWD via fcntl.h) */
#define _DEFAULT_SOURCE                 /* syscall() for SYS_getdents64 */

#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <sched.h>

/****************************************************************
 * Sparse memory
 ****************************************************************/

void
gm_init(guest_mem *m, uint64_t limit)
{
    memset(m, 0, sizeof(*m));
    m->limit = limit;
}

void
gm_free(guest_mem *m)
{
    int i, j;

    for (i = 0; i < GM_L1_ENTRIES; i++) {
        if (!m->dir[i])
            continue;
        for (j = 0; j < GM_L2_ENTRIES; j++)
            free(m->dir[i][j]);
        free(m->dir[i]);
        m->dir[i] = NULL;
    }
    if (m->pool) {                      /* return this guest's pages to the pool */
        if (m->pool->used >= m->committed)
            m->pool->used -= m->committed;
        else
            m->pool->used = 0;
    }
    m->committed = 0;
}

int
gm_map(guest_mem *m, uint32_t base, uint32_t size, int prot, const char *name)
{
    struct gm_region *r;

    if (m->nregions >= GM_MAX_REGIONS)
        return -1;
    r = &m->regions[m->nregions++];
    r->base = base;
    r->size = size;
    r->prot = prot;
    r->name = name;
    return 0;
}

/* The dynamic mapping area sits above the image, heap, and stack the loader
 * places, and grows upward. Each mapping is followed by a one-page gap so a
 * following guard page never abuts the previous mapping. */
#define GM_MMAP_BASE    0x70000000u

uint32_t
gm_mmap(guest_mem *m, uint32_t size, unsigned flags)
{
    uint32_t base;

    uint64_t next, base64, end;
    int slots;

    size = (size + GM_PAGE_SIZE - 1) & ~(GM_PAGE_SIZE - 1);
    if (size == 0)
        return 0;
    if (m->mmap_next == 0)
        m->mmap_next = GM_MMAP_BASE;

    /* Lay out an optional guard page, the mapping, and a trailing gap, all in
     * 64-bit so a large request cannot wrap the 32-bit cursor and hand back a
     * low address that overlaps the image, the heap, or an earlier mapping.
     * The whole extent must stay inside the 32-bit space. */
    next = m->mmap_next;
    base64 = next + ((flags & GM_MAP_GUARD_LO) ? GM_PAGE_SIZE : 0);
    end = base64 + (uint64_t)size + GM_PAGE_SIZE;   /* + the trailing gap */
    if (end > 0x100000000ull)
        return 0;

    /* Reserve the region slots up front, so a full table never leaves a guard
     * mapped with no mapping behind it. */
    slots = (flags & GM_MAP_GUARD_LO) ? 2 : 1;
    if (m->nregions + slots > GM_MAX_REGIONS)
        return 0;

    if (flags & GM_MAP_GUARD_LO)        /* an unmapped page below the region */
        gm_map(m, (uint32_t)next, GM_PAGE_SIZE, 0, "guard");

    base = (uint32_t)base64;
    gm_map(m, base, size, GM_R | GM_W, "mapping");
    m->mmap_next = (uint32_t)end;
    return base;
}

void
gm_munmap(guest_mem *m, uint32_t base, uint32_t size)
{
    uint32_t a, pages, k;
    int i;

    size = (size + GM_PAGE_SIZE - 1) & ~(GM_PAGE_SIZE - 1);
    if (size == 0)
        return;
    if ((uint64_t)base + size > 0x100000000ull)     /* a range that wraps: ignore */
        return;

    /* Free the committed pages of the mapping (its guard never commits). Count
     * pages rather than compare against base + size, which would wrap. */
    pages = size >> GM_PAGE_SHIFT;
    for (k = 0, a = base; k < pages; k++, a += GM_PAGE_SIZE) {
        uint32_t l1 = a >> 22;
        uint32_t l2 = (a >> GM_PAGE_SHIFT) & (GM_L2_ENTRIES - 1);

        if (m->dir[l1] && m->dir[l1][l2]) {
            free(m->dir[l1][l2]);
            m->dir[l1][l2] = NULL;
            if (m->committed >= GM_PAGE_SIZE)
                m->committed -= GM_PAGE_SIZE;
            if (m->pool && m->pool->used >= GM_PAGE_SIZE)
                m->pool->used -= GM_PAGE_SIZE;
        }
    }

    /* Drop the mapping's region entry, and the guard page just below it if it
     * has one, keeping the region table order. */
    for (i = 0; i < m->nregions; i++) {
        if (m->regions[i].base == base && m->regions[i].size == size) {
            int lo = i, drop, j;

            if (i > 0 && m->regions[i - 1].prot == 0 &&
                m->regions[i - 1].base + m->regions[i - 1].size == base)
                lo = i - 1;
            drop = i - lo + 1;
            for (j = lo; j + drop < m->nregions; j++)
                m->regions[j] = m->regions[j + drop];
            m->nregions -= drop;
            break;
        }
    }
    m->last = NULL;             /* the compaction may dangle the lookup cache */
}

/** Region containing addr, or NULL.  Accesses cluster, so the last hit is
 * tried first and the scan is the exception. */
static struct gm_region *
region_of(guest_mem *m, uint32_t addr)
{
    struct gm_region *r = m->last;
    int i;

    if (r && addr >= r->base && addr - r->base < r->size)
        return r;
    for (i = 0; i < m->nregions; i++) {
        r = &m->regions[i];
        if (addr >= r->base && addr - r->base < r->size) {
            m->last = r;
            return r;
        }
    }
    return NULL;
}

/** Record the first fault; later ones do not overwrite it. */
static void
gm_fault(guest_mem *m, uint32_t addr, int is_write, const char *why)
{
    if (m->fault)
        return;
    m->fault = 1;
    m->fault_addr = addr;
    m->fault_write = is_write;
    m->fault_guard = 0;         /* the guard-page path sets this after us */
    m->fault_why = why;
}

uint8_t *
gm_page(guest_mem *m, uint32_t addr, int want_write)
{
    uint32_t l1 = addr >> 22;
    uint32_t l2 = (addr >> GM_PAGE_SHIFT) & (GM_L2_ENTRIES - 1);
    struct gm_region *r;
    uint8_t *page = m->dir[l1] ? m->dir[l1][l2] : NULL;

    /* A committed page satisfies a read outright; a write still has to
     * clear the region's protection. */
    if (page && !want_write)
        return page;

    r = region_of(m, addr);
    if (!r) {
        gm_fault(m, addr, want_write, "unmapped");
        return NULL;
    }
    if (r->prot == 0) {         /* a guard page: any access is a stack overflow */
        gm_fault(m, addr, want_write, "stack overflow");
        m->fault_guard = 1;
        return NULL;
    }
    if (want_write && !(r->prot & GM_W) && !m->loading) {
        gm_fault(m, addr, 1, "read-only");
        return NULL;
    }
    if (page)
        return page;
    if (m->committed + GM_PAGE_SIZE > m->limit) {
        gm_fault(m, addr, want_write, "memory limit");
        return NULL;
    }
    if (m->pool && m->pool->limit &&
        m->pool->used + GM_PAGE_SIZE > m->pool->limit) {
        gm_fault(m, addr, want_write, "memory pool exhausted");
        return NULL;
    }

    if (!m->dir[l1]) {
        m->dir[l1] = calloc(GM_L2_ENTRIES, sizeof(uint8_t *));
        if (!m->dir[l1]) {
            gm_fault(m, addr, want_write, "out of host memory");
            return NULL;
        }
    }
    page = calloc(1, GM_PAGE_SIZE);
    if (!page) {
        gm_fault(m, addr, want_write, "out of host memory");
        return NULL;
    }
    m->dir[l1][l2] = page;
    m->committed += GM_PAGE_SIZE;
    if (m->pool)
        m->pool->used += GM_PAGE_SIZE;
    return page;
}

uint8_t
gm_read8(guest_mem *m, uint32_t addr)
{
    uint8_t *p = gm_page(m, addr, 0);

    return p ? p[addr & (GM_PAGE_SIZE - 1)] : 0;
}

void
gm_write8(guest_mem *m, uint32_t addr, uint8_t val)
{
    uint8_t *p = gm_page(m, addr, 1);

    if (p)
        p[addr & (GM_PAGE_SIZE - 1)] = val;
}

uint32_t
gm_load(guest_mem *m, uint32_t addr, int size, int be)
{
    uint32_t off = addr & (GM_PAGE_SIZE - 1);
    uint8_t b[4];
    int i;

    if (off + (uint32_t)size <= GM_PAGE_SIZE) {
        uint8_t *p = gm_page(m, addr, 0);
        if (!p)
            return 0;
        memcpy(b, p + off, (size_t)size);
    } else {
        for (i = 0; i < size; i++)
            b[i] = gm_read8(m, addr + (uint32_t)i);
    }

    switch (size) {
    case 1:
        return b[0];
    case 2:
        return be ? (uint32_t)((b[0] << 8) | b[1])
                  : (uint32_t)(b[0] | (b[1] << 8));
    default:
        return be ? ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                        ((uint32_t)b[2] << 8) | b[3]
                  : (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                        ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    }
}

void
gm_store(guest_mem *m, uint32_t addr, int size, int be, uint32_t val)
{
    uint32_t off = addr & (GM_PAGE_SIZE - 1);
    uint8_t b[4];
    int i;

    switch (size) {
    case 1:
        b[0] = (uint8_t)val;
        break;
    case 2:
        b[0] = be ? (uint8_t)(val >> 8) : (uint8_t)val;
        b[1] = be ? (uint8_t)val : (uint8_t)(val >> 8);
        break;
    default:
        for (i = 0; i < 4; i++)
            b[i] = be ? (uint8_t)(val >> (24 - 8 * i))
                      : (uint8_t)(val >> (8 * i));
        break;
    }

    if (off + (uint32_t)size <= GM_PAGE_SIZE) {
        uint8_t *p = gm_page(m, addr, 1);
        if (p)
            memcpy(p + off, b, (size_t)size);
        return;
    }
    for (i = 0; i < size; i++)
        gm_write8(m, addr + (uint32_t)i, b[i]);
}

void
gm_write(guest_mem *m, uint32_t addr, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    uint32_t i;

    for (i = 0; i < len; i++)
        gm_write8(m, addr + i, s[i]);
}

void
gm_zero(guest_mem *m, uint32_t addr, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++)
        gm_write8(m, addr + i, 0);
}

void
gm_read_out(guest_mem *m, uint32_t addr, void *dst, uint32_t len)
{
    uint8_t *d = dst;
    uint32_t i;

    for (i = 0; i < len; i++)
        d[i] = gm_read8(m, addr + i);
}

/****************************************************************
 * Syscalls
 *
 * Only the calls a freestanding guest can reach.  Everything else
 * answers -ENOSYS the way Linux would, so an unexpected call shows up as
 * a guest-visible error rather than a silent success.
 ****************************************************************/

#define GUEST_ENOENT    2
#define GUEST_EBADF     9
#define GUEST_ENOMEM    12
#define GUEST_EFAULT    14
#define GUEST_ENOSYS    38
#define GUEST_EINVAL    22

/** Copy a guest buffer into a host one.  Returns 0 on a fault. */
static int
copy_from_guest(guest *g, uint32_t addr, void *dst, uint32_t len)
{
    uint8_t *d = dst;
    uint32_t i;

    for (i = 0; i < len; i++) {
        uint8_t *p = gm_page(&g->mem, addr + i, 0);
        if (!p)
            return 0;
        d[i] = p[(addr + i) & (GM_PAGE_SIZE - 1)];
    }
    return 1;
}

static int32_t
sys_write(guest *g, uint32_t fd, uint32_t buf, uint32_t len)
{
    uint8_t tmp[4096];
    uint32_t done = 0;

    while (done < len) {
        uint32_t chunk = len - done;
        ssize_t n;
        if (chunk > sizeof(tmp))
            chunk = sizeof(tmp);
        if (!copy_from_guest(g, buf + done, tmp, chunk))
            return -GUEST_EFAULT;
        /* --quiet suppresses only the console streams, not a real file. */
        if (g->quiet && (fd == 1 || fd == 2)) {
            done += chunk;
            continue;
        }
        n = write((int)fd, tmp, chunk);
        if (n <= 0)
            return done ? (int32_t)done : -GUEST_EBADF;
        done += (uint32_t)n;
    }
    return (int32_t)done;
}

static int32_t
sys_read(guest *g, uint32_t fd, uint32_t buf, uint32_t len)
{
    uint8_t tmp[4096];
    ssize_t n;

    if (len > sizeof(tmp))
        len = sizeof(tmp);
    n = read((int)fd, tmp, len);
    if (n < 0)
        return -GUEST_EBADF;
    gm_write(&g->mem, buf, tmp, (uint32_t)n);
    if (g->mem.fault)
        return -GUEST_EFAULT;
    return (int32_t)n;
}

/* Host file access for the guest.  skj-run is a user-mode development
 * emulator, like qemu-user, so a guest may open real files with the invoking
 * user's own permissions; this is what lets a guest compiler such as kpcom
 * read its source and write its output.  The open flags and mode are the
 * asm-generic Linux values both the m68k and rv guests use, which match the
 * host's, so they pass through unchanged.  An opened descriptor is the host
 * fd, used directly by sys_read / sys_write / close. */
/* Copy a NUL-terminated guest path string into `out` (at most cap bytes).
 * Returns 0 on success, -GUEST_EFAULT on an unmapped byte, -GUEST_ENOENT if it
 * does not terminate within the buffer. */
static int32_t
copy_path(guest *g, uint32_t pathp, char *out, size_t cap)
{
    size_t i;

    for (i = 0; i < cap - 1; i++) {
        uint8_t *p = gm_page(&g->mem, pathp + (uint32_t)i, 0);
        if (!p)
            return -GUEST_EFAULT;
        out[i] = (char)p[(pathp + (uint32_t)i) & (GM_PAGE_SIZE - 1)];
        if (out[i] == '\0')
            return 0;
    }
    return -GUEST_ENOENT;
}

static int32_t
sys_open(guest *g, uint32_t pathp, uint32_t flags, uint32_t mode)
{
    char path[1024];
    int32_t e;
    int fd;

    if ((e = copy_path(g, pathp, path, sizeof(path))) != 0)
        return e;
    fd = open(path, (int)flags, mode);
    if (fd < 0)
        return -GUEST_ENOENT;
    return (int32_t)fd;
}

/* openat(dirfd, path, flags, mode): the form rv32 (and any asm-generic Linux
 * target) uses, since it has no bare open.  dirfd is passed through to the host
 * openat, so AT_FDCWD (-100, the same value on the guest) and a real directory
 * fd the guest opened both work.  The flags and mode are the asm-generic Linux
 * values, which match the host's. */
static int32_t
sys_openat(guest *g, uint32_t dirfd, uint32_t pathp, uint32_t flags,
           uint32_t mode)
{
    char path[1024];
    int32_t e;
    int fd;

    if ((e = copy_path(g, pathp, path, sizeof(path))) != 0)
        return e;
    fd = openat((int)dirfd, path, (int)flags, mode);
    if (fd < 0)
        return -GUEST_ENOENT;
    return (int32_t)fd;
}

/* lseek(fd, offset, whence): a 32-bit offset for the ILP32 guests, sign
 * extended to the host off_t.  Returns the new offset, or -errno.  This is the
 * simple form m68k uses; rv32/asm-generic use llseek below. */
static int32_t
sys_lseek(guest *g, uint32_t fd, uint32_t offset, uint32_t whence)
{
    off_t r;

    (void)g;
    r = lseek((int)fd, (off_t)(int32_t)offset, (int)whence);
    if (r < 0)
        return -GUEST_EBADF;
    return (int32_t)r;
}

/* _llseek(fd, off_hi, off_lo, loff_t *result, whence): the asm-generic (rv32)
 * form.  The 64-bit offset is split across two words; the new position is
 * written back through the guest pointer as a little-endian loff_t.  Returns 0
 * on success, -errno on failure. */
static int32_t
sys_llseek(guest *g, uint32_t fd, uint32_t off_hi, uint32_t off_lo,
           uint32_t resultp, uint32_t whence)
{
    int64_t off = (int64_t)(((uint64_t)off_hi << 32) | off_lo);
    off_t r = lseek((int)fd, (off_t)off, (int)whence);

    if (r < 0)
        return -GUEST_EBADF;
    /* write the 8-byte loff_t result, little-endian, to the guest */
    gm_store(&g->mem, resultp, 4, 0, (uint32_t)(uint64_t)r);
    gm_store(&g->mem, resultp + 4, 4, 0, (uint32_t)((uint64_t)r >> 32));
    if (g->mem.fault)
        return -GUEST_EFAULT;
    return 0;
}

/* statx(dirfd, path, flags, mask, buf): the stat form the "new" Linux targets
 * (rv32) use, since they have no stat/fstat syscall.  The guest libc translates
 * the filled record into POSIX struct stat (libc/stat.c).  AT_EMPTY_PATH (the
 * 0x1000 flag bit, value matched numerically so the host needs no _GNU_SOURCE)
 * means "stat the fd in dirfd", the fstat case; otherwise it is a path stat
 * relative to dirfd (AT_FDCWD for an absolute or cwd-relative path).  The host
 * fd is the guest fd, as for read/write.  Only the STATX_BASIC_STATS fields the
 * guest reads are filled, at their fixed struct-statx offsets, little-endian. */
static int32_t
sys_statx(guest *g, uint32_t dirfd, uint32_t pathp, uint32_t flags,
          uint32_t mask, uint32_t bufp)
{
    char path[1024];
    struct stat hs;
    int r;
    uint32_t i;

    (void)mask;
    if (flags & 0x1000u) {                      /* AT_EMPTY_PATH: fstat the fd */
        r = fstat((int)(int32_t)dirfd, &hs);
    } else {
        int32_t e = copy_path(g, pathp, path, sizeof(path));
        if (e != 0)
            return e;
        r = fstatat((int)(int32_t)dirfd, path, &hs, 0);
    }
    if (r < 0)
        return -GUEST_ENOENT;

    for (i = 0; i < 256; i += 4)                /* zero the struct statx first */
        gm_store(&g->mem, bufp + i, 4, 0, 0);
    gm_store(&g->mem, bufp + 0,  4, 0, 0x000007ffu);                  /* stx_mask */
    gm_store(&g->mem, bufp + 4,  4, 0, (uint32_t)hs.st_blksize);      /* stx_blksize */
    gm_store(&g->mem, bufp + 16, 4, 0, (uint32_t)hs.st_nlink);        /* stx_nlink */
    gm_store(&g->mem, bufp + 20, 4, 0, (uint32_t)hs.st_uid);          /* stx_uid */
    gm_store(&g->mem, bufp + 24, 4, 0, (uint32_t)hs.st_gid);          /* stx_gid */
    gm_store(&g->mem, bufp + 28, 4, 0, (uint32_t)(hs.st_mode & 0xffffu)); /* stx_mode + spare */
    gm_store(&g->mem, bufp + 32, 4, 0, (uint32_t)hs.st_ino);          /* stx_ino lo */
    gm_store(&g->mem, bufp + 36, 4, 0, (uint32_t)((uint64_t)hs.st_ino >> 32));
    gm_store(&g->mem, bufp + 40, 4, 0, (uint32_t)hs.st_size);         /* stx_size lo */
    gm_store(&g->mem, bufp + 44, 4, 0, (uint32_t)((uint64_t)hs.st_size >> 32));
    gm_store(&g->mem, bufp + 48, 4, 0, (uint32_t)hs.st_blocks);       /* stx_blocks lo */
    gm_store(&g->mem, bufp + 52, 4, 0, (uint32_t)((uint64_t)hs.st_blocks >> 32));
    gm_store(&g->mem, bufp + 64, 4, 0, (uint32_t)hs.st_atime);        /* stx_atime.sec lo */
    gm_store(&g->mem, bufp + 96, 4, 0, (uint32_t)hs.st_ctime);        /* stx_ctime.sec lo */
    gm_store(&g->mem, bufp + 112,4, 0, (uint32_t)hs.st_mtime);        /* stx_mtime.sec lo */
    if (g->mem.fault)
        return -GUEST_EFAULT;
    return 0;
}

/* The *at directory/path operations the guest libc uses (via AT_FDCWD) for
 * mkdir/unlink/rmdir/rename.  dirfd is passed through to the host, as for
 * openat.  A failure returns -ENOENT (the guest libc only checks for nonzero,
 * and remove() retries with AT_REMOVEDIR on it), matching sys_open's style. */
static int32_t
sys_mkdirat(guest *g, uint32_t dirfd, uint32_t pathp, uint32_t mode)
{
    char path[1024];
    int32_t e;

    if ((e = copy_path(g, pathp, path, sizeof(path))) != 0)
        return e;
    return mkdirat((int)(int32_t)dirfd, path, (mode_t)mode) == 0
               ? 0 : -GUEST_ENOENT;
}

static int32_t
sys_unlinkat(guest *g, uint32_t dirfd, uint32_t pathp, uint32_t flags)
{
    char path[1024];
    int32_t e;

    if ((e = copy_path(g, pathp, path, sizeof(path))) != 0)
        return e;
    return unlinkat((int)(int32_t)dirfd, path, (int)flags) == 0
               ? 0 : -GUEST_ENOENT;
}

/* renameat2(olddirfd, oldpath, newdirfd, newpath, flags): the guest libc always
 * passes flags 0, which is plain renameat (no _GNU_SOURCE needed on the host);
 * a nonzero flag word is rejected rather than silently dropped. */
static int32_t
sys_renameat2(guest *g, uint32_t od, uint32_t oldp, uint32_t nd,
              uint32_t newp, uint32_t flags)
{
    char oldpath[1024], newpath[1024];
    int32_t e;

    if (flags != 0)
        return -GUEST_EINVAL;
    if ((e = copy_path(g, oldp, oldpath, sizeof(oldpath))) != 0)
        return e;
    if ((e = copy_path(g, newp, newpath, sizeof(newpath))) != 0)
        return e;
    return renameat((int)(int32_t)od, oldpath, (int)(int32_t)nd, newpath) == 0
               ? 0 : -GUEST_ENOENT;
}

/* Store a 64-bit value to guest memory honoring the guest's byte order. */
static void
store64(guest *g, uint32_t addr, uint64_t v)
{
    if (g->big_endian) {
        gm_store(&g->mem, addr, 4, 0, (uint32_t)(v >> 32));
        gm_store(&g->mem, addr + 4, 4, 0, (uint32_t)v);
    } else {
        gm_store(&g->mem, addr, 4, 0, (uint32_t)v);
        gm_store(&g->mem, addr + 4, 4, 0, (uint32_t)(v >> 32));
    }
}

/* getdents64(fd, dirp, count): read directory entries into the guest buffer.
 * The host is asked (via the raw syscall) for entries into a local buffer, then
 * each struct linux_dirent64 is re-emitted into guest memory field by field so
 * the multi-byte fields take the guest's byte order; the name bytes copy raw.
 * The kernel computes d_reclen as round_up(19 + namelen + 1, 8) with the same
 * 19-byte head on every arch, so the guest layout matches the host one byte for
 * byte and the total equals the host's return.  Returns bytes written, 0 at end
 * of directory, -errno on error. */
static int32_t
sys_getdents64(guest *g, uint32_t fd, uint32_t dirp, uint32_t count)
{
    char hbuf[4096];
    long n;
    int hoff;
    uint32_t gpos = 0;
    uint32_t want = count < sizeof(hbuf) ? count : (uint32_t)sizeof(hbuf);

    n = syscall(SYS_getdents64, (int)fd, hbuf, (size_t)want);
    if (n < 0)
        return -GUEST_ENOENT;
    for (hoff = 0; hoff + 19 <= (int)n; ) {
        /* host entry fields at their fixed offsets (host native order) */
        uint64_t ino, off;
        uint16_t reclen;
        uint8_t  type;
        int namebytes;
        memcpy(&ino, hbuf + hoff + 0, 8);
        memcpy(&off, hbuf + hoff + 8, 8);
        memcpy(&reclen, hbuf + hoff + 16, 2);
        type = (uint8_t)hbuf[hoff + 18];
        if (reclen < 20 || hoff + reclen > (int)n)
            break;
        namebytes = reclen - 19;        /* name + NUL + padding */
        store64(g, dirp + gpos + 0, ino);
        store64(g, dirp + gpos + 8, off);
        gm_store(&g->mem, dirp + gpos + 16, 2, 0, reclen);
        gm_store(&g->mem, dirp + gpos + 18, 1, 0, type);
        gm_write(&g->mem, dirp + gpos + 19, hbuf + hoff + 19, (uint32_t)namebytes);
        gpos += reclen;
        hoff += reclen;
    }
    if (g->mem.fault)
        return -GUEST_EFAULT;
    return (int32_t)gpos;
}

/* Linux brk: return the new break, or the current one when the request
 * cannot be met.  The heap is a region mapped at startup, so growing the
 * break only moves a pointer inside it. */
static int32_t
sys_brk(guest *g, uint32_t want)
{
    if (want >= g->brk_base && want <= g->brk_limit)
        g->brk = want;
    return (int32_t)g->brk;
}

/* clock_gettime(clk_id, struct timespec *ts): write a 32-bit timespec
 * (tv_sec, tv_nsec, each one target-endian word) and return 0.  This seam uses
 * a 32-bit timespec rather than rv32 Linux's 64-bit one, a private convention
 * skj-run and boris share; a monotonic uptime fits in 32 bits of seconds.
 * clk_id 0 (CLOCK_REALTIME) reports wall-clock, anything else the monotonic
 * clock the guest's millisecond timer wants. */
static int32_t
sys_clock_gettime(guest *g, uint32_t clk_id, uint32_t ts_ptr)
{
    struct timespec host;

    if (ts_ptr == 0)
        return -GUEST_EFAULT;
    if (clock_gettime(clk_id == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &host) != 0)
        return -GUEST_EINVAL;
    gm_store(&g->mem, ts_ptr + 0, 4, g->big_endian, (uint32_t)host.tv_sec);
    gm_store(&g->mem, ts_ptr + 4, 4, g->big_endian, (uint32_t)host.tv_nsec);
    return 0;
}

/* getrandom(buf, len, flags): fill the guest buffer with len random bytes from
 * the host and return the count.  The guest seeds its own generators from this;
 * flags are ignored (the host source never blocks meaningfully here). */
static int32_t
sys_getrandom(guest *g, uint32_t buf, uint32_t len, uint32_t flags)
{
    uint8_t tmp[256];
    uint32_t done = 0;

    (void)flags;
    if (len == 0)
        return 0;
    if (buf == 0)
        return -GUEST_EFAULT;
    while (done < len) {
        uint32_t chunk = len - done;
        long n;

        if (chunk > sizeof tmp)
            chunk = sizeof tmp;
        n = syscall(SYS_getrandom, tmp, (size_t)chunk, 0);
        if (n <= 0)
            return done > 0 ? (int32_t)done : -GUEST_EINVAL;
        gm_write(&g->mem, buf + done, tmp, (uint32_t)n);
        done += (uint32_t)n;
    }
    return (int32_t)done;
}

/* nanosleep(req, rem): park for the {tv_sec, tv_nsec} the guest timespec holds
 * (the 32-bit timespec convention, as clock_gettime above).  skj-run really
 * sleeps on the host clock; the guest's Sleep/park glue uses this.  rem (the
 * unslept remainder on a signal) is not written back; skj-run has no signals
 * delivered to the guest, so a sleep here always completes. */
static int32_t
sys_nanosleep(guest *g, uint32_t req_ptr, uint32_t rem_ptr)
{
    struct timespec req;
    uint32_t sec, nsec;

    (void)rem_ptr;
    if (req_ptr == 0)
        return -GUEST_EFAULT;
    sec  = gm_load(&g->mem, req_ptr + 0, 4, g->big_endian);
    nsec = gm_load(&g->mem, req_ptr + 4, 4, g->big_endian);
    if (nsec >= 1000000000u)
        return -GUEST_EINVAL;
    req.tv_sec = (time_t)sec;
    req.tv_nsec = (long)nsec;
    while (nanosleep(&req, &req) != 0)
        ;                               /* resume across a host signal */
    return 0;
}

int32_t
guest_syscall(guest *g, int sys, uint32_t a0, uint32_t a1, uint32_t a2,
              uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a5;

    g->syscalls++;

    switch (sys) {
    case GSYS_WRITE:
        return sys_write(g, a0, a1, a2);

    case GSYS_READ:
        return sys_read(g, a0, a1, a2);

    case GSYS_WRITEV: {
        /* struct iovec { void *base; size_t len; } laid out in guest
         * order; both targets are ILP32 so an entry is two words. */
        uint32_t i, total = 0;
        for (i = 0; i < a2; i++) {
            uint32_t ent = a1 + i * 8;
            uint8_t w[8];
            uint32_t base, len;
            int32_t r;
            if (!copy_from_guest(g, ent, w, 8))
                return -GUEST_EFAULT;
            if (g->big_endian) {
                base = ((uint32_t)w[0] << 24) | ((uint32_t)w[1] << 16) |
                       ((uint32_t)w[2] << 8) | w[3];
                len = ((uint32_t)w[4] << 24) | ((uint32_t)w[5] << 16) |
                      ((uint32_t)w[6] << 8) | w[7];
            } else {
                base = (uint32_t)w[0] | ((uint32_t)w[1] << 8) |
                       ((uint32_t)w[2] << 16) | ((uint32_t)w[3] << 24);
                len = (uint32_t)w[4] | ((uint32_t)w[5] << 8) |
                      ((uint32_t)w[6] << 16) | ((uint32_t)w[7] << 24);
            }
            r = sys_write(g, a0, base, len);
            if (r < 0)
                return total ? (int32_t)total : r;
            total += (uint32_t)r;
        }
        return (int32_t)total;
    }

    case GSYS_BRK:
        return sys_brk(g, a0);

    case GSYS_OPEN:
        return sys_open(g, a0, a1, a2);

    case GSYS_OPENAT:
        return sys_openat(g, a0, a1, a2, a3);

    case GSYS_CLOSE:
        /* Keep the standard streams open; close a real file. */
        if (a0 > 2)
            return close((int)a0) == 0 ? 0 : -GUEST_EBADF;
        return 0;

    case GSYS_LSEEK:
        return sys_lseek(g, a0, a1, a2);

    case GSYS_LLSEEK:
        return sys_llseek(g, a0, a1, a2, a3, a4);

    case GSYS_STATX:
        return sys_statx(g, a0, a1, a2, a3, a4);

    case GSYS_MKDIRAT:
        return sys_mkdirat(g, a0, a1, a2);

    case GSYS_UNLINKAT:
        return sys_unlinkat(g, a0, a1, a2);

    case GSYS_RENAMEAT2:
        return sys_renameat2(g, a0, a1, a2, a3, a4);

    case GSYS_GETDENTS64:
        return sys_getdents64(g, a0, a1, a2);

    case GSYS_CLOCK_GETTIME:
        return sys_clock_gettime(g, a0, a1);

    case GSYS_GETRANDOM:
        return sys_getrandom(g, a0, a1, a2);

    case GSYS_NANOSLEEP:
        return sys_nanosleep(g, a0, a1);

    case GSYS_SCHED_YIELD:
        sched_yield();
        return 0;

    case GSYS_EXIT:
    case GSYS_EXIT_GROUP:
        g->exited = 1;
        g->exit_code = (int)(a0 & 0xFF);
        return 0;

    default:
        return -GUEST_ENOSYS;
    }
}

/****************************************************************
 * Initial stack
 *
 * The layout Linux hands a fresh process: argc, the argv pointers, a
 * NULL, the envp pointers, a NULL, then a terminated auxv.  The strings
 * live above it.  Skjegg's own start.S switches to a private stack and
 * ignores all of this, but a guest built against a real libc does not,
 * and building it costs nothing.
 ****************************************************************/

static void
poke32(guest *g, uint32_t addr, uint32_t val)
{
    if (g->big_endian) {
        gm_write8(&g->mem, addr + 0, (uint8_t)(val >> 24));
        gm_write8(&g->mem, addr + 1, (uint8_t)(val >> 16));
        gm_write8(&g->mem, addr + 2, (uint8_t)(val >> 8));
        gm_write8(&g->mem, addr + 3, (uint8_t)val);
    } else {
        gm_write8(&g->mem, addr + 0, (uint8_t)val);
        gm_write8(&g->mem, addr + 1, (uint8_t)(val >> 8));
        gm_write8(&g->mem, addr + 2, (uint8_t)(val >> 16));
        gm_write8(&g->mem, addr + 3, (uint8_t)(val >> 24));
    }
}

#define AT_NULL     0
#define AT_PAGESZ   6

uint32_t
guest_build_stack(guest *g, const run_opts *o)
{
    uint32_t sp = g->stack_top;
    uint32_t str[64] = { 0 };
    int argc = o->argc;
    int i;

    if (argc > (int)(sizeof(str) / sizeof(str[0])))
        argc = (int)(sizeof(str) / sizeof(str[0]));

    /* strings, top down */
    for (i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(o->argv[i]) + 1;
        sp -= len;
        gm_write(&g->mem, sp, o->argv[i], len);
        str[i] = sp;
    }
    sp &= ~15u;

    /* auxv (two words), the envp NULL, the argv NULL, argv, argc */
    sp -= 4 * (2 + 2 + 1 + 1 + (uint32_t)argc + 1);
    sp &= ~15u;

    {
        uint32_t p = sp;
        poke32(g, p, (uint32_t)argc);
        p += 4;
        for (i = 0; i < argc; i++, p += 4)
            poke32(g, p, str[i]);
        poke32(g, p, 0);            /* argv terminator */
        p += 4;
        poke32(g, p, 0);            /* envp terminator */
        p += 4;
        poke32(g, p, AT_PAGESZ);
        p += 4;
        poke32(g, p, GM_PAGE_SIZE);
        p += 4;
        poke32(g, p, AT_NULL);
        p += 4;
        poke32(g, p, 0);
    }
    return sp;
}
