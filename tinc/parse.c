/* parse.c : recursive-descent parser for TinC
 * Made by a machine.  PUBLIC DOMAIN (CC0-1.0)
 */

#include "tinc.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Helpers
 ****************************************************************/

static struct arena *parse_arena;
static struct token cur;

static void
advance(void)
{
    cur = lex_next();
}

static int
accept(int k)
{
    if (cur.kind == k) {
        advance();
        return 1;
    }
    return 0;
}

static void
expect(int k, const char *what)
{
    if (cur.kind != k)
        die("parse:%d: expected %s", cur.line, what);
    advance();
}

static struct node *
new_node(int kind, int line)
{
    struct node *n;

    n = arena_zalloc(parse_arena, sizeof(*n));
    n->kind = kind;
    n->line = line;
    n->elem = ELEM_INT;
    return n;
}

/****************************************************************
 * Expressions (precedence climb)
 ****************************************************************/

static struct node *parse_expr(void);
static struct node *parse_assign(void);

static struct node *
parse_primary(void)
{
    struct node *n;
    struct token t;

    t = cur;
    if (t.kind == T_NUMBER) {
        advance();
        n = new_node(N_NUM, t.line);
        n->ival = t.nval;
        return n;
    }
    if (t.kind == T_CHARLIT) {
        advance();
        n = new_node(N_CHARLIT, t.line);
        n->ival = t.nval;
        return n;
    }
    if (t.kind == T_STRING) {
        advance();
        n = new_node(N_STR, t.line);
        n->sval = t.sval;
        n->slen = t.slen;
        return n;
    }
    if (t.kind == T_IDENT) {
        advance();
        n = new_node(N_NAME, t.line);
        n->name = t.sval;
        return n;
    }
    if (t.kind == T_UNDERSCORE) {
        advance();
        return new_node(N_DISCARD, t.line);
    }
    if (t.kind == T_LPAREN) {
        advance();
        n = parse_expr();
        expect(T_RPAREN, ")");
        return n;
    }
    die("parse:%d: unexpected token in expression", t.line);
    return NULL;
}

static struct node *
parse_postfix(void)
{
    struct node *n, *idx, *args, **tail, *arg, *p;
    int line;

    n = parse_primary();
    for (;;) {
        line = cur.line;
        if (cur.kind == T_LBRACK) {
            advance();
            idx = parse_expr();
            if (cur.kind == T_COLON) {
                advance();
                p = new_node(N_SLICE, line);
                p->a = n;
                p->b = idx;
                p->c = parse_expr();
                expect(T_RBRACK, "]");
                n = p;
            } else {
                expect(T_RBRACK, "]");
                p = new_node(N_INDEX, line);
                p->a = n;
                p->b = idx;
                n = p;
            }
        } else if (cur.kind == T_LPAREN) {
            advance();
            args = NULL;
            tail = &args;
            if (cur.kind != T_RPAREN) {
                for (;;) {
                    arg = parse_assign();
                    *tail = arg;
                    tail = &arg->next;
                    if (!accept(T_COMMA))
                        break;
                }
            }
            expect(T_RPAREN, ")");
            p = new_node(N_CALL, line);
            p->a = n;
            p->b = args;
            n = p;
        } else {
            break;
        }
    }
    return n;
}

static struct node *
parse_unary(void)
{
    struct node *n, *u;
    int op, line;

    if (cur.kind == T_MINUS || cur.kind == T_BANG ||
        cur.kind == T_TILDE || cur.kind == T_PLUS) {
        op = cur.kind;
        line = cur.line;
        advance();
        n = parse_unary();
        if (op == T_PLUS)
            return n;
        u = new_node(N_UNOP, line);
        u->op = op;
        u->a = n;
        return u;
    }
    return parse_postfix();
}

static int
binop_prec(int k)
{
    switch (k) {
    case T_STAR: case T_SLASH: case T_PERCENT:      return 11;
    case T_PLUS: case T_MINUS:                       return 10;
    case T_SHL:  case T_SHR:                         return  9;
    case T_LT: case T_LE: case T_GT: case T_GE:     return  8;
    case T_EQ: case T_NE:                            return  7;
    case T_AMP:                                      return  6;
    case T_CARET:                                    return  5;
    case T_PIPE:                                     return  4;
    case T_ANDAND:                                   return  3;
    case T_OROR:                                     return  2;
    default:                                         return -1;
    }
}

