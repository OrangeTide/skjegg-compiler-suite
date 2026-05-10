/* test_gc.c : standalone tests for the TinScheme garbage collector */

#include "gc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int
list_len(val_t v)
{
    int n = 0;

    while (!IS_NIL(v)) {
        n++;
        v = gc_cdr(v);
    }
    return n;
}

/****************************************************************
 * Tests
 ****************************************************************/

static void
test_fixnum(void)
{
    assert(IS_FIX(MAKE_FIX(0)));
    assert(IS_FIX(MAKE_FIX(42)));
    assert(IS_FIX(MAKE_FIX(-1)));
    assert(GET_FIX(MAKE_FIX(0)) == 0);
    assert(GET_FIX(MAKE_FIX(42)) == 42);
    assert(GET_FIX(MAKE_FIX(-1)) == -1);
    assert(GET_FIX(MAKE_FIX(-100)) == -100);
    assert(GET_FIX(MAKE_FIX(1000000)) == 1000000);
    assert(!IS_NIL(MAKE_FIX(0)));
    assert(!IS_PTR(MAKE_FIX(42)));
    assert(IS_NIL(VAL_NIL));
    assert(!IS_FIX(VAL_NIL));
    assert(!IS_PTR(VAL_NIL));
    printf("  fixnum: ok\n");
}

static void
test_cons_basic(void)
{
    struct gc_heap h;
    val_t pair;

    gc_init(&h);
    pair = gc_cons(&h, MAKE_FIX(1), MAKE_FIX(2));
    assert(IS_PTR(pair));
    assert(PTR(pair)->type == OBJ_PAIR);
    assert(GET_FIX(gc_car(pair)) == 1);
    assert(GET_FIX(gc_cdr(pair)) == 2);
    gc_destroy(&h);
    printf("  cons_basic: ok\n");
}

static void
test_list(void)
{
    struct gc_heap h;
    val_t list = VAL_NIL;
    int i;

    gc_init(&h);
    gc_push(&h, &list);

    for (i = 4; i >= 0; i--)
        list = gc_cons(&h, MAKE_FIX(i), list);

    assert(list_len(list) == 5);
    for (i = 0; i < 5; i++) {
        assert(GET_FIX(gc_car(list)) == i);
        list = gc_cdr(list);
    }
    assert(IS_NIL(list));

    gc_pop(&h, 1);
    gc_destroy(&h);
    printf("  list: ok\n");
}

static void
test_set_car_cdr(void)
{
    struct gc_heap h;
    val_t p;

    gc_init(&h);
    p = gc_cons(&h, MAKE_FIX(1), MAKE_FIX(2));
    gc_push(&h, &p);

    gc_set_car(p, MAKE_FIX(10));
    gc_set_cdr(p, MAKE_FIX(20));
    assert(GET_FIX(gc_car(p)) == 10);
    assert(GET_FIX(gc_cdr(p)) == 20);

    gc_set_cdr(p, gc_cons(&h, MAKE_FIX(30), VAL_NIL));
    assert(GET_FIX(gc_car(gc_cdr(p))) == 30);

    gc_pop(&h, 1);
    gc_destroy(&h);
    printf("  set_car_cdr: ok\n");
}

static void
test_string(void)
{
    struct gc_heap h;
    val_t s;
    struct gc_string *str;

    gc_init(&h);
    s = gc_string(&h, "hello", 5);
    gc_push(&h, &s);

    assert(IS_PTR(s));
    str = (struct gc_string *)PTR(s);
    assert(str->hdr.type == OBJ_STRING);
    assert(str->len == 5);
    assert(strcmp(str->data, "hello") == 0);

    gc_pop(&h, 1);
    gc_destroy(&h);
    printf("  string: ok\n");
}

