/* rt_host.c : the platform layer that lets the Kobold guest-owned object
   runtimes (str_core.c, later dynarr.c and map.c) run under the Skjegg
   in-process JIT.

   It supplies the freestanding primitives rt_glue.h expects: a guest heap, the
   reference counter, the finalizer registry, and a trap.  The JIT maps guest
   data in the low 2 GB and models a guest address as that real address
   truncated to 32 bits, so here a guest address is a real pointer with its high
   bits zero (identity addressing, RT_PTR(a) == (void *)(uintptr_t)a).  The heap
   therefore lives in a low mapping of its own so every block address fits the
   32 bits the guest uses.

   This mirrors what rt_sandbox.c does for the rv32 sandbox, but over a real
   mmap with libc available, rather than a brk heap with none. */

#define _GNU_SOURCE
#include "obj.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/****************************************************************
 * The guest heap
 *
 * A segregated-free-list allocator, the same design as src/gheap.c, but over a
 * flat low-memory region with identity addressing and no vm.  Each block carries
 * a 16-byte header so the payload stays 16-aligned; free blocks link into an
 * exact-size bin, or a single large list reused by first fit with a split.
 ****************************************************************/

struct block {
    uint32_t      size;         /* total bytes including this header, a multiple of ALIGN */
    uint32_t      is_free;      /* 1 while on a free list, for a double-free guard */
    struct block *next;         /* free-list link when free */
};

#define ALIGN       16u
#define HDR         (((uint32_t)sizeof(struct block) + ALIGN - 1u) & ~(ALIGN - 1u))
#define GHEAP_NBINS 512u                        /* bins cover total sizes up to 8 KiB */
#define GHEAP_CAP   ((size_t)256u << 20)        /* 256 MiB reservation, committed on demand */

static uint8_t      *g_base;
static size_t        g_cap;
static size_t        g_brk;
static size_t        g_live;         /* bytes in allocated (not freed) blocks */
static struct block *g_bins[GHEAP_NBINS];
static struct block *g_large;

static void rt_oom(const char *what)
{
    fprintf(stderr, "skj-kp-jit: runtime: %s\n", what);
    exit(70);
}

/* Map `len` bytes of read/write memory below 4 GB, so an address fits the 32-bit
   guest model.  x86-64 has MAP_32BIT; a 32-bit host is low by construction; any
   other 64-bit host (arm64) has neither, so probe fixed low hints until one lands
   below 4 GB and stays there.  Mirrors jit/jit_arena.c:kp_map_low, inlined here to
   keep this runtime independent of the JIT library. */
static void *rt_map_low(size_t len)
{
#if defined(MAP_32BIT)
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#elif UINTPTR_MAX <= 0xffffffffULL
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    static uintptr_t hint = 0x10000000;
    while (hint + len <= 0x100000000ULL) {
        uintptr_t at = hint;
        void *p = mmap((void *)at, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        hint += (len + 0xffffffUL) & ~(uintptr_t)0xffffff;   /* advance 16MB */
        if (p != MAP_FAILED && (uintptr_t)p == at)
            return p;
        if (p != MAP_FAILED)
            munmap(p, len);
    }
    return MAP_FAILED;
#endif
}

/* Reserve the heap region in the low 2 GB on first use, so a block address fits
   the 32-bit guest address model. */
static void gheap_init(void)
{
    void *p;

    if (g_base)
        return;
    p = rt_map_low(GHEAP_CAP);
    if (p == MAP_FAILED)
        rt_oom("guest heap mmap failed");
    g_base = p;
    g_cap = GHEAP_CAP;
    g_brk = 0;
}

static uint32_t round_up(uint32_t n, uint32_t a)
{
    return (n + a - 1u) / a * a;
}

static void push_free(struct block *b)
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

static uint32_t payload_addr(struct block *b)
{
    return (uint32_t)(uintptr_t)((uint8_t *)b + HDR);
}

static struct block *block_of(uint32_t addr)
{
    return (struct block *)((uintptr_t)addr - HDR);
}

uint32_t gheap_alloc(uint32_t nbytes)
{
    struct block *b, *prev, *rem;
    uint32_t need, idx;

    if (nbytes == 0)
        return 0;
    gheap_init();
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

    if (g_brk + need > g_cap || g_brk + need < g_brk)
        rt_oom("guest heap exhausted");
    b = (struct block *)(g_base + g_brk);
    g_brk += need;
    b->size = need;
    b->is_free = 0;
    b->next = NULL;
    g_live += b->size;
    return payload_addr(b);
}

void gheap_free(uint32_t addr)
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

/* Live guest-heap bytes (allocated, not freed).  A guest leak check snapshots
   this around a scope and diffs it, so only symmetric alloc/free matters. */
uint32_t kp_gheap_live(void)
{
    return (uint32_t)g_live;
}

uint32_t gheap_realloc(uint32_t addr, uint32_t nbytes)
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
 * Reference counting and finalizers
 ****************************************************************/

static void (*g_final[KP_OBJ_NKINDS])(uint32_t addr);

void rt_register_final(uint32_t kind, void (*fn)(uint32_t addr))
{
    if (kind < KP_OBJ_NKINDS)
        g_final[kind] = fn;
}

/* A managed value is nil (0), a gheap object (low bit 0), or a handle-table id
   (low bit 1).  A gheap object is reference-counted through its header here; a
   table id (currently a coroutine, KP_H_CONT) routes to the handle table in
   co_shim.c.  Host opaque handles will route the same way once ported. */
void co_table_retain(uint32_t id);
void co_table_release(uint32_t id);

void kp_retain(uint32_t v)
{
    struct kp_obj_hdr *h;

    if (v == 0)
        return;
    if (KP_VAL_IS_TABLE(v)) {
        co_table_retain(v);
        return;
    }
    h = (struct kp_obj_hdr *)(uintptr_t)v;
    if (h->refcount > 0 && h->refcount != KP_RC_STATIC)
        h->refcount++;
}

void kp_release(uint32_t v)
{
    struct kp_obj_hdr *h;
    void (*fin)(uint32_t);

    if (v == 0)
        return;
    if (KP_VAL_IS_TABLE(v)) {
        co_table_release(v);
        return;
    }
    h = (struct kp_obj_hdr *)(uintptr_t)v;
    if (h->refcount == 0 || h->refcount == KP_RC_STATIC)
        return;
    if (--h->refcount > 0)
        return;
    fin = (h->kind < KP_OBJ_NKINDS) ? g_final[h->kind] : NULL;
    if (fin)
        fin(v);                 /* the reclaimer frees the block and any storage it owns */
}

/****************************************************************
 * Misc primitives rt_glue.h routes through the platform
 ****************************************************************/

int rt_memcmp(const void *a, const void *b, size_t n)
{
    return memcmp(a, b, (size_t)n);
}

void rt_trap(uint32_t kind)
{
    (void)kind;
    rt_oom("runtime trap (index out of bounds)");
}
