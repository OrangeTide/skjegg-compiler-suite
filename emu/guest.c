/* guest.c : sparse guest memory, the syscall layer, and the initial stack */

/* This file is the host-agnostic guest syscall layer: it carries no host-OS
 * #ifdefs.  Every facility that differs by host (confined opens, directory
 * enumeration, the clock, random bytes) is reached through hostplat.h, whose
 * one implementation per OS lives in hostplat.c. */
#if defined(__APPLE__)
# define _DARWIN_C_SOURCE               /* the *at family, fdopendir, ...        */
#else
# define _POSIX_C_SOURCE 200809L        /* openat / fstatat / renameat, AT_FDCWD  */
# define _DEFAULT_SOURCE
#endif

#include "guest.h"
#include "elf32.h"
#include "fs.h"             /* the guest fs/ mount-table layout the launcher fills */
#include "ninefs.h"         /* P9_MSIZE: the 9p channel buffer size               */
#include "p9_server.h"      /* the host-side 9p file server the doorbell drives   */
#include "hostplat.h"       /* the host facilities that service the guest ABI     */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

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
    for (i = 0; i < GM_L1_ENTRIES; i++) {       /* wdir borrows dir's pages */
        free(m->wdir[i]);
        m->wdir[i] = NULL;
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
            if (m->wdir[l1])            /* drop the borrowed write-cache pointer */
                m->wdir[l1][l2] = NULL;
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

void
gm_set_extern(guest_mem *m, uint8_t *base, uint32_t size)
{
    m->extern_base = base;
    m->extern_size = size;
    gm_map(m, 0, size, GM_R | GM_W | GM_X, "ram");
}

uint8_t *
gm_page(guest_mem *m, uint32_t addr, int want_write)
{
    uint32_t l1 = addr >> 22;
    uint32_t l2 = (addr >> GM_PAGE_SHIFT) & (GM_L2_ENTRIES - 1);
    struct gm_region *r;
    uint8_t *page;

    /* Extern-backed guest: the whole range is one host buffer.  Honour the
     * region protection (guard pages, read-only during loading) but return the
     * arithmetic page pointer rather than a committed one. */
    if (m->extern_base) {
        if (addr >= m->extern_size) {
            gm_fault(m, addr, want_write, "unmapped");
            return NULL;
        }
        r = region_of(m, addr);
        if (!r) {
            gm_fault(m, addr, want_write, "unmapped");
            return NULL;
        }
        if (r->prot == 0) {
            gm_fault(m, addr, want_write, "stack overflow");
            m->fault_guard = 1;
            return NULL;
        }
        if (want_write && !(r->prot & GM_W) && !m->loading) {
            gm_fault(m, addr, 1, "read-only");
            return NULL;
        }
        return m->extern_base + (addr & ~(GM_PAGE_SIZE - 1));
    }

    /* A committed page satisfies a read outright. A write to a committed page
     * in a writable region is recorded in wdir, so it too returns without the
     * region-protection re-check (the common case for the hot store path). */
    if (want_write) {
        page = m->wdir[l1] ? m->wdir[l1][l2] : NULL;
        if (page)
            return page;
    } else {
        page = m->dir[l1] ? m->dir[l1][l2] : NULL;
        if (page)
            return page;
    }

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

    page = m->dir[l1] ? m->dir[l1][l2] : NULL;
    if (!page) {
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
    }

    /* Warm the write fast path for a page that is writable in steady state.
     * A read commit warms it too, so the first store is already fast. During
     * loading a read-only region is transiently writable; it is deliberately
     * left out of wdir, so once loading ends a store to it faults here. */
    if ((r->prot & GM_W)) {
        if (!m->wdir[l1])
            m->wdir[l1] = calloc(GM_L2_ENTRIES, sizeof(uint8_t *));
        if (m->wdir[l1])
            m->wdir[l1][l2] = page;
    }
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

/* ---- the sandbox fd table (see guest.h) --------------------------------
 *
 * A guest fd is an opaque index into g->fdtab; the host fd lives only in the
 * entry, so the guest can never name a host fd it was not granted.  0/1/2 are
 * pre-opened as the console; open/openat allocate a fresh index for the host
 * fd; close frees the index (and closes the real file, but not the console).
 */
#define GUEST_AT_FDCWD (-100)           /* same value on guest and host */

static void
gfd_init(guest *g)
{
    int i;
    if (g->fds_inited)
        return;
    for (i = 0; i < 3; i++) {           /* stdin/stdout/stderr -> console */
        g->fdtab[i].used = 1;
        g->fdtab[i].dev = GDEV_CONSOLE;
        g->fdtab[i].host_fd = i;
    }
    g->fds_inited = 1;
}

/* A guest fd -> its host fd, or -1 if the index is not an open entry. */
static int
gfd_host(guest *g, uint32_t gfd)
{
    if (gfd >= GUEST_MAX_FDS || !g->fdtab[gfd].used)
        return -1;
    return g->fdtab[gfd].host_fd;
}

/* A guest fd -> its table entry, or NULL if the index is not open.  getdents64
 * caches a directory stream (hp_dir) on the entry. */
static struct gfd *
gfd_entry(guest *g, uint32_t gfd)
{
    if (gfd >= GUEST_MAX_FDS || !g->fdtab[gfd].used)
        return 0;
    return &g->fdtab[gfd];
}

/* A guest dirfd -> host dirfd via *out.  AT_FDCWD passes straight through (it
 * names no table entry).  Returns 0, or -1 on a bad index. */
static int
gfd_dir(guest *g, uint32_t dirfd, int *out)
{
    if ((int32_t)dirfd == GUEST_AT_FDCWD) {
        *out = GUEST_AT_FDCWD;
        return 0;
    }
    *out = gfd_host(g, dirfd);
    return *out < 0 ? -1 : 0;
}

/* Record an opened host fd, returning the guest index (-1 if the table full). */
static int
gfd_alloc(guest *g, int dev, int host_fd)
{
    int i;
    for (i = 3; i < GUEST_MAX_FDS; i++)
        if (!g->fdtab[i].used) {
            g->fdtab[i].used = 1;
            g->fdtab[i].dev = dev;
            g->fdtab[i].host_fd = host_fd;
            g->fdtab[i].dirp = 0;       /* a directory stream is opened lazily */
            return i;
        }
    return -1;
}

static int32_t
gfd_close(guest *g, uint32_t gfd)
{
    int hf;
    if (gfd >= GUEST_MAX_FDS || !g->fdtab[gfd].used)
        return -GUEST_EBADF;
    hf = g->fdtab[gfd].host_fd;
    if (g->fdtab[gfd].dirp) {           /* a getdents64 stream owns its own dup fd */
        hp_dirclose((hp_dir *)g->fdtab[gfd].dirp);
        g->fdtab[gfd].dirp = 0;
    }
    g->fdtab[gfd].used = 0;
    if (gfd >= 3)                       /* keep 0/1/2's host fds open */
        close(hf);
    return 0;
}

/* Write one 16-byte fs_mount entry at table slot `slot`:
 *   struct fs_mount { char name[8]; int16 fd; int16 cwd; u32 flags }
 * `fd` is the guest mount fd (a capability fd index, or 0 for a backend with no
 * backing fd); `kind` goes in the low byte of flags. */
static void
write_mount_entry(guest *g, uint32_t tbl, int slot, const char *name,
                  int namelen, int fd, uint32_t kind)
{
    uint32_t ent = tbl + (uint32_t)slot * 16u;
    int k;
    for (k = 0; k < FS_NAME_MAX; k++)
        gm_store(&g->mem, ent + (uint32_t)k, 1, 0,
                 (uint32_t)(k < namelen ? (unsigned char)name[k] : 0));
    gm_store(&g->mem, ent + 8, 2, g->big_endian, (uint32_t)(uint16_t)fd);
    gm_store(&g->mem, ent + 10, 2, g->big_endian, 0xffffu);    /* cwd = root */
    gm_store(&g->mem, ent + 12, 4, g->big_endian, kind);
}

/* Load a host romfs image file into a fresh guest memory region (the "host-
 * shared buffer" use gm_mmap exists for).  On success writes the region base
 * and byte length; returns -1 on any error. */
static int
load_rom_image(guest *g, const char *path, uint32_t *base_out, uint32_t *len_out)
{
    FILE *f = fopen(path, "rb");
    long sz;
    uint32_t base;
    uint8_t *buf;
    size_t got;

    if (!f) {
        fprintf(stderr, "skj-run: cannot open rom image '%s': %s\n", path,
                strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    if (sz == 0 || sz > 0x40000000L) {  /* refuse empty or absurd sizes */
        fprintf(stderr, "skj-run: rom image '%s' has a bad size\n", path);
        fclose(f);
        return -1;
    }
    buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return -1; }
    base = gm_mmap(&g->mem, (uint32_t)sz, 0);
    if (!base) {
        fprintf(stderr, "skj-run: no guest memory for rom image '%s'\n", path);
        free(buf);
        return -1;
    }
    gm_write(&g->mem, base, buf, (uint32_t)sz);
    free(buf);
    if (g->mem.fault)
        return -1;
    *base_out = base;
    *len_out = (uint32_t)sz;
    return 0;
}

/* The launcher: turn each grant into a guest fs_mount entry.  A --mount opens a
 * confined host directory as a hostfs capability fd; a --dev grants a devfs
 * mount with no backing fd; a --rom loads a read-only image into guest memory;
 * a --9p serves a host directory over a shared doorbell channel.  The guest
 * never mounts; it reads this pre-populated table (and fs_mount_aux for the
 * extra romfs/9pfs parameters).  A guest that links no fs_mounts symbol just
 * runs with no mounts (a warning if a grant was nonetheless given). */
void
guest_install_mounts(guest *g, const char *elf_path, const run_opts *o)
{
    uint32_t tbl, cnt_sym, cur_sym, aux_sym;
    int i, installed = 0;
    int ngrants = o->nmounts + o->ndevs + o->nroms + o->np9s;

    if (ngrants == 0)
        return;

    gfd_init(g);                        /* reserve 0/1/2 before any grant fd */

    tbl = elf32_symbol(elf_path, "fs_mounts");
    cnt_sym = elf32_symbol(elf_path, "fs_mount_count");
    cur_sym = elf32_symbol(elf_path, "fs_cur_mount");
    aux_sym = elf32_symbol(elf_path, "fs_mount_aux");
    if (!tbl || !cnt_sym) {
        fprintf(stderr, "skj-run: a mount was given but %s links no fs_mounts "
                "table; ignoring %d grant(s)\n", elf_path, ngrants);
        return;
    }

    for (i = 0; i < o->nmounts && installed < FS_MAX_MOUNTS; i++) {
        const char *spec = o->mounts[i];
        const char *colon = strchr(spec, ':');
        const char *dir;
        int hostfd, idx, nl;

        if (!colon) {
            fprintf(stderr, "skj-run: bad --mount '%s' (want name:hostdir)\n",
                    spec);
            continue;
        }
        nl = (int)(colon - spec);
        if (nl <= 0 || nl > FS_NAME_MAX) {
            fprintf(stderr, "skj-run: mount name in '%s' must be 1..%d bytes\n",
                    spec, FS_NAME_MAX);
            continue;
        }
        dir = colon + 1;
        hostfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (hostfd < 0) {
            fprintf(stderr, "skj-run: cannot mount '%s': %s\n", dir,
                    strerror(errno));
            continue;
        }
        idx = gfd_alloc(g, GDEV_HOSTFS, hostfd);
        if (idx < 0) {
            fprintf(stderr, "skj-run: fd table full mounting '%s'\n", dir);
            close(hostfd);
            continue;
        }
        write_mount_entry(g, tbl, installed, spec, nl, idx, (uint32_t)FS_KIND_HOST);
        installed++;
    }

    for (i = 0; i < o->ndevs && installed < FS_MAX_MOUNTS; i++) {
        const char *name = o->devs[i];
        int nl = (int)strlen(name);
        if (nl <= 0 || nl > FS_NAME_MAX) {
            fprintf(stderr, "skj-run: --dev name '%s' must be 1..%d bytes\n",
                    name, FS_NAME_MAX);
            continue;
        }
        write_mount_entry(g, tbl, installed, name, nl, 0, (uint32_t)FS_KIND_DEV);
        installed++;
    }

    for (i = 0; i < o->nroms && installed < FS_MAX_MOUNTS; i++) {
        const char *spec = o->roms[i];
        const char *colon = strchr(spec, ':');
        uint32_t base, len;
        int nl;

        if (!colon) {
            fprintf(stderr, "skj-run: bad --rom '%s' (want name:image)\n", spec);
            continue;
        }
        nl = (int)(colon - spec);
        if (nl <= 0 || nl > FS_NAME_MAX) {
            fprintf(stderr, "skj-run: rom name in '%s' must be 1..%d bytes\n",
                    spec, FS_NAME_MAX);
            continue;
        }
        if (!aux_sym) {
            fprintf(stderr, "skj-run: --rom given but %s links no fs_mount_aux; "
                    "ignoring '%s'\n", elf_path, spec);
            continue;
        }
        if (load_rom_image(g, colon + 1, &base, &len) != 0)
            continue;
        write_mount_entry(g, tbl, installed, spec, nl, 0, (uint32_t)FS_KIND_ROM);
        gm_store(&g->mem, aux_sym + (uint32_t)installed * 8u, 4,
                 g->big_endian, base);
        gm_store(&g->mem, aux_sym + (uint32_t)installed * 8u + 4u, 4,
                 g->big_endian, len);
        installed++;
    }

    for (i = 0; i < o->np9s && installed < FS_MAX_MOUNTS; i++) {
        const char *spec = o->p9s[i];
        const char *colon = strchr(spec, ':');
        const char *dir;
        struct p9_server *srv;
        uint32_t chan, chansz = 8u + 2u * (uint32_t)P9_MSIZE;
        int hostfd, nl;

        if (!colon) {
            fprintf(stderr, "skj-run: bad --9p '%s' (want name:hostdir)\n", spec);
            continue;
        }
        nl = (int)(colon - spec);
        if (nl <= 0 || nl > FS_NAME_MAX) {
            fprintf(stderr, "skj-run: 9p name in '%s' must be 1..%d bytes\n",
                    spec, FS_NAME_MAX);
            continue;
        }
        if (!aux_sym) {
            fprintf(stderr, "skj-run: --9p given but %s links no fs_mount_aux; "
                    "ignoring '%s'\n", elf_path, spec);
            continue;
        }
        if (g->np9 >= GUEST_MAX_P9) {
            fprintf(stderr, "skj-run: too many 9p channels for '%s'\n", spec);
            continue;
        }
        dir = colon + 1;
        hostfd = open(dir, O_RDONLY | O_DIRECTORY);  /* a clear error if missing */
        if (hostfd < 0) {
            fprintf(stderr, "skj-run: cannot serve 9p '%s': %s\n", dir,
                    strerror(errno));
            continue;
        }
        close(hostfd);
        srv = p9_server_create(dir);
        if (!srv) {
            fprintf(stderr, "skj-run: no memory for 9p server '%s'\n", dir);
            continue;
        }
        chan = gm_mmap(&g->mem, chansz, 0);
        if (!chan) {
            fprintf(stderr, "skj-run: no guest memory for 9p channel '%s'\n", dir);
            p9_server_destroy(srv);
            continue;
        }
        gm_zero(&g->mem, chan, chansz);              /* req_len/rsp_len start 0 */
        write_mount_entry(g, tbl, installed, spec, nl, 0, (uint32_t)FS_KIND_9P);
        gm_store(&g->mem, aux_sym + (uint32_t)installed * 8u, 4,
                 g->big_endian, chan);
        gm_store(&g->mem, aux_sym + (uint32_t)installed * 8u + 4u, 4,
                 g->big_endian, 0);
        g->p9[g->np9].chan = chan;
        g->p9[g->np9].srv = srv;
        g->np9++;
        installed++;
    }

    gm_store(&g->mem, cnt_sym, 4, g->big_endian, (uint32_t)installed);
    if (cur_sym)
        gm_store(&g->mem, cur_sym, 4, g->big_endian, 0);   /* default mount 0 */
    if (g->mem.fault)
        fprintf(stderr, "skj-run: fault writing the mount table into %s\n",
                elf_path);
}

int
guest_p9_doorbell(guest *g, uint32_t chan)
{
    struct p9_server *srv = 0;
    uint8_t req[P9_MSIZE], rsp[P9_MSIZE];
    uint32_t reqlen, rsplen;
    int i, n;

    for (i = 0; i < g->np9; i++)
        if (g->p9[i].chan == chan) {
            srv = (struct p9_server *)g->p9[i].srv;
            break;
        }
    if (!srv)
        return -1;                      /* no channel bound here */

    reqlen = gm_load(&g->mem, chan + 0, 4, g->big_endian);
    if (reqlen > P9_MSIZE)
        reqlen = P9_MSIZE;
    gm_read_out(&g->mem, chan + 8u, req, reqlen);
    n = p9_server_handle(srv, req, reqlen, rsp, sizeof rsp);
    rsplen = n < 0 ? 0 : (uint32_t)n;
    gm_write(&g->mem, chan + 8u + (uint32_t)P9_MSIZE, rsp, rsplen);
    gm_store(&g->mem, chan + 4u, 4, g->big_endian, rsplen);
    return 0;
}

void
guest_p9_free(guest *g)
{
    int i;
    for (i = 0; i < g->np9; i++) {
        p9_server_destroy((struct p9_server *)g->p9[i].srv);
        g->p9[i].srv = 0;
    }
    g->np9 = 0;
}

static int32_t
sys_write(guest *g, uint32_t fd, uint32_t buf, uint32_t len)
{
    uint8_t tmp[4096];
    uint32_t done = 0;
    int hf = gfd_host(g, fd);

    if (hf < 0)
        return -GUEST_EBADF;
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
        n = write(hf, tmp, chunk);
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
    int hf = gfd_host(g, fd);

    if (hf < 0)
        return -GUEST_EBADF;
    if (len > sizeof(tmp))
        len = sizeof(tmp);
    n = read(hf, tmp, len);
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
    e = gfd_alloc(g, GDEV_HOSTFS, fd);
    if (e < 0) {
        close(fd);
        return -GUEST_EBADF;            /* table full */
    }
    return e;
}

/* openat(dirfd, path, flags, mode): the form rv32 (and any asm-generic Linux
 * target) uses, since it has no bare open.  A capability dirfd resolves through
 * hp_open_beneath, confined to the granted tree; AT_FDCWD (-100, the same value
 * on the guest) stays the unconfined ambient path.  The flags and mode are the
 * asm-generic Linux values, which match the host's. */
static int32_t
sys_openat(guest *g, uint32_t dirfd, uint32_t pathp, uint32_t flags,
           uint32_t mode)
{
    char path[1024];
    int32_t e;
    int fd, hd;

    if ((e = copy_path(g, pathp, path, sizeof(path))) != 0)
        return e;
    if (gfd_dir(g, dirfd, &hd) < 0)
        return -GUEST_EBADF;
    fd = (hd == GUEST_AT_FDCWD)         /* ambient: unconfined, as before */
             ? openat(hd, path, (int)flags, (mode_t)mode)
             : hp_open_beneath(hd, path, (int)flags, (int)mode);  /* confined */
    if (fd < 0)
        return -GUEST_ENOENT;
    e = gfd_alloc(g, GDEV_HOSTFS, fd);
    if (e < 0) {
        close(fd);
        return -GUEST_EBADF;
    }
    return e;
}

/* lseek(fd, offset, whence): a 32-bit offset for the ILP32 guests, sign
 * extended to the host off_t.  Returns the new offset, or -errno.  This is the
 * simple form m68k uses; rv32/asm-generic use llseek below. */
static int32_t
sys_lseek(guest *g, uint32_t fd, uint32_t offset, uint32_t whence)
{
    off_t r;
    int hf = gfd_host(g, fd);

    if (hf < 0)
        return -GUEST_EBADF;
    r = lseek(hf, (off_t)(int32_t)offset, (int)whence);
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
    int hf = gfd_host(g, fd);
    off_t r;

    if (hf < 0)
        return -GUEST_EBADF;
    r = lseek(hf, (off_t)off, (int)whence);
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
 * guest reads are filled, at their fixed struct-statx offsets, in the guest's
 * byte order: the 32-bit fields with gm_store's be flag, the 16-bit stx_mode as
 * a halfword so it does not land in __spare0 on a big-endian guest, and the
 * 64-bit fields through store64 (low word placed by endianness). */
static void store64(guest *g, uint32_t addr, uint64_t v);

static int32_t
sys_statx(guest *g, uint32_t dirfd, uint32_t pathp, uint32_t flags,
          uint32_t mask, uint32_t bufp)
{
    int be = g->big_endian;
    char path[1024];
    struct stat hs;
    int r;
    uint32_t i;

    (void)mask;
    if (flags & 0x1000u) {                      /* AT_EMPTY_PATH: fstat the fd */
        int hf = gfd_host(g, dirfd);
        if (hf < 0)
            return -GUEST_EBADF;
        r = fstat(hf, &hs);
    } else {
        int hd;
        int32_t e = copy_path(g, pathp, path, sizeof(path));
        if (e != 0)
            return e;
        if (gfd_dir(g, dirfd, &hd) < 0)
            return -GUEST_EBADF;
        if (hd == GUEST_AT_FDCWD) {
            r = fstatat(hd, path, &hs, 0);          /* ambient: unconfined */
        } else {
            r = hp_stat_beneath(hd, path, &hs);     /* confined to the grant */
        }
    }
    if (r < 0)
        return -GUEST_ENOENT;

    for (i = 0; i < 256; i += 4)                /* zero the struct statx first */
        gm_store(&g->mem, bufp + i, 4, be, 0);
    gm_store(&g->mem, bufp + 0,  4, be, 0x000007ffu);                 /* stx_mask */
    gm_store(&g->mem, bufp + 4,  4, be, (uint32_t)hs.st_blksize);     /* stx_blksize */
    gm_store(&g->mem, bufp + 16, 4, be, (uint32_t)hs.st_nlink);       /* stx_nlink */
    gm_store(&g->mem, bufp + 20, 4, be, (uint32_t)hs.st_uid);         /* stx_uid */
    gm_store(&g->mem, bufp + 24, 4, be, (uint32_t)hs.st_gid);         /* stx_gid */
    gm_store(&g->mem, bufp + 28, 2, be, (uint32_t)(hs.st_mode & 0xffffu)); /* stx_mode (u16) */
    store64(g, bufp + 32, (uint64_t)hs.st_ino);                      /* stx_ino */
    store64(g, bufp + 40, (uint64_t)(int64_t)hs.st_size);            /* stx_size */
    store64(g, bufp + 48, (uint64_t)(int64_t)hs.st_blocks);          /* stx_blocks */
    store64(g, bufp + 64, (uint64_t)(int64_t)hs.st_atime);           /* stx_atime.sec */
    store64(g, bufp + 96, (uint64_t)(int64_t)hs.st_ctime);           /* stx_ctime.sec */
    store64(g, bufp + 112, (uint64_t)(int64_t)hs.st_mtime);          /* stx_mtime.sec */
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
    int hd;

    if ((e = copy_path(g, pathp, path, sizeof(path))) != 0)
        return e;
    if (gfd_dir(g, dirfd, &hd) < 0)
        return -GUEST_EBADF;
    if (hd == GUEST_AT_FDCWD)
        return mkdirat(hd, path, (mode_t)mode) == 0 ? 0 : -GUEST_ENOENT;
    {
        const char *base;
        int pf = hp_parent_beneath(hd, path, &base), rc;
        if (pf < 0)
            return -GUEST_ENOENT;
        rc = mkdirat(pf, base, (mode_t)mode);
        close(pf);
        return rc == 0 ? 0 : -GUEST_ENOENT;
    }
}

static int32_t
sys_unlinkat(guest *g, uint32_t dirfd, uint32_t pathp, uint32_t flags)
{
    char path[1024];
    int32_t e;
    int hd;

    if ((e = copy_path(g, pathp, path, sizeof(path))) != 0)
        return e;
    if (gfd_dir(g, dirfd, &hd) < 0)
        return -GUEST_EBADF;
    if (hd == GUEST_AT_FDCWD)
        return unlinkat(hd, path, (int)flags) == 0 ? 0 : -GUEST_ENOENT;
    {
        const char *base;
        int pf = hp_parent_beneath(hd, path, &base), rc;
        if (pf < 0)
            return -GUEST_ENOENT;
        rc = unlinkat(pf, base, (int)flags);
        close(pf);
        return rc == 0 ? 0 : -GUEST_ENOENT;
    }
}

/* fchmodat(dirfd, path, mode, flags): the guest libc's chmod backs on this, so
   the linker can make its output executable in-guest. */
static int32_t
sys_fchmodat(guest *g, uint32_t dirfd, uint32_t pathp, uint32_t mode,
             uint32_t flags)
{
    char path[1024];
    int32_t e;
    int hd;

    if ((e = copy_path(g, pathp, path, sizeof(path))) != 0)
        return e;
    if (gfd_dir(g, dirfd, &hd) < 0)
        return -GUEST_EBADF;
    if (hd == GUEST_AT_FDCWD)
        return fchmodat(hd, path, (mode_t)mode, (int)flags) == 0
                   ? 0 : -GUEST_ENOENT;
    {
        const char *base;
        int pf = hp_parent_beneath(hd, path, &base), rc;
        if (pf < 0)
            return -GUEST_ENOENT;
        rc = fchmodat(pf, base, (mode_t)mode, (int)flags);
        close(pf);
        return rc == 0 ? 0 : -GUEST_ENOENT;
    }
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
    int ho, hn;

    if (flags != 0)
        return -GUEST_EINVAL;
    if ((e = copy_path(g, oldp, oldpath, sizeof(oldpath))) != 0)
        return e;
    if ((e = copy_path(g, newp, newpath, sizeof(newpath))) != 0)
        return e;
    if (gfd_dir(g, od, &ho) < 0 || gfd_dir(g, nd, &hn) < 0)
        return -GUEST_EBADF;
    if (ho == GUEST_AT_FDCWD && hn == GUEST_AT_FDCWD)
        return renameat(ho, oldpath, hn, newpath) == 0 ? 0 : -GUEST_ENOENT;
    if (ho == GUEST_AT_FDCWD || hn == GUEST_AT_FDCWD)
        return -GUEST_EINVAL;           /* no rename across ambient and a capability */
    {
        /* Confine both ends beneath their capabilities, then rename within the
         * resolved parents.  fs/ only ever renames inside one mount, so both
         * capabilities are the same granted tree. */
        const char *ob, *nb;
        int opf = hp_parent_beneath(ho, oldpath, &ob), npf, rc;
        if (opf < 0)
            return -GUEST_ENOENT;
        npf = hp_parent_beneath(hn, newpath, &nb);
        if (npf < 0) {
            close(opf);
            return -GUEST_ENOENT;
        }
        rc = renameat(opf, ob, npf, nb);
        close(opf);
        close(npf);
        return rc == 0 ? 0 : -GUEST_ENOENT;
    }
}

/* Store a 64-bit value to guest memory honoring the guest's byte order: the low
 * word sits at the low address on a little-endian guest and the high address on
 * a big-endian one, and each 32-bit word is itself byte-ordered to match, so a
 * native 64-bit load reads the value back whole. */
static void
store64(guest *g, uint32_t addr, uint64_t v)
{
    int be = g->big_endian;

    gm_store(&g->mem, addr + (be ? 4 : 0), 4, be, (uint32_t)v);
    gm_store(&g->mem, addr + (be ? 0 : 4), 4, be, (uint32_t)(v >> 32));
}

/* Emit one directory entry into the guest buffer as a struct linux_dirent64:
 * ino and off as 64-bit guest-ordered words, d_reclen = round_up(19 + namelen +
 * 1, 8) as a guest halfword, d_type as a byte, then the NUL-terminated name with
 * the record padded out with zeroes.  Returns the record length.  The 19-byte
 * head is the same on every guest arch, so the layout matches what the guest's
 * linux_dirent64 reader (libc dirent.c, the fs/ backends) expects. */
static uint16_t
emit_dirent64(guest *g, uint32_t dirp, uint32_t gpos, uint64_t ino, uint64_t off,
              uint8_t type, const char *name, int namelen)
{
    uint16_t reclen = (uint16_t)((19 + namelen + 1 + 7) & ~7);
    int pad = reclen - 19 - namelen;    /* NUL + alignment padding */

    store64(g, dirp + gpos + 0, ino);
    store64(g, dirp + gpos + 8, off);
    gm_store(&g->mem, dirp + gpos + 16, 2, g->big_endian, reclen);
    gm_store(&g->mem, dirp + gpos + 18, 1, 0, type);
    gm_write(&g->mem, dirp + gpos + 19, name, (uint32_t)namelen);
    gm_zero(&g->mem, dirp + gpos + 19 + (uint32_t)namelen, (uint32_t)pad);
    return reclen;
}

/* getdents64(fd, dirp, count): read directory entries into the guest buffer as a
 * packed run of struct linux_dirent64 (0 at end of directory, -errno on error).
 *
 * The directory is read through hp_dir, a host directory stream cached on the fd
 * table (opened on the first call, kept across calls).  hp_dirnext yields one
 * entry at a time and hp_dirtell/hp_dirseek rewind the stream when the next entry
 * will not fit, so the guest resumes there on its next call. */
static int32_t
sys_getdents64(guest *g, uint32_t fd, uint32_t dirp, uint32_t count)
{
    struct gfd *ent = gfd_entry(g, fd);
    uint32_t gpos = 0;
    hp_dir *d;

    if (!ent)
        return -GUEST_EBADF;
    d = (hp_dir *)ent->dirp;
    if (!d) {                           /* open the stream on the first call */
        d = hp_diropen(ent->host_fd);
        if (!d)
            return -GUEST_ENOENT;
        ent->dirp = d;
    }
    for (;;) {
        long cookie = hp_dirtell(d);
        uint64_t ino;
        unsigned type;
        const char *name;
        int namelen, r;
        uint16_t reclen;

        r = hp_dirnext(d, &ino, &type, &name);
        if (r < 0)
            return -GUEST_ENOENT;
        if (r == 0)
            break;                      /* end of directory */
        namelen = (int)strlen(name);
        reclen = (uint16_t)((19 + namelen + 1 + 7) & ~7);
        if (gpos + reclen > count) {    /* will not fit: resume here next call */
            hp_dirseek(d, cookie);
            if (gpos == 0)              /* not even one entry fits */
                return -GUEST_EINVAL;
            break;
        }
        emit_dirent64(g, dirp, gpos, ino, (uint64_t)hp_dirtell(d),
                      (uint8_t)type, name, namelen);
        gpos += reclen;
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
    uint32_t sec, nsec;

    if (ts_ptr == 0)
        return -GUEST_EFAULT;
    if (hp_clock(clk_id == 0, &sec, &nsec) != 0)
        return -GUEST_EINVAL;
    gm_store(&g->mem, ts_ptr + 0, 4, g->big_endian, sec);
    gm_store(&g->mem, ts_ptr + 4, 4, g->big_endian, nsec);
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
        int n;

        if (chunk > sizeof tmp)
            chunk = sizeof tmp;
        n = hp_random(tmp, (int)chunk);
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
    uint32_t sec, nsec;

    (void)rem_ptr;
    if (req_ptr == 0)
        return -GUEST_EFAULT;
    sec  = gm_load(&g->mem, req_ptr + 0, 4, g->big_endian);
    nsec = gm_load(&g->mem, req_ptr + 4, 4, g->big_endian);
    if (nsec >= 1000000000u)
        return -GUEST_EINVAL;
    hp_sleep(sec, nsec);
    return 0;
}

int32_t
guest_syscall(guest *g, int sys, uint32_t a0, uint32_t a1, uint32_t a2,
              uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a5;

    g->syscalls++;
    gfd_init(g);                        /* pre-open 0/1/2 on first syscall */

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
        return gfd_close(g, a0);

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

    case GSYS_FCHMODAT:
        return sys_fchmodat(g, a0, a1, a2, a3);

    case GSYS_GETDENTS64:
        return sys_getdents64(g, a0, a1, a2);

    case GSYS_CLOCK_GETTIME:
        return sys_clock_gettime(g, a0, a1);

    case GSYS_GETRANDOM:
        return sys_getrandom(g, a0, a1, a2);

    case GSYS_NANOSLEEP:
        return sys_nanosleep(g, a0, a1);

    case GSYS_SCHED_YIELD:
        hp_yield();
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

static void
poke64(guest *g, uint32_t addr, uint64_t val)
{
    int i;

    for (i = 0; i < 8; i++)             /* the HV guests are little-endian */
        gm_write8(&g->mem, addr + (uint32_t)i, (uint8_t)(val >> (8 * i)));
}

/* The LP64 initial stack for the hardware-virtualization guests: the System V
 * layout with 8-byte slots (argc, argv[], NULL, envp NULL, auxv, AT_NULL).  The
 * returned pointer is 16-byte aligned, which is what the amd64 and aarch64 entry
 * conventions expect at _start. */
uint32_t
guest_build_stack64(guest *g, const run_opts *o)
{
    uint32_t sp = g->stack_top;
    uint32_t str[64] = { 0 };
    int argc = o->argc;
    int i;

    if (argc > (int)(sizeof(str) / sizeof(str[0])))
        argc = (int)(sizeof(str) / sizeof(str[0]));

    for (i = argc - 1; i >= 0; i--) {   /* strings, top down */
        uint32_t len = (uint32_t)strlen(o->argv[i]) + 1;
        sp -= len;
        gm_write(&g->mem, sp, o->argv[i], len);
        str[i] = sp;
    }
    sp &= ~15u;

    /* argc + argv + NULL + envp NULL + two auxv words, 8 bytes each.  Keep the
     * final sp 16-byte aligned by padding the slot count to an even number. */
    {
        uint32_t slots = 1 + (uint32_t)argc + 1 + 1 + 2 + 1;
        sp -= 8 * (slots + (slots & 1));
        sp &= ~15u;
    }

    {
        uint32_t p = sp;
        poke64(g, p, (uint64_t)(uint32_t)argc);
        p += 8;
        for (i = 0; i < argc; i++, p += 8)
            poke64(g, p, str[i]);
        poke64(g, p, 0);                /* argv terminator */
        p += 8;
        poke64(g, p, 0);                /* envp terminator */
        p += 8;
        poke64(g, p, AT_PAGESZ);
        p += 8;
        poke64(g, p, GM_PAGE_SIZE);
        p += 8;
        poke64(g, p, AT_NULL);
        p += 8;
        poke64(g, p, 0);
    }
    return sp;
}