static struct node *
parse_binop(int min_prec)
{
    struct node *lhs, *rhs, *b;
    int prec, op, line;

    lhs = parse_unary();
    for (;;) {
        prec = binop_prec(cur.kind);
        if (prec < min_prec)
            return lhs;
        op = cur.kind;
        line = cur.line;
        advance();
        rhs = parse_binop(prec + 1);
        b = new_node(N_BINOP, line);
        b->op = op;
        b->a = lhs;
        b->b = rhs;
        lhs = b;
    }
}

static int
is_compound_assign(int k)
{
    return k == T_PLUS_EQ || k == T_MINUS_EQ || k == T_STAR_EQ ||
           k == T_SLASH_EQ || k == T_PERCENT_EQ ||
           k == T_AMP_EQ || k == T_PIPE_EQ || k == T_CARET_EQ ||
           k == T_SHL_EQ || k == T_SHR_EQ;
}

static int
compound_to_binop(int k)
{
    switch (k) {
    case T_PLUS_EQ:    return T_PLUS;
    case T_MINUS_EQ:   return T_MINUS;
    case T_STAR_EQ:    return T_STAR;
    case T_SLASH_EQ:   return T_SLASH;
    case T_PERCENT_EQ: return T_PERCENT;
    case T_AMP_EQ:     return T_AMP;
    case T_PIPE_EQ:    return T_PIPE;
    case T_CARET_EQ:   return T_CARET;
    case T_SHL_EQ:     return T_SHL;
    case T_SHR_EQ:     return T_SHR;
    }
    return -1;
}

static struct node *
parse_assign(void)
{
    struct node *lhs, *rhs, *n;
    int line;

    lhs = parse_binop(2);
    line = cur.line;
    if (cur.kind == T_ASSIGN) {
        advance();
        rhs = parse_assign();
        n = new_node(N_ASSIGN, line);
        n->a = lhs;
        n->b = rhs;
        return n;
    }
    if (is_compound_assign(cur.kind)) {
        int op = compound_to_binop(cur.kind);
        advance();
        rhs = parse_assign();
        n = new_node(N_COMPOUND_ASSIGN, line);
        n->op = op;
        n->a = lhs;
        n->b = rhs;
        return n;
    }
    return lhs;
}

static struct node *
parse_expr(void)
{
    return parse_assign();
}

/****************************************************************
 * Initializers
 ****************************************************************/

static struct node *
parse_initializer(int *is_string, int *string_len)
{
    struct node *head, **tail, *e;

    *is_string = 0;
    *string_len = 0;

    if (cur.kind == T_STRING) {
        e = parse_primary();
        *is_string = 1;
        *string_len = e->slen;
        return e;
    }
    if (accept(T_LBRACE)) {
        head = NULL;
        tail = &head;
        if (cur.kind != T_RBRACE) {
            for (;;) {
                e = parse_assign();
                *tail = e;
                tail = &e->next;
                if (!accept(T_COMMA))
                    break;
                if (cur.kind == T_RBRACE)
                    break;
            }
        }
        expect(T_RBRACE, "}");
        {
            struct node *n = new_node(N_INIT_LIST, cur.line);
            n->a = head;
            return n;
        }
    }
    return parse_assign();
}

/****************************************************************
 * Variable declarations (shared by global and local scope)
 ****************************************************************/

