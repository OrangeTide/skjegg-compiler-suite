/* parse.c : S-expression parser for TinScheme */

#include "scheme.h"

#include <stdio.h>
#include <string.h>

static val_t read_datum(struct gc_heap *h);

static val_t
read_list(struct gc_heap *h)
{
    struct token t;
    val_t head = VAL_NIL;
    val_t tail = VAL_NIL;
    val_t elem = VAL_NIL, cell = VAL_NIL;

    gc_push(h, &head);
    gc_push(h, &tail);
    gc_push(h, &elem);
    gc_push(h, &cell);

    for (;;) {
        t = lex_peek();
        if (t.kind == TOK_EOF)
            die("%s:%d: unterminated list", "<input>", t.line);
        if (t.kind == TOK_RPAREN) {
            lex_next();
            break;
        }
        if (t.kind == TOK_DOT) {
            lex_next();
            if (IS_NIL(head))
                die("%s:%d: dot at start of list",
                    "<input>", t.line);
            elem = read_datum(h);
            t = lex_next();
            if (t.kind != TOK_RPAREN)
                die("%s:%d: expected ')' after dotted cdr",
                    "<input>", t.line);
            gc_set_cdr(tail, elem);
            break;
        }
        elem = read_datum(h);
        cell = gc_cons(h, elem, VAL_NIL);
        if (IS_NIL(head)) {
            head = cell;
        } else {
            gc_set_cdr(tail, cell);
        }
        tail = cell;
    }

    gc_pop(h, 4);
    return head;
}

static val_t
read_datum(struct gc_heap *h)
{
    struct token t;
    val_t v, quoted;

    t = lex_next();
    switch (t.kind) {
    case TOK_NUM:
        return MAKE_FIX((int)t.nval);
    case TOK_STR:
        return gc_string(h, t.sval, t.slen);
    case TOK_SYM:
        return gc_symbol(h, t.sval);
    case TOK_TRUE:
        return VAL_TRUE;
    case TOK_FALSE:
        return VAL_FALSE;
    case TOK_LPAREN:
        return read_list(h);
    case TOK_QUOTE:
        v = VAL_NIL;
        gc_push(h, &v);
        quoted = read_datum(h);
        v = gc_cons(h, quoted, VAL_NIL);
        v = gc_cons(h, gc_symbol(h, "quote"), v);
        gc_pop(h, 1);
        return v;
    case TOK_RPAREN:
        die("%s:%d: unexpected ')'", "<input>", t.line);
        break;
    case TOK_DOT:
        die("%s:%d: unexpected '.'", "<input>", t.line);
        break;
    case TOK_EOF:
        die("%s:%d: unexpected end of input", "<input>", t.line);
        break;
    }
    return VAL_NIL;
}

val_t
scm_read(struct gc_heap *h)
{
    if (lex_peek().kind == TOK_EOF)
        return VAL_NIL;
    return read_datum(h);
}

val_t
scm_read_all(struct gc_heap *h)
{
    val_t head = VAL_NIL;
    val_t tail = VAL_NIL;
    val_t form, cell;

    gc_push(h, &head);
    gc_push(h, &tail);
    gc_push(h, &form);
    gc_push(h, &cell);

    while (lex_peek().kind != TOK_EOF) {
        form = read_datum(h);
        cell = gc_cons(h, form, VAL_NIL);
        if (IS_NIL(head)) {
            head = cell;
        } else {
            gc_set_cdr(tail, cell);
        }
        tail = cell;
    }

    gc_pop(h, 4);
    return head;
}
