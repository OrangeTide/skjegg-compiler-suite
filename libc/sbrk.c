#include <stddef.h>
#include <stdint.h>

/* The memory seam for the allocator.  __libc_sbrk grows or shrinks the program
   break by `incr` bytes and returns the old break, or (void *)-1 if the request
   would leave the heap.  This is the portable "fixed static region" backing
   (doc/tools/libc.md Phase B): the heap is a reserved .bss array the linker places in
   the low-address image, which every target and host shares without a syscall.
   A VM host, or the qemu/skj-run tier wanting a growable heap, can replace this
   one file with a real brk without touching the allocator above it. */

#ifndef __LIBC_HEAP_SIZE
#define __LIBC_HEAP_SIZE (1024 * 1024)      /* 1 MiB default */
#endif

/* An array of the widest scalar, so the region base is 8-byte aligned (the
   allocator needs no stronger alignment, long double is not used). */
static long long heap_store[(__LIBC_HEAP_SIZE + sizeof(long long) - 1)
                            / sizeof(long long)];

/* The current break.  Initialized lazily to the region base on first use
   rather than with a static initializer, since skj-cc does not yet emit a
   static pointer whose initializer is another object's address. */
static char *heap_brk;

void *
__libc_sbrk(long incr)
{
    /* Round the region base up to 8 bytes: the array's symbol alignment is not
       guaranteed to be 8 on every target, and the allocator hands out memory
       at the base, so a double or long long there must still be aligned. */
    uintptr_t raw = (uintptr_t)(char *)heap_store;
    char *base = (char *)((raw + 7u) & ~(uintptr_t)7u);
    char *limit = (char *)heap_store + sizeof heap_store;
    char *old, *next;

    if (heap_brk == 0)
        heap_brk = base;
    old = heap_brk;
    next = old + incr;

    if (next < base || next > limit)
        return (void *)-1;
    heap_brk = next;
    return old;
}