static void
test_symbol(void)
{
    struct gc_heap h;
    val_t a, b;
    struct gc_string *sa, *sb;

    gc_init(&h);
    a = gc_symbol(&h, "foo");
    gc_push(&h, &a);
    b = gc_symbol(&h, "bar");
    gc_push(&h, &b);

    sa = (struct gc_string *)PTR(a);
    sb = (struct gc_string *)PTR(b);
    assert(sa->hdr.type == OBJ_SYMBOL);
    assert(sb->hdr.type == OBJ_SYMBOL);
    assert(strcmp(sa->data, "foo") == 0);
    assert(strcmp(sb->data, "bar") == 0);

    gc_pop(&h, 2);
    gc_destroy(&h);
    printf("  symbol: ok\n");
}

static void
test_gc_reclaim(void)
{
    struct gc_heap h;
    val_t keep = VAL_NIL;
    int i;

    gc_init(&h);
    h.threshold = 10;
    gc_push(&h, &keep);

    for (i = 0; i < 100; i++)
        gc_cons(&h, MAKE_FIX(i), VAL_NIL);

    keep = VAL_NIL;
    for (i = 0; i < 5; i++)
        keep = gc_cons(&h, MAKE_FIX(i), keep);

    gc_collect(&h);
    assert(h.count == 5);
    assert(list_len(keep) == 5);

    gc_pop(&h, 1);
    gc_destroy(&h);
    printf("  gc_reclaim: ok\n");
}

static void
test_gc_stress(void)
{
    struct gc_heap h;
    val_t list = VAL_NIL;
    int i;

    gc_init(&h);
    h.threshold = 8;
    gc_push(&h, &list);

    for (i = 0; i < 10000; i++)
        list = gc_cons(&h, MAKE_FIX(i), list);

    assert(list_len(list) == 10000);
    assert(GET_FIX(gc_car(list)) == 9999);

    gc_pop(&h, 1);
    gc_destroy(&h);
    printf("  gc_stress: ok\n");
}

static void
test_closure(void)
{
    struct gc_heap h;
    val_t captured[2];
    val_t cl;
    struct gc_closure *c;

    gc_init(&h);
    captured[0] = MAKE_FIX(10);
    captured[1] = MAKE_FIX(20);
    cl = gc_closure(&h, 42, 2, captured);
    gc_push(&h, &cl);

    assert(IS_PTR(cl));
    c = (struct gc_closure *)PTR(cl);
    assert(c->hdr.type == OBJ_CLOSURE);
    assert(c->func_id == 42);
    assert(c->nfree == 2);
    assert(GET_FIX(c->free[0]) == 10);
    assert(GET_FIX(c->free[1]) == 20);

    gc_pop(&h, 1);
    gc_destroy(&h);
    printf("  closure: ok\n");
}

static void
test_closure_gc(void)
{
    struct gc_heap h;
    val_t cl = VAL_NIL;
    val_t inner = VAL_NIL;
    val_t captured[1];
    struct gc_closure *c;
    int i;

    gc_init(&h);
    h.threshold = 4;
    gc_push(&h, &cl);
    gc_push(&h, &inner);

    inner = gc_cons(&h, MAKE_FIX(99), VAL_NIL);
    captured[0] = inner;
    cl = gc_closure(&h, 1, 1, captured);
    inner = VAL_NIL;

    for (i = 0; i < 50; i++)
        gc_cons(&h, MAKE_FIX(i), VAL_NIL);

    gc_collect(&h);

    c = (struct gc_closure *)PTR(cl);
    assert(IS_PTR(c->free[0]));
    assert(GET_FIX(gc_car(c->free[0])) == 99);
    assert(h.count == 2);

    gc_pop(&h, 2);
    gc_destroy(&h);
    printf("  closure_gc: ok\n");
}

