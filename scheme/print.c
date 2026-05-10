/* print.c : S-expression printer for TinScheme */

#include "scheme.h"

#include <stdio.h>

void
scm_print(val_t v)
{
    val_t rest;
    struct gc_string *s;

    if (IS_NIL(v)) {
        printf("()");
        return;
    }
    if (v == VAL_TRUE) {
        printf("#t");
        return;
    }
    if (v == VAL_FALSE) {
        printf("#f");
        return;
    }
    if (IS_FIX(v)) {
        printf("%d", GET_FIX(v));
        return;
    }
    if (!IS_PTR(v)) {
        printf("#<unknown>");
        return;
    }

    switch (PTR(v)->type) {
    case OBJ_SYMBOL:
        s = (struct gc_string *)PTR(v);
        printf("%s", s->data);
        break;
    case OBJ_STRING:
        s = (struct gc_string *)PTR(v);
        printf("\"%s\"", s->data);
        break;
    case OBJ_PAIR:
        printf("(");
        scm_print(gc_car(v));
        rest = gc_cdr(v);
        while (IS_PTR(rest) && PTR(rest)->type == OBJ_PAIR) {
            printf(" ");
            scm_print(gc_car(rest));
            rest = gc_cdr(rest);
        }
        if (!IS_NIL(rest)) {
            printf(" . ");
            scm_print(rest);
        }
        printf(")");
        break;
    case OBJ_CLOSURE:
        printf("#<closure>");
        break;
    }
}

void
scm_println(val_t v)
{
    scm_print(v);
    printf("\n");
}
