/* libexc.h : the seam between libexc (the portable Excelsior guest
 * runtime, layer 1 of excelsior/host-abi.md) and a host binding
 * (layer 2). A binding implements the __exh_* calls and the entry
 * driver; libexc implements everything the compiler emits calls to.
 * The native binding is runtime/exc_native.c.
 *
 */
#ifndef LIBEXC_H
#define LIBEXC_H

/* Two builds of this runtime.
 *
 * The ordinary build targets a 32-bit guest directly (ColdFire, RISC-V
 * RV32): the toolchain's `long` is already four bytes, a pointer is four
 * bytes, and the shared object model, `word` values and pointer fields all
 * match the guest with no help.
 *
 * The EXC_GUEST32 build runs this same runtime as LP64 *host* code over a
 * 32-bit guest object model (the in-process JIT, jit/exc_main.c). There a
 * guest value is four bytes and a guest pointer is a 32-bit address in the
 * host's low memory. Every shared-struct field is held at guest width, and
 * a stored pointer is recovered by zero-extension because the guest arena
 * lives below 4GB. EXC_G() reads a guest pointer field into a host pointer,
 * EXC_TOG() narrows a host pointer into a guest field, and EXC_PTRFIELD()
 * declares a pointer field at guest width. All three are identities in the
 * ordinary build, so its layout and code are byte-for-byte unchanged. */
#ifdef EXC_GUEST32
#include <stdint.h>
typedef int32_t word;
typedef uint32_t exc_gptr;
#define EXC_G(w, T)          ((T)(uintptr_t)(uint32_t)(w))
#define EXC_TOG(p)           ((exc_gptr)(uintptr_t)(p))
#define EXC_PTRFIELD(T, nm)  exc_gptr nm
#else
typedef long word;
typedef void *exc_gptr;
#define EXC_G(w, T)          ((T)(w))
#define EXC_TOG(p)           (p)
#define EXC_PTRFIELD(T, nm)  T nm
#endif

/* Field-access helpers for the shared structs, so a site reads the same in
 * both builds. */
#define EXC_SDATA(s)         EXC_G((s)->data, const char *)
#define EXC_SETDATA(r, p)    ((r)->data = EXC_TOG(p))
#define EXC_CLS(o)           EXC_G((o)->cls, struct class_desc *)
#define EXC_PARENT(c)        EXC_G((c)->parent, struct class_desc *)
#define EXC_IMAGE(c)         EXC_G((c)->image, const word *)

/* A string value: a pointer to one of these descriptors (UTF-8 bytes,
 * byte length; text-encoding.md). */
struct exc_str {
    int len;
    EXC_PTRFIELD(const char *, data);
};

/* The compiler emits one descriptor per class:
 *     { parent; nverbs; nwords; image; (selector, code) * nverbs;
 *       nfields; (name, woffset, kind) * nfields; }
 * where nwords sizes the object's field segment and image seeds it.
 * The trailing field table (own fields; the parent chain covers
 * inherited ones) is the freeze/thaw walker's map (host-abi.md D7):
 * reach it at verbs + 2 * nverbs. */
struct class_desc {
    EXC_PTRFIELD(struct class_desc *, parent);
    word nverbs;
    word nwords;
    EXC_PTRFIELD(const word *, image);
    word verbs[];               /* (selector, code) pairs, then the
                                 * field table */
};

/* field kinds in the descriptor's field table */
#define EXC_FK_WORD  0          /* int, bool, decimal, enum, set */
#define EXC_FK_STR   1          /* serialized by content */
#define EXC_FK_REC   2          /* a record: not walked yet */
#define EXC_FK_MAYBE 3          /* a maybe T: a null-word pointer at
                                 * rest, not walked yet (serializing
                                 * the raw word would thaw a dangling
                                 * pointer) */
#define EXC_FK_OBJ   4          /* an obj / class / interface handle: not
                                 * walked. A handle is an object-table
                                 * index, not stable across a freeze, so
                                 * persisting the raw word would thaw a
                                 * dangling reference. Stable cross-freeze
                                 * object identity is unsettled (host-abi.md) */

/* An object: its class then its own field segment (host-abi.md D3). */
struct exc_obj {
    EXC_PTRFIELD(struct class_desc *, cls);
    word fields[];
};