static struct node *
parse_var_decl(void)
{
    struct node *n;
    int line, elem, arr;

    line = cur.line;
    elem = ELEM_INT;
    if (cur.kind == T_BYTE) {
        elem = ELEM_BYTE;
    }
    advance();

    if (cur.kind != T_IDENT)
        die("parse:%d: expected identifier in declaration", cur.line);

    n = new_node(N_GLOBAL, line);
    n->name = cur.sval;
    n->elem = elem;
    advance();

    arr = 0;
    if (accept(T_LBRACK)) {
        if (cur.kind == T_NUMBER) {
            arr = (int)cur.nval;
            advance();
        } else {
            arr = -1;
        }
        expect(T_RBRACK, "]");
    }
    n->arr_size = arr;

    if (accept(T_ASSIGN)) {
        int is_string = 0;
        int string_len = 0;
        n->a = parse_initializer(&is_string, &string_len);
        if (arr == -1) {
            if (is_string)
                n->arr_size = string_len + 1;
            else if (n->a && n->a->kind == N_INIT_LIST) {
                int cnt = 0;
                struct node *e;
                for (e = n->a->a; e; e = e->next)
                    cnt++;
                n->arr_size = cnt;
            }
        }
    }

    expect(T_SEMI, ";");
    return n;
}

/****************************************************************
 * Statements
 ****************************************************************/

static struct node *parse_stmt(void);

static struct node *
parse_compound(void)
{
    struct node *n, *stmts, **tail, *s;
    int line;

    line = cur.line;
    expect(T_LBRACE, "{");
    stmts = NULL;
    tail = &stmts;
    while (cur.kind != T_RBRACE && cur.kind != T_EOF) {
        s = parse_stmt();
        *tail = s;
        tail = &s->next;
    }
    expect(T_RBRACE, "}");
    n = new_node(N_BLOCK, line);
    n->a = stmts;
    return n;
}

static struct node *
parse_destruct_stmt(int line)
{
    struct node *head, **tail, *target = NULL, *rhs, *n;
    int tline;

    head = NULL;
    tail = &head;
    for (;;) {
        tline = cur.line;
        if (cur.kind == T_UNDERSCORE) {
            advance();
            target = new_node(N_DISCARD, tline);
        } else if (cur.kind == T_IDENT) {
            target = new_node(N_NAME, tline);
            target->name = cur.sval;
            advance();
            if (accept(T_DOTDOT)) {
                struct node *rest = new_node(N_REST, tline);
                rest->name = target->name;
                target = rest;
            }
        } else {
            die("parse:%d: expected identifier or _ in destructuring",
                tline);
        }
        *tail = target;
        tail = &target->next;
        if (!accept(T_COMMA))
            break;
    }
    expect(T_RPAREN, ")");
    expect(T_ASSIGN, "=");
    rhs = parse_expr();
    expect(T_SEMI, ";");

    n = new_node(N_DESTRUCT, line);
    n->a = head;
    n->b = rhs;
    return n;
}

static struct node *
parse_stmt(void)
{
    struct node *n, *e;
    int line;

    line = cur.line;
    if (cur.kind == T_LBRACE)
        return parse_compound();

    if (cur.kind == T_INT || cur.kind == T_BYTE)
        return parse_var_decl();

    if (cur.kind == T_IF) {
        advance();
        expect(T_LPAREN, "(");
        n = new_node(N_IF, line);
        n->a = parse_expr();
        expect(T_RPAREN, ")");
        n->b = parse_stmt();
        if (accept(T_ELSE))
            n->c = parse_stmt();
        return n;
    }
    if (cur.kind == T_WHILE) {
        advance();
        expect(T_LPAREN, "(");
        n = new_node(N_WHILE, line);
        n->a = parse_expr();
        expect(T_RPAREN, ")");
        n->b = parse_stmt();
        return n;
    }
    if (cur.kind == T_FOREACH) {
        advance();
        expect(T_LPAREN, "(");
        n = new_node(N_FOREACH, line);
        if (cur.kind != T_IDENT)
            die("parse:%d: expected identifier in foreach", cur.line);
        n->name = cur.sval;
        advance();
        expect(T_COLON, ":");
        n->a = parse_expr();
        expect(T_RPAREN, ")");
        n->b = parse_stmt();
        return n;
    }
    if (cur.kind == T_BREAK) {
        advance();
        expect(T_SEMI, ";");
        return new_node(N_BREAK, line);
    }
    if (cur.kind == T_CONTINUE) {
        advance();
        expect(T_SEMI, ";");
        return new_node(N_CONTINUE, line);
    }
    if (cur.kind == T_RETURN) {
        advance();
        n = new_node(N_RETURN, line);
        if (cur.kind == T_DOTDOT) {
            advance();
            n->kind = N_RETURN_PUSH;
            n->a = parse_expr();
        } else if (cur.kind != T_SEMI) {
            struct node **t = &n->a;
            for (;;) {
                e = parse_assign();
                *t = e;
                t = &e->next;
                if (!accept(T_COMMA))
                    break;
            }
        }
        expect(T_SEMI, ";");
        return n;
    }

    /* destructuring: (targets) = expr; */
    if (cur.kind == T_LPAREN) {
        struct token t1 = lex_peek();
        int is_destruct = 0;

        if (t1.kind == T_UNDERSCORE) {
            is_destruct = 1;
        } else if (t1.kind == T_IDENT) {
            struct token t2 = lex_peek2();
            if (t2.kind == T_COMMA || t2.kind == T_DOTDOT)
                is_destruct = 1;
        }
        if (is_destruct) {
            int dline = cur.line;
            advance();
            return parse_destruct_stmt(dline);
        }
    }

    if (accept(T_SEMI))
        return new_node(N_EXPR_STMT, line);

    e = parse_expr();
    expect(T_SEMI, ";");
    n = new_node(N_EXPR_STMT, line);
    n->a = e;
    return n;
}

