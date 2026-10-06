/* obj.h : the guest-owned managed-object model shared by the string, dynamic
   array, and coroutine runtimes.

   A guest-owned managed value (a string, a dynamic array, or a coroutine) is a
   gheap object whose first two words are a reference count and a kind tag. The
   value the front end passes around is the object's guest address, which the
   guest heap keeps 16-aligned, so its low bit is 0. That makes it disjoint from a
   host-handle value, a table id tagged with low bit 1 (see handle.c), and from
   nil, which is 0. kp_retain and kp_release route on the low bit: a table id to
   the handle table, a gheap object to the per-kind finalizer the VM holds.

   Only guest-owned data lives in a gheap object. A host opaque handle stays a
   table id so its host pointer and method table never sit in guest-addressable
   memory. */

#ifndef KP_OBJ_H
#define KP_OBJ_H

#include <stdint.h>

enum kp_obj_kind {
    KP_OBJ_STRING = 1,
    KP_OBJ_ARRAY  = 2,
    KP_OBJ_CORO   = 3,
    KP_OBJ_BUFFER = 4,          /* a host-backed reference array: same layout as an array, but
                                   its element backing is host-owned, so its finalizer frees the
                                   descriptor and never the backing */
    KP_OBJ_MAP    = 5,          /* a string-keyed associative array (map of T) */
    KP_OBJ_MAPITER = 6,         /* a cursor over a map, produced by MapFirst, advanced by MapNext */
    KP_OBJ_NKINDS = 7,          /* one past the last, the finalizer table's length */
};

struct kp_obj_hdr {
    uint32_t refcount;
    uint32_t kind;
};

/* A saturated reference count marks a static object: one laid out in a module's data image at
   compile time (a string literal or constant) rather than allocated on the guest heap. retain
   and release leave it untouched, so it is never freed and lives for the program. A real heap
   object never reaches this count. Strings are immutable, so a static string needs only this
   no-free guarantee to be shared safely (doc/kp-units.md, decision log 2026-09-30). */
#define KP_RC_STATIC 0xFFFFFFFFu

/* A managed value with low bit 1 is a host-handle table id; low bit 0 (and
   nonzero) is a gheap-object address; 0 is nil. */
#define KP_VAL_IS_TABLE(v) (((v) & 1u) != 0u)

#endif /* KP_OBJ_H */