static void
test_nested_structures(void)
{
    struct gc_heap h;
    val_t tree = VAL_NIL;
    val_t left = VAL_NIL;
    val_t right = VAL_NIL;
    int i;

    gc_init(&h);
    h.threshold = 4;
    gc_push(&h, &tree);
    gc_push(&h, &left);
    gc_push(&h, &right);

    left = gc_cons(&h, MAKE_FIX(1),
        gc_cons(&h, MAKE_FIX(2), VAL_NIL));
    right = gc_cons(&h, MAKE_FIX(3),
        gc_cons(&h, MAKE_FIX(4), VAL_NIL));
    tree = gc_cons(&h, left, right);
    left = VAL_NIL;
    right = VAL_NIL;

    for (i = 0; i < 200; i++)
        gc_cons(&h, MAKE_FIX(i), VAL_NIL);

    gc_collect(&h);

    assert(h.count == 5);
    assert(GET_FIX(gc_car(gc_car(tree))) == 1);
    assert(GET_FIX(gc_car(gc_cdr(gc_car(tree)))) == 2);
    assert(GET_FIX(gc_car(gc_cdr(tree))) == 3);
    assert(GET_FIX(gc_car(gc_cdr(gc_cdr(tree)))) == 4);

    gc_pop(&h, 3);
    gc_destroy(&h);
    printf("  nested_structures: ok\n");
}

static void
test_string_survives_gc(void)
{
    struct gc_heap h;
    val_t list = VAL_NIL;
    val_t s = VAL_NIL;
    struct gc_string *str;
    int i;

    gc_init(&h);
    h.threshold = 4;
    gc_push(&h, &list);
    gc_push(&h, &s);

    s = gc_string(&h, "preserved", 9);
    list = gc_cons(&h, s, VAL_NIL);
    s = VAL_NIL;

    for (i = 0; i < 100; i++)
        gc_cons(&h, MAKE_FIX(i), VAL_NIL);

    gc_collect(&h);

    assert(h.count == 2);
    str = (struct gc_string *)PTR(gc_car(list));
    assert(str->len == 9);
    assert(strcmp(str->data, "preserved") == 0);

    gc_pop(&h, 2);
    gc_destroy(&h);
    printf("  string_survives_gc: ok\n");
}

static void
test_booleans(void)
{
    assert(!IS_FIX(VAL_TRUE));
    assert(!IS_FIX(VAL_FALSE));
    assert(!IS_NIL(VAL_TRUE));
    assert(!IS_NIL(VAL_FALSE));
    assert(!IS_PTR(VAL_TRUE));
    assert(!IS_PTR(VAL_FALSE));
    assert(IS_IMM(VAL_TRUE));
    assert(IS_IMM(VAL_FALSE));

    assert(IS_TRUE(VAL_TRUE));
    assert(IS_FALSE(VAL_FALSE));
    assert(!IS_TRUE(VAL_FALSE));
    assert(!IS_FALSE(VAL_TRUE));

    assert(IS_TRUE(MAKE_FIX(0)));
    assert(IS_TRUE(MAKE_FIX(42)));
    assert(IS_TRUE(VAL_NIL));

    assert(VAL_TRUE != VAL_FALSE);
    assert(VAL_FALSE != VAL_NIL);
    printf("  booleans: ok\n");
}

static void
test_bool_in_list(void)
{
    struct gc_heap h;
    val_t list = VAL_NIL;

    gc_init(&h);
    gc_push(&h, &list);

    list = gc_cons(&h, VAL_TRUE, gc_cons(&h, VAL_FALSE, VAL_NIL));
    gc_collect(&h);

    assert(h.count == 2);
    assert(gc_car(list) == VAL_TRUE);
    assert(gc_car(gc_cdr(list)) == VAL_FALSE);

    gc_pop(&h, 1);
    gc_destroy(&h);
    printf("  bool_in_list: ok\n");
}

/****************************************************************
 * Main
 ****************************************************************/

int
main(void)
{
    printf("gc tests:\n");
    test_fixnum();
    test_booleans();
    test_cons_basic();
    test_list();
    test_set_car_cdr();
    test_string();
    test_symbol();
    test_gc_reclaim();
    test_gc_stress();
    test_closure();
    test_closure_gc();
    test_nested_structures();
    test_string_survives_gc();
    test_bool_in_list();
    printf("all passed\n");
    return 0;
}