/****************************************************************
 * Top level
 ****************************************************************/

static struct node *
parse_function(void)
{
    struct node *fn, *p, **ptail;
    int line, pline, elem;

    line = cur.line;
    advance();

    if (cur.kind != T_IDENT)
        die("parse:%d: expected function name", cur.line);
    fn = new_node(N_FUNC, line);
    fn->name = cur.sval;
    advance();

    expect(T_LPAREN, "(");
    ptail = &fn->a;
    if (cur.kind != T_RPAREN) {
        for (;;) {
            pline = cur.line;
            elem = ELEM_INT;
            if (cur.kind == T_INT) {
                advance();
            } else if (cur.kind == T_BYTE) {
                elem = ELEM_BYTE;
                advance();
            } else {
                die("parse:%d: expected parameter type", cur.line);
            }
            if (cur.kind != T_IDENT)
                die("parse:%d: expected parameter name", cur.line);
            p = new_node(N_PARAM, pline);
            p->name = cur.sval;
            p->elem = elem;
            advance();
            if (accept(T_LBRACK)) {
                expect(T_RBRACK, "]");
                p->arr_size = -1;
            }
            *ptail = p;
            ptail = &p->next;
            if (!accept(T_COMMA))
                break;
        }
    }
    expect(T_RPAREN, ")");

    fn->ret_mode = RET_VOID;
    fn->ret_count = 0;
    if (accept(T_ARROW)) {
        if (cur.kind == T_INT) {
            advance();
            if (accept(T_DOTDOT)) {
                fn->ret_mode = RET_VAR;
            } else {
                fn->ret_mode = RET_ONE;
            }
        } else if (cur.kind == T_NUMBER) {
            fn->ret_count = (int)cur.nval;
            advance();
            if (accept(T_DOTDOT)) {
                fn->ret_mode = RET_VAR_MIN;
            } else {
                fn->ret_mode = RET_FIXED;
            }
        } else {
            die("parse:%d: expected return type after ->", cur.line);
        }
    }

    fn->b = parse_compound();
    return fn;
}

static struct node *
parse_top_decl(void)
{
    if (cur.kind == T_DEFINE)
        return parse_function();
    if (cur.kind == T_INT || cur.kind == T_BYTE)
        return parse_var_decl();
    die("parse:%d: expected function definition or declaration",
        cur.line);
    return NULL;
}

/****************************************************************
 * Entry point
 ****************************************************************/

struct node *
parse_program(struct arena *a)
{
    struct node *prog, *head, **tail, *d;

    parse_arena = a;
    advance();
    head = NULL;
    tail = &head;
    while (cur.kind != T_EOF) {
        d = parse_top_decl();
        *tail = d;
        tail = &d->next;
    }
    prog = new_node(N_PROGRAM, 1);
    prog->a = head;
    return prog;
}
