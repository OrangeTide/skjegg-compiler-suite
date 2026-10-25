/* jit_arena.c : the low-memory mapping primitive and the guest bump arena
   (see jit_arena.h). */

#define _POSIX_C_SOURCE 200809L

#include "jit_arena.h"
#include "target.h"

#include <stdint.h>
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* The runtime arena's size.  Excelsior test programs bump-allocate strings,
   lists, objects, and 4KB coroutine stacks from here; 16MB is ample and still
   leaves the low address space open for the code, data, and execution stack
   regions. */
#define KP_ARENA_LEN (16u << 20)

/* Map a region for the guest code, data, and stack.  Under LP64 a guest address
   is a full 64-bit host pointer, so the mapping can live anywhere.  Otherwise the
   guest is 32-bit and the region must sit below 4GB: on x86-64 that is MAP_32BIT;
   a 32-bit host is low by construction; any other 64-bit host (arm64 cross JIT)
   has neither, so request successive low addresses with MAP_FIXED_NOREPLACE and
   take the first that lands (and stays) below 4GB.  Mirrors rt_host.c:rt_map_low.
   PROT_READ|PROT_WRITE; a caller that wants executable memory mprotects it
   afterward. */
void *
kp_map_low(size_t len)
{
#if defined(KP_LP64) || (SKJ_LP64 && defined(__x86_64__))
    /* LP64 with a backend proven for high addresses (the kobold JIT on any arch,
       and the x86-64 C JIT): guest addresses are full 64-bit host pointers, so
       the region can live anywhere, with no low-memory constraint.  The arm64 C
       JIT is LP64 too but its varargs register-save-area addressing is not yet
       audited for high addresses, so it keeps the low arena below (not the 32-on
       -64 model being retired, just a bounded address range), pending that fix. */
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#elif UINTPTR_MAX <= 0xffffffffULL
    /* A 32-bit host (e.g. rv32): every address already fits the front end's
       32-bit model, so a plain mapping is low by construction. */
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#elif defined(__x86_64__)
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
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

void *
kp_arena_alloc(struct kp_arena *a, int size)
{
    void *p;
    size_t need;

    if (size < 0)
        return NULL;
    if (!a->base) {
        void *m = kp_map_low(KP_ARENA_LEN);
        if (m == MAP_FAILED)
            return NULL;
        a->base = m;
        a->len = KP_ARENA_LEN;
        a->off = 0;
    }
    need = ((size_t)size + 7u) & ~(size_t)7u;   /* keep words 8-byte aligned */
    if (need > a->len - a->off)
        return NULL;                             /* exhausted */
    p = a->base + a->off;
    a->off += need;
    return p;
}

void
kp_arena_free(struct kp_arena *a)
{
    if (a->base)
        munmap(a->base, a->len);
    a->base = NULL;
    a->len = 0;
    a->off = 0;
}
