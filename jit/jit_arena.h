/* jit_arena.h : guest-region mapping and the guest bump arena for the
   in-process JIT.

   Under LP64 (every x86-64 and arm64 JIT) a guest pointer is a full 64-bit host
   pointer, so the JIT's data, code, and stack regions, and the runtime arena a
   guest reaches through __moo_arena_alloc, live anywhere.  On a 32-bit host (the
   rv32/ColdFire cross JITs) every such region is low by construction.
   kp_map_low() gets a region for either model (the name is historical).

   The bump arena backs a guest runtime's strings, lists, and objects when it
   runs under the JIT as guest host code.  A C-only program (skj-jit) never
   allocates from it, so the region is mapped lazily on the first allocation
   and stays unmapped otherwise. */

#ifndef JIT_ARENA_H
#define JIT_ARENA_H

#include <stddef.h>
#include <stdint.h>

/* Map len bytes read-write for the guest model: anywhere under LP64, low by
   construction on a 32-bit host.  Returns MAP_FAILED on failure, so a caller
   compares against MAP_FAILED (from <sys/mman.h>). */
void *kp_map_low(size_t len);

/* A bump allocator over one mapped region.  Zero-initialized means empty
   and unmapped; the first kp_arena_alloc maps the region. */
struct kp_arena {
    uint8_t *base;              /* the mapped region, or NULL if unmapped */
    size_t len;                 /* its mapped length */
    size_t off;                 /* bytes handed out so far */
};

/* Hand out size bytes (8-byte aligned) from a, mapping the region on first
   use.  Returns a pointer into the guest region, or NULL if the mapping fails
   or the arena is exhausted. */
void *kp_arena_alloc(struct kp_arena *a, int size);

/* Release the arena's region (a no-op if never mapped) and reset a. */
void kp_arena_free(struct kp_arena *a);

#endif /* JIT_ARENA_H */
