/* gc.h : mark-sweep garbage collector for TinScheme values */

#ifndef GC_H
#define GC_H

#include <stdint.h>
#include <stddef.h>

/****************************************************************
 * Value representation
 *
 * Tagged uintptr_t with 2-bit tag:
 *   bits[1:0] == 01: fixnum (signed, arithmetic shift right 2)
 *   bits[1:0] == 00: heap object pointer (nil when value == 0)
 *   bits[1:0] == 10: immediate constant (#t, #f)
 *   bits[1:0] == 11: unused
 ****************************************************************/

typedef uintptr_t val_t;

#define TAG_MASK    ((val_t)3)
#define TAG_OBJ     ((val_t)0)
#define TAG_FIX     ((val_t)1)
#define TAG_IMM     ((val_t)2)

#define VAL_NIL     ((val_t)0)
#define VAL_FALSE   ((val_t)TAG_IMM)
#define VAL_TRUE    ((val_t)(TAG_IMM | 4))

#define MAKE_FIX(n) ((val_t)((uintptr_t)(intptr_t)(int)(n) << 2 | TAG_FIX))
#define GET_FIX(v)  ((int)((intptr_t)(v) >> 2))
#define IS_FIX(v)   (((v) & TAG_MASK) == TAG_FIX)
#define IS_NIL(v)   ((v) == VAL_NIL)
#define IS_PTR(v)   (((v) & TAG_MASK) == TAG_OBJ && (v) != VAL_NIL)
#define IS_IMM(v)   (((v) & TAG_MASK) == TAG_IMM)
#define IS_FALSE(v) ((v) == VAL_FALSE)
#define IS_TRUE(v)  ((v) != VAL_FALSE)

static inline struct gc_obj *PTR(val_t v)
{
    return (struct gc_obj *)v;
}

static inline val_t VAL(void *p)
{
    return (val_t)p;
}

/****************************************************************
 * Object types
 ****************************************************************/

enum {
    OBJ_PAIR,
    OBJ_STRING,
    OBJ_SYMBOL,
    OBJ_CLOSURE,
};

struct gc_obj {
    unsigned char type;
    unsigned char mark;
    struct gc_obj *next;
};

struct gc_pair {
    struct gc_obj hdr;
    val_t car;
    val_t cdr;
};

struct gc_string {
    struct gc_obj hdr;
    int len;
    char data[];
};

struct gc_closure {
    struct gc_obj hdr;
    int func_id;
    int nfree;
    val_t free[];
};

/****************************************************************
 * Heap
 ****************************************************************/

#define GC_ROOTS_MAX 256

struct gc_heap {
    struct gc_obj *all;
    int count;
    int threshold;
    val_t *roots[GC_ROOTS_MAX];
    int nroots;
};

/****************************************************************
 * API
 ****************************************************************/

void gc_init(struct gc_heap *h);
void gc_destroy(struct gc_heap *h);
void gc_collect(struct gc_heap *h);

void gc_push(struct gc_heap *h, val_t *root);
void gc_pop(struct gc_heap *h, int n);

val_t gc_cons(struct gc_heap *h, val_t car, val_t cdr);
val_t gc_string(struct gc_heap *h, const char *s, int len);
val_t gc_symbol(struct gc_heap *h, const char *s);
val_t gc_closure(struct gc_heap *h, int func_id, int nfree, val_t *captured);

val_t gc_car(val_t v);
val_t gc_cdr(val_t v);
void gc_set_car(val_t v, val_t x);
void gc_set_cdr(val_t v, val_t x);

#endif /* GC_H */
