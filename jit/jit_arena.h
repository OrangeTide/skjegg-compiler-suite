/* jit_arena.h : low-memory mapping and the guest bump arena for the
   in-process JIT.

   Every guest pointer must fit the front end's 32-bit address model, so the
   JIT's data, code, and stack regions, and the runtime arena a guest reaches
   through __moo_arena_alloc, all live below 4GB.  kp_map_low() is the single
   primitive that gets such a region.

   The bump arena backs the Excelsior runtime (libexc's strings, lists,
   objects, and coroutine stacks) when it runs under the JIT as guest-32 host
   code.  A C-only program (skj-jit) never allocates from it, so the region is
   mapped lazily on the first allocation and stays unmapped otherwise. */

#ifndef JIT_ARENA_H
#define JIT_ARENA_H

#include <stddef.h>
#include <stdint.h>

/* Map len bytes read-write below 4GB.  Returns MAP_FAILED on failure, so a
   caller compares against MAP_FAILED (from <sys/mman.h>). */
void *kp_map_low(size_t len);

/* A bump allocator over one low-memory region.  Zero-initialized means empty
   and unmapped; the first kp_arena_alloc maps the region. */
struct kp_arena {
    uint8_t *base;              /* the low-memory region, or NULL if unmapped */
    size_t len;                 /* its mapped length */
    size_t off;                 /* bytes handed out so far */
};

/* Hand out size bytes (8-byte aligned) from a, mapping the region on first
   use.  Returns a low (< 4GB) pointer, or NULL if the mapping fails or the
   arena is exhausted. */
void *kp_arena_alloc(struct kp_arena *a, int size);

/* Release the arena's region (a no-op if never mapped) and reset a. */
void kp_arena_free(struct kp_arena *a);

#endif /* JIT_ARENA_H */
