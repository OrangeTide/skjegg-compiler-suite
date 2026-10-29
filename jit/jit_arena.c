/* jit_arena.c : the low-memory mapping primitive and the guest bump arena
   (see jit_arena.h). */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE                /* expose MAP_ANON under the Darwin API */
#else
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE                 /* glibc gates MAP_ANONYMOUS behind this */
#endif

#include "jit_arena.h"
#include "target.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* macOS spells the anonymous-mapping flag MAP_ANON and gates the MAP_ANONYMOUS
   alias behind _DARWIN_C_SOURCE, which strict _POSIX_C_SOURCE turns off. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

/* The runtime arena's size.  Excelsior test programs bump-allocate strings,
   lists, objects, and 4KB coroutine stacks from here; 16MB is ample and still
   leaves the low address space open for the code, data, and execution stack
   regions. */
#define KP_ARENA_LEN (16u << 20)

/* Map a region for the guest code, data, and stack.  Under LP64 a guest address
   is a full 64-bit host pointer, so the mapping can live anywhere.  On a 32-bit
   host (rv32/ColdFire) a plain mapping is low by construction.  The name is
   historical: there is no longer a low-memory constraint on any LP64 backend.
   PROT_READ|PROT_WRITE; a caller that wants executable memory mprotects it
   afterward.

   SKJ_JIT_MAP_HIGH (LP64 only) forces the mapping to a fixed high base so a test
   can exercise guest addresses above 4GB deterministically, independent of where
   the kernel or an emulator would otherwise place an anonymous mapping; see the
   test-jit-arm64-high target. */
void *
kp_map_low(size_t len)
{
#if SKJ_LP64
    const char *hi = getenv("SKJ_JIT_MAP_HIGH");
    if (hi && *hi) {
        uintptr_t base = 0x400000000000ULL;
        while (base + len <= 0x600000000000ULL) {
            void *p = mmap((void *)base, len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                           -1, 0);
            base += (len + 0xffffffUL) & ~(uintptr_t)0xffffff;   /* advance 16MB */
            if (p != MAP_FAILED && (uintptr_t)p >= 0x400000000000ULL)
                return p;
            if (p != MAP_FAILED)
                munmap(p, len);
        }
        return MAP_FAILED;
    }
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    /* A 32-bit host (rv32/ColdFire): every address already fits the front end's
       32-bit model, so a plain mapping is low by construction. */
    return mmap(NULL, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
