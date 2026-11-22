/* The moo/basic bump arena for the WASM target.

   The native targets define __moo_arena_alloc and __moo_arena_reset in their crt
   (start.S) over a fixed static buffer.  WASM has no crt arena, so provide the
   same bump allocator here over __libc_sbrk, which skj-ld-wasm synthesizes.
   Allocations are 8-aligned and never individually freed; reset drops the whole
   arena back to its base.  __libc_sbrk is contiguous and, in a program that
   links only a front-end runtime (no malloc), the arena owns it, so the grown
   region stays one contiguous span that reset can reuse from the start.

   This is the per-arena shared seam the moo string/list runtime and the BASIC
   runtime both lower allocations to, so any WASM front end links this one
   object. */

extern void *__libc_sbrk(int incr);

static char *arena_base;
static char *arena_ptr;
static char *arena_end;

void *
__moo_arena_alloc(int size)
{
    char *r;

    size = (size + 7) & ~7;
    if (arena_ptr == 0) {
        arena_base = __libc_sbrk(0);    /* the current break is the arena base */
        arena_ptr = arena_base;
        arena_end = arena_base;
    }
    if (arena_ptr + size > arena_end) {
        int need = (int)(arena_ptr + size - arena_end);
        int chunk = need < 65536 ? 65536 : need;
        __libc_sbrk(chunk);             /* grow, contiguous with arena_end */
        arena_end += chunk;
    }
    r = arena_ptr;
    arena_ptr += size;
    return r;
}

void
__moo_arena_reset(void)
{
    arena_ptr = arena_base;
}