#define EXC_OBJ_HDR ((long)sizeof(struct exc_obj))

/* Fault reporting (excelsior/runtime-errors.md): one { kind, line,
 * &file, &name } descriptor per trap site. */
#define EXC_TRAP_NO_BRANCH   1
#define EXC_TRAP_INDEX_RANGE 2
#define EXC_TRAP_UNCONSUMED  3
#define EXC_TRAP_DIV_ZERO    4
#define EXC_TRAP_OVERFLOW    5

struct exc_trapdesc {
    int kind;
    int line;
    EXC_PTRFIELD(const char *, file);   /* NUL-terminated, or 0 (host-detected) */
    EXC_PTRFIELD(const char *, name);   /* the failing callee etc., or 0 */
};

/* file/name read as host pointers (0 stays 0). */
#define EXC_TRAPFILE(w)  EXC_G((w)->file, const char *)
#define EXC_TRAPNAME(w)  EXC_G((w)->name, const char *)

/* Reserved globals and the compiler's entry symbols. __exc_self is shared
 * with the guest (it reads it for `self`), so it is held at guest width; in
 * the EXC_GUEST32 build the runtime stores it with EXC_TOG and reads it with
 * EXC_G. */
#ifdef EXC_GUEST32
typedef exc_gptr exc_selfref;
typedef exc_gptr exc_classref;
#else
typedef struct exc_obj *exc_selfref;
typedef struct class_desc *exc_classref;
#endif
extern exc_selfref __exc_self;
extern exc_classref __exc_entry_class;
extern word __exc_entry_selector;
extern word __exc_entry_argc;
extern word __exc_selnames[];   /* { count, &name... }, names in id order */

/* The arena (runtime/start.S). */
extern void *__moo_arena_alloc(int size);

/* libexc, for the binding: dispatch, spawn, and the selector intern
 * lookup a binding uses to wire its native objects (a console's tell). */
word __exc_send(struct exc_obj *recv, long selector, long argc, word *argv);
struct exc_obj *__exc_spawn(struct class_desc *cls);
long exc_sel_by_name(const char *name);

/* The freeze/thaw walker (host-abi.md D7): serialize an object's field
 * segment to the host's properties through __exh_prop_put, and overlay
 * found properties back onto an image-seeded segment. Keys are the
 * field name prefixed "x_" (visible in a property editor, safe from a
 * host's reserved names). Freeze writes every walked field: a
 * delta-from-defaults freeze cannot express a field reverting to its
 * default (the stale property would resurrect the old value), so delta
 * waits on a property-delete joining the binding surface. An absent
 * property on thaw leaves the default. Record and maybe fields are not
 * walked yet (EXC_FK_REC, EXC_FK_MAYBE). */
void exc_freeze(struct exc_obj *o, long host_obj);
void exc_thaw(struct exc_obj *o, long host_obj);

/* The binding, for libexc (host-abi.md D5). libexc calls only
 * __exh_emit and __exh_fault today; the rest complete the bound subset
 * of the D5 surface ahead of their callers (the freeze/thaw walker for
 * the properties, the turn machinery for yield/sleep), so a binding is
 * written once. A host without a service implements it as a no-op or an
 * error return. __exh_wait and __exh_spawn stay note-only until the
 * dispatch-loop work lands.
 *
 * __exh_emit channel 0 is the author channel (trace and fault text),
 * channel 1 the player console. __exh_fault routes a fault after libexc
 * has emitted the report; it does not return (the native binding exits
 * 70, a VM binding aborts the turn). The property calls are the
 * persistence bridge (D7): NUL-terminated keys, prop_get returns the
 * value length or negative, prop_put takes a NUL-terminated value.
 * __exh_post is cross-VM fire-and-forget (D2). */
void __exh_emit(long chan, const char *buf, long len);
void __exh_fault(const struct exc_trapdesc *why, long a, long b);
long __exh_prop_get(long obj, const char *key, char *buf, long bufsz);
long __exh_prop_put(long obj, const char *key, const char *val);
long __exh_post(long target, const char *buf, long len);
void __exh_yield(void);
void __exh_sleep(long ms);

#endif /* LIBEXC_H */
