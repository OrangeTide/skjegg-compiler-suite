/* libexc.h : the seam between libexc (the portable Excelsior guest
 * runtime, layer 1 of excelsior/host-abi.md) and a host binding
 * (layer 2). A binding implements the __exh_* calls and the entry
 * driver; libexc implements everything the compiler emits calls to.
 * The native binding is runtime/exc_native.c.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#ifndef LIBEXC_H
#define LIBEXC_H

typedef long word;

/* A string value: a pointer to one of these descriptors (UTF-8 bytes,
 * byte length; text-encoding.md). */
struct exc_str {
    int len;
    const char *data;
};

/* The compiler emits one descriptor per class:
 *     { parent; nverbs; nwords; image; (selector, code) * nverbs;
 *       nfields; (name, woffset, kind) * nfields; }
 * where nwords sizes the object's field segment and image seeds it.
 * The trailing field table (own fields; the parent chain covers
 * inherited ones) is the freeze/thaw walker's map (host-abi.md D7):
 * reach it at verbs + 2 * nverbs. */
struct class_desc {
    struct class_desc *parent;
    long nverbs;
    long nwords;
    const word *image;
    long verbs[];               /* (selector, code) pairs, then the
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
    struct class_desc *cls;
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
    const char *file;           /* NUL-terminated, or 0 (host-detected) */
    const char *name;           /* the failing callee etc., or 0 */
};

/* Reserved globals and the compiler's entry symbols. */
extern struct exc_obj *__exc_self;
extern struct class_desc *__exc_entry_class;
extern long __exc_entry_selector;
extern long __exc_entry_argc;
extern long __exc_selnames[];   /* { count, &name... }, names in id order */

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
