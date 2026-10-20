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

/* write and _exit reach the host over the unified syscall seam (the same
   POSIX-named wrappers the C libc links; runtime/io_free_*.S here). */
int write(int fd, const void *buf, int len);
void _exit(int code) __attribute__((noreturn));

/* Ask the host for a symbolized backtrace before a fatal exit.  The crt
   provides it: the skj-run RV_SYS_FATAL ecall on RISC-V, a no-op elsewhere
   (and a no-op off skj-run, where the ecall is -ENOSYS), so trap_die then falls
   through to its own _exit. */
void __skj_trap_backtrace(int code);

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
    write(2, msg, n);
    write(2, "\n", 1);
    __skj_trap_backtrace(70);           /* under skj-run: a symbolized stack */
    _exit(70);
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

/* The language fatals: abort() ends the program, assert(cond) ends it unless
   cond holds.  Both route through trap_die, so under skj-run they print the
   same symbolized backtrace the arithmetic and index traps do. */
void kp_abort(void) { trap_die("skj-kp: aborted"); }
void kp_assert(uint32_t cond) { if (!cond) trap_die("skj-kp: assertion failed"); }

/****************************************************************
 * Guest heap: the shared coalescing allocator over the host-provided region
 *
 * The allocator itself is gheap_core.h, shared with the hosted platform layer
 * (rt_host.c); only the memory source differs here.  This tier's source is a
 * flat region [base, base + size) the host hands in through kp_rt_init, bumped
 * on demand and never grown, so GH_SBRK just advances a cursor in it.
 ****************************************************************/

static uint8_t *g_base;         /* region base, set by kp_rt_init */
static uint32_t g_cap;          /* region size in bytes */
static uint32_t g_brk;          /* bytes handed out from the region */

/* Install the guest heap region.  The host calls this once at entry with a region
   below 4 GB (so a block address fits the 32-bit guest address model).  base must
   be 16-aligned.  Until it is called the heap is empty and any allocation traps. */
void
kp_rt_init(uint32_t base, uint32_t size)
{
    g_base = (uint8_t *)(uintptr_t)base;
    g_cap = size;
    g_brk = 0;
}

/* The core's memory source: grow the break by `incr`, return the old break, or
   (uint8_t *)-1 if the region (or its absence) cannot satisfy it. */
static uint8_t *
gh_sbrk(long incr)
{
    uint8_t *old;

    if (g_base == NULL)
        return (uint8_t *)-1;
    old = g_base + g_brk;
    if (incr < 0) {
        if ((uint32_t)(-incr) > g_brk)
            return (uint8_t *)-1;
        g_brk -= (uint32_t)(-incr);
    } else {
        if (g_brk + (uint32_t)incr > g_cap || g_brk + (uint32_t)incr < g_brk)
            return (uint8_t *)-1;
        g_brk += (uint32_t)incr;
    }
    return old;
}

#define GH_SBRK(incr) gh_sbrk((long)(incr))
#include "gheap_core.h"

/* The guest-address wrappers rt_glue.h declares: convert a native payload
   pointer to and from a 32-bit guest address, and turn exhaustion into a fatal
   trap (the guest has no way to recover). */
uint32_t
gheap_alloc(uint32_t nbytes)
{
    void *p = gh_alloc(nbytes);

    if (p == NULL && nbytes != 0)
        rt_trap(0);             /* heap exhausted (or not installed): fatal */
    return (uint32_t)(uintptr_t)p;
}

void
gheap_free(uint32_t addr)
{
    gh_free((void *)(uintptr_t)addr);
}

uint32_t
gheap_realloc(uint32_t addr, uint32_t nbytes)
{
    void *p = gh_realloc((void *)(uintptr_t)addr, nbytes);

    if (p == NULL && nbytes != 0)
        rt_trap(0);
    return (uint32_t)(uintptr_t)p;
}

/* Live guest-heap bytes (allocated, not freed); a guest leak check diffs it. */
uint32_t
kp_gheap_live(void)
{
    return gh_live();
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
