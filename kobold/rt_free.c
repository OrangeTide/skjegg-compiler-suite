/* rt_free.c : the freestanding platform for a Kobold guest with no host OS.

   It is the re-bottoming of rt_host.c for a downstream host (boris and the like)
   whose seam to the guest is only write, read, and exit, with the guest heap
   handed in at entry rather than mapped by the runtime.  The guest-owned object
   runtimes (str_core.c, dynarr.c, map.c) are the same source the hosted JIT
   compiles, built here against rt_glue.h with KP_FREESTANDING; this file supplies
   what that glue needs with no libc: the guest heap, the reference counter and its
   per-kind finalizer registry, the freestanding memory primitives, and the trap.

   The heap is a binned allocator, the same design as rt_host.c, but over a region
   the host hands in through kp_rt_init instead of an mmap of its own.  A guest
   address is a real pointer (flat address space, identity addressing), so a block
   address is the pointer truncated to 32 bits; the host arranges that the region
   sits below 4 GB so that truncation is lossless.

   This source is target-neutral C; it issues no syscall itself.  write and exit,
   which the trap needs, come from the crt (io_free.c / kp_crt_rv.S). */

#include "obj.h"
#include "kp_abi.h"

#include <stdint.h>
#include <stddef.h>

/* The entry-ABI version marker (kp_abi.h).  The crt references kp_guest_abi_v<N>,
   so an archive of one version linked against a crt of another fails to link; the
   plain value is there for a host that prefers to read it. */
#define KP_ABI_CAT2(a, b) a##b
#define KP_ABI_CAT(a, b) KP_ABI_CAT2(a, b)
const uint32_t KP_ABI_CAT(kp_guest_abi_v, KP_GUEST_ABI_VERSION) = KP_GUEST_ABI_VERSION;
const uint32_t kp_guest_abi_version = KP_GUEST_ABI_VERSION;

/* write and exit reach the host; the crt provides them over the seam. */
long kp_sys_write(long fd, const void *buf, long len);
void kp_sys_exit(int code) __attribute__((noreturn));

/****************************************************************
 * Freestanding memory primitives (the glue's memcpy/memset/rt_memcmp)
 ****************************************************************/

void *
memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *
memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    /* Overlap-safe: copy back to front when the destination is above the source,
       so an overlapping move (a dynamic-array Delete or Insert) is correct. */
    if (d <= s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int
rt_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;

    while (n--) {
        if (*x != *y)
            return *x < *y ? -1 : 1;
        x++, y++;
    }
    return 0;
}

/****************************************************************
 * Traps
 *
 * Every runtime fault on this tier is fatal: the trap writes a one-line
 * diagnostic to standard error and exits 70, the convention the hosted AOT crt
 * uses.  There is no recovery and no landing pad.
 ****************************************************************/

static void
trap_die(const char *msg)
{
    const char *p = msg;
    long n = 0;

    while (p[n])
        n++;
    kp_sys_write(2, msg, n);
    kp_sys_write(2, "\n", 1);
    kp_sys_exit(70);
}

/* The index/key/concurrent-modification trap the shared string and map cores
   raise through rt_glue.h's RT_TRAP_*.  KP_TRAP_INDEX is their only kind. */
void
rt_trap(uint32_t kind)
{
    (void)kind;
    trap_die("skj-kp: runtime fault: index out of bounds");
}

/* The guarded-arithmetic traps the backend emits (backend/rv_select.c). */
void kp_trap_div_zero(void) { trap_die("skj-kp: runtime fault: divide by zero"); }
void kp_trap_overflow(void) { trap_die("skj-kp: runtime fault: integer overflow"); }
void kp_trap_index(void)    { trap_die("skj-kp: runtime fault: array index out of bounds"); }

/****************************************************************
 * Guest heap: a binned allocator over the host-provided region
 *
 * The same segregated-free-list design as rt_host.c and the upstream sandbox:
 * each block carries a 16-byte header so the payload stays 16-aligned; a freed
 * block files into an exact-size bin (an O(1) pop with no scan), or a single
 * large list reused by first fit with a split.  The only difference from the
 * hosted allocator is the memory source: a flat region [base, base + size) the
 * host hands in through kp_rt_init, bumped on demand, never grown.
 ****************************************************************/

struct block {
    uint32_t      size;         /* total bytes including this header, a multiple of ALIGN */
    uint32_t      is_free;      /* 1 while on a free list, a double-free guard */
    struct block *next;         /* free-list link when free */
};

#define ALIGN       16u
#define HDR         (((uint32_t)sizeof(struct block) + ALIGN - 1u) & ~(ALIGN - 1u))
#define GHEAP_NBINS 512u                        /* bins cover total sizes up to 8 KiB */

static uint8_t      *g_base;    /* region base, set by kp_rt_init */
static uint32_t      g_cap;     /* region size in bytes */
static uint32_t      g_brk;     /* bytes handed out from the region */
static uint32_t      g_live;    /* bytes in allocated (not freed) blocks */
static struct block *g_bins[GHEAP_NBINS];
static struct block *g_large;

