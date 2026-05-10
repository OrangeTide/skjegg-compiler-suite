/* gc.c : mark-sweep garbage collector for TinScheme values */

#include "gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
gc_init(struct gc_heap *h)
{
    h->all = NULL;
    h->count = 0;
    h->threshold = 64;
    h->nroots = 0;
}

void
gc_destroy(struct gc_heap *h)
{
    struct gc_obj *o, *next;

    for (o = h->all; o; o = next) {
        next = o->next;
        free(o);
    }
    h->all = NULL;
    h->count = 0;
    h->nroots = 0;
}

/****************************************************************
 * Mark phase
 ****************************************************************/

static void
mark_val(val_t v)
{
    struct gc_obj *o;
    struct gc_pair *p;
    struct gc_closure *cl;
    int i;

    if (!IS_PTR(v))
        return;
    o = PTR(v);
    if (o->mark)
        return;
    o->mark = 1;
    switch (o->type) {
    case OBJ_PAIR:
        p = (struct gc_pair *)o;
        mark_val(p->car);
        mark_val(p->cdr);
        break;
    case OBJ_CLOSURE:
        cl = (struct gc_closure *)o;
        for (i = 0; i < cl->nfree; i++)
            mark_val(cl->free[i]);
        break;
    case OBJ_STRING:
    case OBJ_SYMBOL:
        break;
    }
}

/****************************************************************
 * Collect
 ****************************************************************/

void
gc_collect(struct gc_heap *h)
{
    struct gc_obj **pp, *o;
    int i;

    for (i = 0; i < h->nroots; i++)
        mark_val(*h->roots[i]);

    pp = &h->all;
    while ((o = *pp) != NULL) {
        if (o->mark) {
            o->mark = 0;
            pp = &o->next;
        } else {
            *pp = o->next;
            free(o);
            h->count--;
        }
    }

    if (h->threshold < h->count * 2)
        h->threshold = h->count * 2;
    if (h->threshold < 64)
        h->threshold = 64;
}

/****************************************************************
 * Allocation
 ****************************************************************/

static struct gc_obj *
gc_alloc(struct gc_heap *h, int type, size_t sz)
{
    struct gc_obj *o;

    if (h->count >= h->threshold)
        gc_collect(h);
    o = malloc(sz);
    if (!o) {
        gc_collect(h);
        o = malloc(sz);
        if (!o) {
            fprintf(stderr, "gc: out of memory\n");
            exit(1);
        }
    }
    memset(o, 0, sz);
    o->type = (unsigned char)type;
    o->next = h->all;
    h->all = o;
    h->count++;
    return o;
}

/****************************************************************
 * Root stack
 ****************************************************************/

void
gc_push(struct gc_heap *h, val_t *root)
{
    if (h->nroots >= GC_ROOTS_MAX) {
        fprintf(stderr, "gc: root stack overflow\n");
        exit(1);
    }
    h->roots[h->nroots++] = root;
}

void
gc_pop(struct gc_heap *h, int n)
{
    h->nroots -= n;
}

/****************************************************************
 * Allocators (root-safe: arguments are protected across GC)
 ****************************************************************/

val_t
gc_cons(struct gc_heap *h, val_t car, val_t cdr)
{
    struct gc_pair *p;

    gc_push(h, &car);
    gc_push(h, &cdr);
    p = (struct gc_pair *)gc_alloc(h, OBJ_PAIR, sizeof(*p));
    p->car = car;
    p->cdr = cdr;
    gc_pop(h, 2);
    return VAL(p);
}

static val_t
alloc_str(struct gc_heap *h, int type, const char *s, int len)
{
    struct gc_string *str;

    str = (struct gc_string *)gc_alloc(h, type,
        sizeof(*str) + (size_t)len + 1);
    str->len = len;
    memcpy(str->data, s, (size_t)len);
    str->data[len] = '\0';
    return VAL(str);
}

val_t
gc_string(struct gc_heap *h, const char *s, int len)
{
    return alloc_str(h, OBJ_STRING, s, len);
}

val_t
gc_symbol(struct gc_heap *h, const char *s)
{
    return alloc_str(h, OBJ_SYMBOL, s, (int)strlen(s));
}

val_t
gc_closure(struct gc_heap *h, int func_id, int nfree, val_t *captured)
{
    struct gc_closure *cl;
    int i;

    for (i = 0; i < nfree; i++)
        gc_push(h, &captured[i]);
    cl = (struct gc_closure *)gc_alloc(h, OBJ_CLOSURE,
        sizeof(*cl) + (size_t)nfree * sizeof(val_t));
    cl->func_id = func_id;
    cl->nfree = nfree;
    for (i = 0; i < nfree; i++)
        cl->free[i] = captured[i];
    gc_pop(h, nfree);
    return VAL(cl);
}

/****************************************************************
 * Accessors
 ****************************************************************/

val_t
gc_car(val_t v)
{
    return ((struct gc_pair *)PTR(v))->car;
}

val_t
gc_cdr(val_t v)
{
    return ((struct gc_pair *)PTR(v))->cdr;
}

void
gc_set_car(val_t v, val_t x)
{
    ((struct gc_pair *)PTR(v))->car = x;
}

void
gc_set_cdr(val_t v, val_t x)
{
    ((struct gc_pair *)PTR(v))->cdr = x;
}
