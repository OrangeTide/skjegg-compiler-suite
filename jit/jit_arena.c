/* jit_arena.c : the low-memory mapping primitive and the guest bump arena
   (see jit_arena.h). */

#define _POSIX_C_SOURCE 200809L

#include "jit_arena.h"

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

/* Map a region in low memory so a guest pointer fits the front end's 32-bit
   address model.  On x86-64 that is MAP_32BIT; elsewhere there is no such flag,
   so request successive low addresses with MAP_FIXED_NOREPLACE and take the
   first that lands (and stays) below 4GB.  PROT_READ|PROT_WRITE; a caller that
   wants executable memory mprotects it afterward. */
void *
kp_map_low(size_t len)
{
#if UINTPTR_MAX <= 0xffffffffULL
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