/* Install the guest heap region.  The host calls this once at entry with a region
   below 4 GB (so a block address fits the 32-bit guest address model).  base must
   be 16-aligned.  Until it is called the heap is empty and any allocation traps. */
void
kp_rt_init(uint32_t base, uint32_t size)
{
    g_base = (uint8_t *)(uintptr_t)base;
    g_cap = size;
    g_brk = 0;
    g_live = 0;
}

static uint32_t
round_up(uint32_t n, uint32_t a)
{
    return (n + a - 1u) / a * a;
}

static void
push_free(struct block *b)
{
    uint32_t idx = b->size / ALIGN;

    b->is_free = 1;
    if (idx < GHEAP_NBINS) {
        b->next = g_bins[idx];
        g_bins[idx] = b;
    } else {
        b->next = g_large;
        g_large = b;
    }
}

static uint32_t
payload_addr(struct block *b)
{
    return (uint32_t)(uintptr_t)((uint8_t *)b + HDR);
}

static struct block *
block_of(uint32_t addr)
{
    return (struct block *)((uintptr_t)addr - HDR);
}

uint32_t
gheap_alloc(uint32_t nbytes)
{
    struct block *b, *prev, *rem;
    uint32_t need, idx;

    if (nbytes == 0)
        return 0;
    need = round_up(nbytes + HDR, ALIGN);
    idx = need / ALIGN;

    if (idx < GHEAP_NBINS) {
        b = g_bins[idx];
        if (b) {
            g_bins[idx] = b->next;
            b->is_free = 0;
            b->next = NULL;
            g_live += b->size;
            return payload_addr(b);
        }
    } else {
        prev = NULL;
        for (b = g_large; b; prev = b, b = b->next) {
            if (b->size < need)
                continue;
            if (prev)
                prev->next = b->next;
            else
                g_large = b->next;
            if (b->size - need >= HDR + ALIGN) {
                rem = (struct block *)((uint8_t *)b + need);
                rem->size = b->size - need;
                b->size = need;
                push_free(rem);
            }
            b->is_free = 0;
            b->next = NULL;
            g_live += b->size;
            return payload_addr(b);
        }
    }

    if (g_base == NULL || g_brk + need > g_cap || g_brk + need < g_brk)
        rt_trap(0);             /* heap exhausted (or not installed): fatal */
    b = (struct block *)(g_base + g_brk);
    g_brk += need;
    b->size = need;
    b->is_free = 0;
    b->next = NULL;
    g_live += b->size;
    return payload_addr(b);
}

void
gheap_free(uint32_t addr)
{
    struct block *b;

    if (addr == 0)
        return;
    b = block_of(addr);
    if (b->is_free)
        return;                 /* ignore a double free */
    g_live -= b->size;
    push_free(b);
}

/* Live guest-heap bytes (allocated, not freed); a guest leak check diffs it. */
uint32_t
kp_gheap_live(void)
{
    return g_live;
}

uint32_t
gheap_realloc(uint32_t addr, uint32_t nbytes)
{
    struct block *b;
    uint32_t need, oldpayload, na, copy;

    if (addr == 0)
        return gheap_alloc(nbytes);
    if (nbytes == 0) {
        gheap_free(addr);
        return 0;
    }
    b = block_of(addr);
    need = round_up(nbytes + HDR, ALIGN);
    if (need <= b->size)
        return addr;            /* fits in place */
    oldpayload = b->size - HDR;
    na = gheap_alloc(nbytes);
    copy = (oldpayload < nbytes) ? oldpayload : nbytes;
    memcpy((void *)(uintptr_t)na, (void *)(uintptr_t)addr, copy);
    gheap_free(addr);
    return na;
}

/****************************************************************
 * Reference counting and the per-kind finalizer registry
 *
 * Every managed value is a gheap object with a {refcount, kind} header (obj.h);
 * nil is address 0.  A release at zero runs the reclaimer the object's kind
 * registered.  Unlike the hosted VM there is no handle table: a coroutine on this
 * tier is an ordinary gheap object, counted here like any other.
 ****************************************************************/

static void (*g_final[KP_OBJ_NKINDS])(uint32_t addr);

void
rt_register_final(uint32_t kind, void (*fn)(uint32_t addr))
{
    if (kind < KP_OBJ_NKINDS)
        g_final[kind] = fn;
}

void
kp_retain(uint32_t v)
{
    struct kp_obj_hdr *h;

    if (v == 0)
        return;
    h = (struct kp_obj_hdr *)(uintptr_t)v;
    if (h->refcount > 0 && h->refcount != KP_RC_STATIC)
        h->refcount++;
}

void
kp_release(uint32_t v)
{
    struct kp_obj_hdr *h;
    void (*fin)(uint32_t);

    if (v == 0)
        return;
    h = (struct kp_obj_hdr *)(uintptr_t)v;
    if (h->refcount == 0 || h->refcount == KP_RC_STATIC)
        return;
    if (--h->refcount > 0)
        return;
    fin = (h->kind < KP_OBJ_NKINDS) ? g_final[h->kind] : NULL;
    if (fin)
        fin(v);
}
