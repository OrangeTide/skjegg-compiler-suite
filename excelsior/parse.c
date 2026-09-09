/* parse.c : recursive-descent parser for Excelsior.
 *
 * Produces the AST in excelsior.h from the token stream. Follows
 * grammar.ebnf. Statements are newline-terminated (the lexer emits
 * T_NL); blocks close with per-kind terminators.
 *
 * Deferred for a later pass (noted TODO):
 * string ${..} holes, and section-level error resynchronisation.
 * Errors currently stop at the first offence.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "excelsior.h"

static struct arena *pa;
static const char *pfile;

/* Nonzero while parsing an `if` / `elseif` condition or a loop header, where
 * a top-level `then` / `do` closes the statement (then-in-if.md). Bracketed
 * subexpressions clear it: inside `( )` a `then` is an if-expression again. */
static int in_cond;

/* Nonzero inside a `macro ... endmacro` body, where a builtin type keyword is
 * an expression naming that type (typed-macros.md). Meta tier only. */
static int in_macro;

/****************************************************************
 * Token helpers
 ****************************************************************/

static int
at(int k)
{
    return lex_peek().kind == k;
}

static struct token
advance(void)
{
    return lex_next();
}

static int
accept(int k)
{
    if (at(k)) {
        lex_next();
        return 1;
    }
    return 0;
}

static NORETURN void
perr(const char *fmt, ...)
{
    struct token t = lex_peek();
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    die("%s:%d: %s, found `%s`", pfile, t.line, msg, tok_str(t.kind));
}

static struct token
expect(int k)
{
    if (!at(k))
        perr("expected `%s`", tok_str(k));
    return lex_next();
}

/* End of a simple statement: a newline, end of file, or the terminator of
 * the enclosing block. The last case is the one-line guard `if C then STMT
 * endif` (then-in-if.md D4): after `then` / `do` a single statement may sit
 * inline, and the block terminator closes it in place of a newline. The
 * terminator is left for the block to consume. */
static void
end_stmt(void)
{
    if (at(T_NL)) {
        lex_next();
        return;
    }
    switch (lex_peek().kind) {
    case T_EOF:
    case T_ENDIF: case T_ELSE: case T_ELSEIF:
    case T_ENDWHILE: case T_ENDFOR:
        return;
    default:
        perr("expected end of line");
    }
}

static void
skip_nl(void)
{
    while (at(T_NL))
        lex_next();
}

/* at the contextual `on fail` statement suffix (fallible-consumers.md):
 * a bare identifier `on` followed by the `fail` keyword */
static int
at_on_fail(void)
{
    struct token t = lex_peek();

    if (t.kind != T_IDENT || t.slen != 2 ||
        t.sval[0] != 'o' || t.sval[1] != 'n')
        return 0;
    return lex_peek2().kind == T_FAIL;
}

/****************************************************************
 * AST helpers
 ****************************************************************/

static struct node *
nn(int kind)
{
    struct node *n = arena_zalloc(pa, sizeof *n);
    n->kind = kind;
    n->line = lex_peek().line;
    return n;
}

/* append node to a singly linked list built with a tail pointer */
struct list {
    struct node *head;
    struct node **tail;
};

static void
list_init(struct list *l)
{
    l->head = NULL;
    l->tail = &l->head;
}

static void
list_add(struct list *l, struct node *n)
{
    *l->tail = n;
    l->tail = &n->next;
}

/****************************************************************
 * Types
 ****************************************************************/

static struct node *parse_expr(void);
static struct node *parse_params(void);
static struct node *parse_interface(void);
static struct node *parse_funclit(void);
static struct node *parse_stmts(const int *terms);

static struct ex_type *
new_type(int kind)
{
    struct ex_type *t = arena_zalloc(pa, sizeof *t);
    t->kind = kind;
    return t;
}

/* a possibly module-qualified name: IDENT ( "." IDENT )* */
static char *
parse_qualified_name(void)
{
    char buf[256];
    int len;
    struct token id = expect(T_IDENT);

    len = snprintf(buf, sizeof buf, "%s", id.sval);
    while (at(T_DOT)) {
        advance();
        id = expect(T_IDENT);
        len += snprintf(buf + len, sizeof buf - (size_t)len, ".%s", id.sval);
    }
    return arena_strndup(pa, buf, (size_t)len);
}

static struct ex_type *
parse_type(void)
{
    struct token t = lex_peek();
    struct ex_type *ty;

    switch (t.kind) {
    case T_TOBJ:
        advance();
        if (at(T_WITH)) {
            /* `obj with (open, close)`: an object slice, a structural verb
             * subset (object-slices.md D1). The same word the record field
             * slice uses; the base says which reading applies. */
            struct list vs;
            advance();
            expect(T_LPAREN);
            list_init(&vs);
            for (;;) {
                struct node *v = nn(N_NAME);
                v->name = expect(T_IDENT).sval;
                list_add(&vs, v);
                if (!accept(T_COMMA))
                    break;
            }
            expect(T_RPAREN);
            ty = new_type(ET_SLICE);
            ty->verbs = vs.head;
            return ty;
        }
        return new_type(T_TOBJ);
    case T_TINT: case T_TFLOAT: case T_TDEC: case T_TVEC: case T_TMAT:
    case T_TSTR: case T_TBOOL: case T_TERR: case T_TPROP:
        advance();
        return new_type(t.kind);
    case T_FUNC: {
        /* a func value's type: `func(T1, T2) returns U` (function-values.md
         * D3). The parameters are bare types (positional, no names); the
         * result is optional. Held as an ET_FUNC whose `verbs` is a list of
         * N_PARAM nodes carrying the parameter types and `inner` the result. */
        struct list ps;
        advance();
        expect(T_LPAREN);
        list_init(&ps);
        if (!at(T_RPAREN)) {
            for (;;) {
                struct node *p = nn(N_PARAM);
                p->type = parse_type();
                list_add(&ps, p);
                if (!accept(T_COMMA))
                    break;
            }
        }
        expect(T_RPAREN);
        ty = new_type(ET_FUNC);
        ty->verbs = ps.head;
        if (accept(T_RETURNS))
            ty->inner = parse_type();
        return ty;
    }
    case T_TLIST:
        advance();
        expect(T_LT);
        ty = new_type(T_TLIST);
        ty->inner = parse_type();
        expect(T_GT);
        return ty;
    case T_TMAYBE:
        /* maybe T: a T or nothing (fallible.md). Restrictions live
         * here so the message lands on the spelling. */
        advance();
        ty = new_type(ET_MAYBE);
        ty->inner = parse_type();
        switch (ty->inner->kind) {
        case T_TINT: case T_TBOOL: case T_TDEC:
        case T_TSTR: case T_TLIST:
            return ty;
        case T_TOBJ:
            perr("maybe obj is not needed: an absent object is `nil`");
        case ET_MAYBE:
            perr("maybe maybe does not nest; one maybe is enough");
        default:
            perr("maybe of this type is not supported yet (int, bool, "
                 "decimal, str, and list<T> are)");
        }
    case T_HOLE:
        /* `${T}` in a type position: a type-parametric template
         * (typed-macros.md D4). Macro bodies only, like the other meta
         * spellings; form_build substitutes the type it evaluates to. */
        if (!in_macro)
            perr("expected a type");
        advance();
        ty = new_type(ET_TYPEHOLE);
        ty->hole = parse_expr();
        expect(T_HOLE_END);
        return ty;
    case T_IDENT:
        /* `set of E`: a bitmask over an enum universe (set-of.md). `set` and
         * `of` stay ordinary identifiers (the `set` list builtin); this is a
         * contextual type form, recognized only as `set of ...`. */
        if (ex_ci_eq(t.sval, "set") && lex_peek2().kind == T_IDENT &&
            ex_ci_eq(lex_peek2().sval, "of")) {
            advance();              /* set */
            advance();              /* of */
            ty = new_type(T_TSET);
            ty->inner = parse_type();
            return ty;
        }
        /* `source of T` (sources.md D4): the failable-continuation source.
         * Contextual like `set of`, so `source` stays an ordinary
         * identifier elsewhere. The element is word-sized, the same set a
         * `maybe` accepts, since the pump yields through the null-word
         * maybe protocol. */
        if (ex_ci_eq(t.sval, "source") && lex_peek2().kind == T_IDENT &&
            ex_ci_eq(lex_peek2().sval, "of")) {
            struct ex_type *in;
            advance();              /* source */
            advance();              /* of */
            in = parse_type();
            switch (in->kind) {
            case T_TINT: case T_TBOOL: case T_TDEC:
            case T_TSTR: case T_TLIST:
                break;
            default:
                perr("a source's element must be int, bool, decimal, str, "
                     "or a list (the word-sized set a maybe accepts)");
            }
            ty = new_type(ET_SOURCE);
            ty->inner = in;
            return ty;
        }
        ty = new_type(T_IDENT);
        ty->name = parse_qualified_name();
        return ty;
    default:
        perr("expected a type");
    }
}

/****************************************************************
 * Expressions
 ****************************************************************/

static struct node *parse_expr(void);
static struct node *parse_postfix(void);
static struct node *parse_or_level(void);
static struct node *parse_match_expr(void);
static struct node *bin(int op, struct node *a, struct node *b);
static void guard_bare_select(void);

/* An interface declaration (interface-decl.md D1/D2): a named object slice
 * whose body is a list of verb signatures, the same bodyless `verb` form an
 * `.exi` carries. The verb nodes hang off an ET_SLICE type, so a named
 * interface and an inline `obj with (...)` slice are one thing to everything
 * downstream; the named one just carries signatures too. */
static struct node *
parse_interface(void)
{
    struct node *n = nn(N_INTERFACE);
    struct ex_type *ty = new_type(ET_SLICE);
    struct list vs;

    expect(T_INTERFACE);
    n->name = expect(T_IDENT).sval;
    expect(T_NL);
    list_init(&vs);
    for (;;) {
        struct node *v;
        skip_nl();
        if (at(T_ENDINTERFACE) || at(T_EOF))
            break;
        if (!at(T_VERB))
            perr("an interface body is one `verb` signature per line");
        advance();
        v = nn(N_VERB);
        v->name = expect(T_IDENT).sval;
        expect(T_LPAREN);
        v->a = at(T_RPAREN) ? NULL : parse_params();
        expect(T_RPAREN);
        if (accept(T_RETURNS))
            v->type = parse_type();
        end_stmt();
        list_add(&vs, v);
    }
    expect(T_ENDINTERFACE);
    end_stmt();
    ty->verbs = vs.head;
    n->type = ty;
    return n;
}

/* One argument: a named `name: value` (a record field, or a named function/verb
 * argument; the name is carried on the value node's `alias`), or a plain
 * positional expression. `:` over `=` so a named argument never reads as an
 * assignment and is never one keystroke from `==`. `IDENT :` here is distinct
 * from the retired postfix colon-send, which follows an expression. */
static struct node *
parse_arg(void)
{
    switch (lex_peek().kind) {
    case T_TINT: case T_TSTR: case T_TBOOL: case T_TDEC:
    case T_TFLOAT: case T_TOBJ: {
        /* a builtin type as an argument, for a type-parametric macro
         * (`convert(3, float)`, typed-macros.md D4). Only where a type can
         * complete the argument, so nothing else changes shape; the checker
         * rejects one that reaches an ordinary call. A named type arrives as
         * an IDENT and needs no special case here. */
        int k2 = lex_peek2().kind;
        if (k2 == T_COMMA || k2 == T_RPAREN) {
            struct node *n = nn(N_TYPELIT);
            n->type = parse_type();
            return n;
        }
        break;
    }
    default:
        break;
    }
    if (at(T_IDENT) && lex_peek2().kind == T_COLON) {
        char *aname = expect(T_IDENT).sval;
        struct node *v;
        expect(T_COLON);
        v = parse_expr();
        v->alias = aname;
        return v;
    }
    return parse_expr();
}

static struct node *
parse_arg_list(void)
{
    struct list args;
    int outer = in_cond;

    in_cond = 0;                    /* an argument is inside brackets */
    list_init(&args);
    if (!at(T_RPAREN)) {
        guard_bare_select();
        list_add(&args, parse_arg());
        while (accept(T_COMMA)) {
            guard_bare_select();
            list_add(&args, parse_arg());
        }
    }
    in_cond = outer;
    return args.head;
}

/* Is the `[...]` at the cursor a comprehension? Scan to the matching `]`,
 * looking for a `for` at the comprehension's own bracket level (a nested
 * `[...]` in the head hides its own `for` at a deeper depth). The lexer state
 * is saved and restored, so this is pure lookahead. */
static int
lbrack_is_comprehension(void)
{
    struct lex_state save;
    int depth = 0, found = 0;

    lex_save(&save);
    for (;;) {
        int k = lex_peek().kind;
        if (k == T_EOF)
            break;
        if (k == T_LBRACK || k == T_LPAREN) {
            depth++;
        } else if (k == T_RBRACK || k == T_RPAREN) {
            if (--depth == 0)
                break;              /* the matching close bracket */
        } else if (k == T_FOR && depth == 1) {
            found = 1;
            break;
        }
        lex_next();
    }
    lex_restore(&save);
    return found;
}

/* `[HEAD for NAME in SOURCE (if COND)?]` (function-values.md D5): the one
 * comprehension form for now, a single binder over a list or str source. It
 * lowers to a `for` loop building a list (the lowering synthesizes the loop).
 * Multiple `for` clauses, a range source, and a nested-head matrix are the
 * deferred follow-ons. */
static struct node *
parse_comprehension(void)
{
    struct node *n = nn(N_COMP);

    expect(T_LBRACK);
    n->a = parse_expr();                /* the head expression */
    expect(T_FOR);
    n->name = expect(T_IDENT).sval;     /* the binder */
    expect(T_IN);
    n->b = parse_expr();                /* the source: a list, str, or range */
    if (accept(T_TO)) {                 /* `for i in lo to hi`: a range */
        struct node *r = nn(N_RANGE);
        r->a = n->b;
        r->b = parse_expr();
        n->b = r;
    }
    if (at(T_FOR))
        perr("a comprehension takes one `for` clause for now; a nested "
             "iteration is not lowered yet");
    if (accept(T_IF))
        n->c = parse_expr();            /* the optional filter */
    expect(T_RBRACK);
    return n;
}

static struct node *
parse_data_literal(void)
{
    struct node *n = nn(N_DATALIT);
    struct list items;
    struct token t;

    expect(T_LBRACK);
    list_init(&items);
    while (!at(T_RBRACK) && !at(T_EOF)) {
        struct node *it;
        t = lex_peek();
        switch (t.kind) {
        case T_LBRACK:
            it = parse_data_literal();
            break;
        case T_IDENT:
            if (lex_peek2().kind == T_LPAREN) {
                /* `Ident(...)` is a record construction (or call) element: a
                 * computed value, not a quoted atom, so it lowers like a
                 * ${} hole (list<Point>, records in a value list) */
                it = nn(N_HOLE);
                it->a = parse_postfix();
            } else {
                advance();
                it = nn(N_NAME);
                it->name = t.sval;
            }
            break;
        case T_TO:
            /* `to` is an ordinary linking word inside data, not grammar
             * (`[choice "Goodbye" to end]`, typed-data.md); it reads as a
             * plain atom here even though it is a keyword elsewhere. */
            advance();
            it = nn(N_NAME);
            it->name = "to";
            break;
        case T_NUMBER:
            advance();
            it = nn(N_NUM);
            it->ival = t.nval;
            break;
        case T_FLOAT_LIT:
            advance();
            it = nn(N_FLOAT);
            it->fval = t.fval;
            break;
        case T_STRING:
            advance();
            it = nn(N_STR);
            it->sval = t.sval;
            it->slen = t.slen;
            break;
        case T_HOLE:
            /* ${expr}: a computed element spliced into the literal;
             * the one escape hatch of the quoted world (sequences.md) */
            advance();
            it = nn(N_HOLE);
            it->a = parse_expr();
            expect(T_HOLE_END);
            break;
        default:
            perr("unexpected token in data literal");
        }
        list_add(&items, it);
    }
    expect(T_RBRACK);
    n->a = items.head;
    return n;
}

static struct node *
str_lit(struct token t)
{
    struct node *n = nn(N_STR);
    n->sval = t.sval;
    n->slen = t.slen;
    return n;
}

/* An interpolated string `"...${e}...${e}..."` is a chain of string-concat:
 * each literal piece is an N_STR; each hole is an ordinary expression,
 * stringified by static type (N_TOSTR). The old `${cond ? a : b}` pick
 * became the general `cond then A else B` if-expression, which is just
 * an expr here like anywhere else. The lexer feeds the literal pieces as
 * T_ISTR_PIECE and a final T_STRING; hole tokens arrive between them. */
static struct node *
parse_interp(void)
{
    struct node *acc = str_lit(advance());      /* leading T_ISTR_PIECE */

    for (;;) {
        struct node *hole = nn(N_TOSTR);

        hole->a = parse_expr();
        acc = bin(T_PLUS, acc, hole);
        if (at(T_ISTR_PIECE)) {                 /* another hole follows */
            acc = bin(T_PLUS, acc, str_lit(advance()));
            continue;
        }
        acc = bin(T_PLUS, acc, str_lit(expect(T_STRING)));
        break;
    }
    return acc;
}

/* An unparenthesized `select` at the head of a comma-list element (a call
 * argument or another select's branch) would eat the following commas as
 * its own branches (R4, parse-traps.md). Reject it at the offending token
 * with the parenthesize fix, rather than letting it silently swallow the
 * rest of the list into a later, misleading arity or type error. */
static void
guard_bare_select(void)
{
    if (at(T_SELECT))
        perr("a `select` in a comma list must be parenthesized; its "
             "branch commas are greedy and would otherwise eat the "
             "following items: `f((select i from a, b), x)`");
}

/* `select IDX from E0, E1, ..., EN [ else DEFAULT ]` : a positional select.
 * IDX (an int) picks the Ith branch; an index outside 0..N yields DEFAULT, or
 * traps at runtime when no `else` is given. Branch and else expressions share
 * a type. Only the chosen branch evaluates. The branch commas are greedy, so
 * a select passed as one of several call arguments needs parentheses. */
static struct node *
parse_select(void)
{
    struct node *n = nn(N_SELECT);
    struct list branches;

    advance();                          /* select */
    n->a = parse_or_level();            /* index (above the else operator) */
    expect(T_FROM);
    list_init(&branches);
    guard_bare_select();
    list_add(&branches, parse_or_level());
    while (accept(T_COMMA)) {
        guard_bare_select();
        list_add(&branches, parse_or_level());
    }
    n->b = branches.head;               /* branch list, chained by ->next */
    if (accept(T_OTHERWISE))
        n->c = parse_expr();            /* optional default (fallback-words.md) */
    return n;
}

static struct node *
parse_primary(void)
{
    struct token t = lex_peek();
    struct node *n;

    switch (t.kind) {
    case T_SELECT:
        return parse_select();
    case T_MATCH:
        return parse_match_expr();
    case T_NUMBER:
        advance();
        n = nn(N_NUM);
        n->ival = t.nval;
        return n;
    case T_FLOAT_LIT:
        advance();
        n = nn(N_FLOAT);
        n->fval = t.fval;
        return n;
    case T_STRING:
        advance();
        n = nn(N_STR);
        n->sval = t.sval;
        n->slen = t.slen;
        return n;
    case T_ISTR_PIECE:
        return parse_interp();
    case T_TRUE:
    case T_FALSE:
        advance();
        n = nn(N_BOOL);
        n->ival = (t.kind == T_TRUE);
        return n;
    case T_TINT: case T_TSTR: case T_TBOOL: case T_TDEC:
    case T_TFLOAT: case T_TOBJ:
        /* A builtin type names itself in a macro body, so a typed macro can
         * compare against one (`typeof(x) = int`, typed-macros.md). Only
         * there: in ordinary code a type keyword is not an expression, and
         * making it one would change what a bare `int` means everywhere. */
        if (!in_macro)
            perr("expected an expression");
        n = nn(N_TYPELIT);
        n->type = parse_type();
        return n;
    case T_NIL:
        advance();
        return nn(N_NIL);
    case T_NOTHING:
        advance();
        return nn(N_NOTHING);
    case T_SELF:
        advance();
        return nn(N_SELF);
    case T_IDENT:
        advance();
        n = nn(N_NAME);
        n->name = t.sval;
        return n;
    case T_FUNC:
        /* `func(params) returns T ... endfunc`: an anonymous function value
         * (function-values.md D1). The named form `func name(...)` is a class
         * member, parsed elsewhere; here in expression position it is a
         * lambda, always straight to `(`. */
        return parse_funclit();
    case T_LBRACK:
        /* `[e for x in xs if c]` is a comprehension, `[1 2 3]` a data literal.
         * A top-level `for` inside the brackets tells them apart, and `for`
         * is a keyword that cannot be a data atom, so the scan is exact. */
        if (lbrack_is_comprehension())
            return parse_comprehension();
        return parse_data_literal();
    case T_QUOTE:
        /* quote FORM: reify a form as data (meta.md). The form is a full
         * expression; its names are resolved after the macro splices it. */
        advance();
        n = nn(N_QUOTE);
        n->a = parse_expr();
        return n;
    case T_QUASI:
        /* quasi FORM: a template whose `${}` holes splice meta values */
        advance();
        n = nn(N_QUASI);
        n->a = parse_expr();
        return n;
    case T_HOLE:
        /* ${meta} splice inside a quote/quasi form */
        advance();
        n = nn(N_HOLE);
        n->a = parse_expr();
        expect(T_HOLE_END);
        return n;
    case T_LPAREN: {
        /* parentheses end the condition region for what is inside them, so a
         * parenthesized if-expression reads normally there (then-in-if.md D5) */
        int outer = in_cond;

        advance();
        in_cond = 0;
        n = parse_expr();
        in_cond = outer;
        expect(T_RPAREN);
        return n;
    }
    default:
        perr("expected an expression");
    }
}

static struct node *
parse_postfix(void)
{
    struct node *n = parse_primary();

    for (;;) {
        struct token t = lex_peek();
        struct node *m;

        if (t.kind == T_DOT) {
            /* `.name` reads a field (which may hold a function value); a
             * trailing `(` on that field is a call-through-field, built by
             * the T_LPAREN arm below as a call whose callee is this read. */
            advance();
            m = nn(N_FIELD_ACC);
            m->a = n;
            /* a member name is an identifier; the meta accessors `.type` and
             * `.tags` spell it with a keyword, allowed here after the dot */
            if (at(T_TYPE) || at(T_TAGS)) {
                m->name = (char *)tok_str(lex_peek().kind);
                advance();
            } else {
                m->name = expect(T_IDENT).sval;
            }
            n = m;
        } else if (t.kind == T_COLON) {
            /* `:` left the expression language (verbs.md): sends use the
             * dot, and the old ratio literal became fixed()/0f. */
            advance();
            if (at(T_NUMBER))
                perr("the num:den ratio literal was replaced; "
                     "write the decimal value plainly (numbers.md)");
            perr("there is no `:` send; write recv.verb(args)");
        } else if (t.kind == T_LPAREN) {
            advance();
            m = nn(N_CALL);
            m->a = n;
            m->b = parse_arg_list();
            expect(T_RPAREN);
            n = m;
        } else if (t.kind == T_LBRACK) {
            advance();
            m = parse_expr();
            if (accept(T_TO)) {
                struct node *s = nn(N_SLICE);
                s->a = n;
                s->b = m;
                s->c = parse_expr();
                expect(T_RBRACK);
                n = s;
            } else {
                struct node *ix = nn(N_INDEX);
                ix->a = n;
                ix->b = m;
                expect(T_RBRACK);
                n = ix;
            }
        } else if (t.kind == T_AS) {
            advance();
            m = nn(N_CAST);
            m->a = n;
            m->type = parse_type();
            n = m;
        } else if (t.kind == T_WITH) {
            /* `p with (x, y)`: a record field slice (record-slicing.md). The
             * fields are names, carried as N_NAME nodes in ->b. */
            struct list fs;
            advance();
            expect(T_LPAREN);
            m = nn(N_WITH);
            m->a = n;
            list_init(&fs);
            if (!at(T_RPAREN)) {
                struct node *fn = nn(N_NAME);
                fn->name = expect(T_IDENT).sval;
                list_add(&fs, fn);
                while (accept(T_COMMA)) {
                    fn = nn(N_NAME);
                    fn->name = expect(T_IDENT).sval;
                    list_add(&fs, fn);
                }
            }
            m->b = fs.head;
            expect(T_RPAREN);
            n = m;
        } else {
            break;
        }
    }
    return n;
}

static struct node *
parse_unary(void)
{
    if (at(T_MINUS)) {
        struct node *n = nn(N_UNOP);
        n->op = T_MINUS;
        advance();
        n->a = parse_unary();
        return n;
    }
    return parse_postfix();
}

static struct node *
bin(int op, struct node *a, struct node *b)
{
    struct node *n = nn(N_BINOP);
    n->op = op;
    n->a = a;
    n->b = b;
    return n;
}

static struct node *
parse_mul(void)
{
    struct node *n = parse_unary();
    while (at(T_STAR) || at(T_SLASH) || at(T_PERCENT)) {
        int op = advance().kind;
        n = bin(op, n, parse_unary());
    }
    return n;
}

static struct node *
parse_add(void)
{
    struct node *n = parse_mul();
    while (at(T_PLUS) || at(T_MINUS) || at(T_CARET)) {   /* ^ is set toggle */
        int op = advance().kind;
        n = bin(op, n, parse_mul());
    }
    return n;
}

static struct node *
parse_membership(void)
{
    struct node *n = parse_add();

    if (at(T_IN)) {
        advance();
        return bin(T_IN, n, parse_add());
    }
    if (at(T_OVERLAPS)) {           /* set intersection test (set-of.md D6) */
        advance();
        return bin(T_OVERLAPS, n, parse_add());
    }
    if (at(T_IS)) {
        struct node *m = nn(N_ISTEST);
        advance();
        m->a = n;
        m->name = parse_qualified_name();
        return m;
    }
    return n;
}

/* equality is `=` (T_ASSIGN in an expression; the binding `=` is eaten at
 * statement level before the expression parser, equality.md), inequality `<>`
 * (T_NE), and the four orderings. */
static int
at_cmp_op(void)
{
    return at(T_ASSIGN) || at(T_NE) || at(T_LT) || at(T_LE) ||
           at(T_GT) || at(T_GE);
}

/* the AST op for a comparison token: `=` (T_ASSIGN) is the equality op T_EQ */
static int
cmp_op(void)
{
    int k = advance().kind;
    return k == T_ASSIGN ? T_EQ : k;
}

/* One comparison is a plain binop. Two or more chain: `a < b < c` means
 * `a < b and b < c` with each middle operand evaluated once and the
 * chain short-circuiting on the first false. Direction rules (same-way
 * only, no `!=`) are enforced by the type checker. */
static struct node *
parse_comparison(void)
{
    struct node *n = parse_membership();
    struct node *rhs, *chain, *l;
    struct list links;
    int op;

    if (!at_cmp_op())
        return n;
    op = cmp_op();
    rhs = parse_membership();
    if (!at_cmp_op())
        return bin(op, n, rhs);

    chain = nn(N_CMPCHAIN);
    chain->a = n;
    list_init(&links);
    l = nn(N_CMPLINK);
    l->op = op;
    l->a = rhs;
    list_add(&links, l);
    while (at_cmp_op()) {
        l = nn(N_CMPLINK);
        l->op = cmp_op();
        l->a = parse_membership();
        list_add(&links, l);
    }
    chain->b = links.head;
    return chain;
}

static struct node *
parse_not(void)
{
    if (at(T_NOT)) {
        struct node *n = nn(N_UNOP);
        n->op = T_NOT;
        advance();
        n->a = parse_not();
        return n;
    }
    return parse_comparison();
}

static struct node *
parse_and(void)
{
    struct node *n = parse_not();
    while (at(T_AND)) {
        advance();
        n = bin(T_AND, n, parse_not());
    }
    return n;
}

/* everything above the `else` fallback operator (or / xor and below) */
static struct node *
parse_or_level(void)
{
    struct node *n = parse_and();
    while (at(T_OR) || at(T_XOR)) {
        int op = advance().kind;
        n = bin(op, n, parse_and());
    }
    return n;
}

/* At a `then` inside a condition region: is this the start of an
 * if-expression, or the head keyword's closer? The if-expression's `else`
 * is what tells them apart (then-in-if.md D5), so scan forward for one.
 *
 * The scan stops at the end of the line, at a second top-level `then` (the
 * statement's own closer, so any `else` past it belongs elsewhere), at a
 * structural keyword, or at the `)` of an enclosing group. Newlines are
 * visible to the scan because the condition region is left first, and the
 * whole lexer position is rewound afterwards. */
static int
then_has_else(void)
{
    struct lex_state save;
    int depth = 0, found = 0;

    lex_save(&save);
    lex_cond_end();                 /* let the scan see the line break */
    lex_next();                     /* the `then` itself */
    for (;;) {
        int k = lex_peek().kind;

        if (k == T_LPAREN || k == T_LBRACK) {
            depth++;
        } else if (k == T_RPAREN || k == T_RBRACK) {
            if (--depth < 0)
                break;              /* the end of an enclosing group */
        } else if (depth == 0) {
            if (k == T_ELSE) {
                found = 1;
                break;
            }
            if (k == T_NL || k == T_EOF || k == T_THEN || k == T_DO)
                break;
            switch (k) {            /* a structural keyword bounds the scan */
            case T_IF: case T_ELSEIF: case T_ENDIF:
            case T_WHILE: case T_ENDWHILE: case T_FOR: case T_ENDFOR:
            case T_MATCH: case T_WHEN: case T_ENDMATCH:
            case T_VAR: case T_RETURN: case T_BREAK: case T_CONTINUE:
            case T_TRACE: case T_ENDVERB: case T_ENDFUNC:
                lex_restore(&save);
                return 0;
            default:
                break;
            }
        }
        lex_next();
    }
    lex_restore(&save);
    return found;
}

/* The lowest precedence tier, right associative.
 *
 * `cond then A else B` is the if-expression: cond is bool, exactly one
 * branch evaluates, and the `else` is REQUIRED (a missing "otherwise"
 * is a compile error, not a runtime trap). The else branch is a full
 * expr, so `a then x else b then y else z` chains like elseif.
 *
 * `A otherwise B` is the fallback operator: `A` is a fallible
 * expression; when it has no value, `B` (fallback-words.md). `a
 * otherwise b otherwise c` is `a otherwise (b otherwise c)`, first value
 * wins. `else` is only the boolean branch (the `if` statement and the
 * `cond then A else B` if-expression); it never means a value fallback.
 * `select` parses its branches with parse_or_level, so its own trailing
 * `otherwise` is not swallowed here.
 *
 * Inside a condition region the `then` is usually the head keyword's own
 * closer, not an if-expression, so it is taken here only when an `else`
 * follows it on the line (then-in-if.md D5). */
static struct node *
parse_expr(void)
{
    struct node *n = parse_or_level();

    if (at(T_THEN) && in_cond) {
        if (!then_has_else())
            return n;                    /* the `then` closes the condition */
        /* The one rule, the same one a misplaced if-expression breaks
         * anywhere else: it binds loosest, so it is parenthesized when it is
         * not the whole expression. Here that is visible as two `then`s on
         * one line, which no reader can sort out (then-in-if.md D5). */
        perr("an if-expression binds loosest, so it is parenthesized when it "
             "is not the whole expression: write `if (cond then A else B) "
             "then`, and the outer `then` closes the statement");
    }
    if (accept(T_THEN)) {
        struct node *e = nn(N_THENELSE);
        e->a = n;
        e->b = parse_or_level();
        if (!accept(T_ELSE))
            perr("`then` needs its `else` (the if-expression is `cond "
                 "then A else B`; an `if` statement takes no `then`)");
        e->c = parse_expr();
        return e;
    }
    if (accept(T_OTHERWISE)) {           /* the value fallback operator */
        struct node *e = nn(N_OTHERWISE);
        e->a = n;
        e->b = parse_expr();
        n = e;
    }
    /* a bare expression is never legitimately followed by `else` (a real
     * `else` follows `then A` above, or a newline and a body in an `if`
     * statement), so `else` here is the old fallback spelling. Teach the
     * rename rather than let it surface as "expected end of line". */
    if (at(T_ELSE))
        perr("unexpected `else`; it is only the boolean branch (an `if` "
             "statement, or `cond then A else B`). For a value fallback "
             "the operator is now `otherwise`: write `A otherwise B`");
    return n;
}

/****************************************************************
 * Statements
 ****************************************************************/

static struct node *parse_stmt(void);

/* parse statements until the current token is one of the terminators */
static struct node *
parse_stmts(const int *terms)
{
    struct list body;

    list_init(&body);
    for (;;) {
        int i;

        skip_nl();
        if (at(T_EOF))
            break;
        for (i = 0; terms[i] != 0; i++)
            if (at(terms[i]))
                break;
        if (terms[i] != 0)
            break;
        list_add(&body, parse_stmt());
    }
    return body.head;
}

static struct node *
parse_var(void)
{
    struct node *n = nn(N_VAR);

    expect(T_VAR);
    n->name = expect(T_IDENT).sval;
    if (accept(T_IS))
        n->type = parse_type();
    if (accept(T_ASSIGN))
        n->a = parse_expr();
    if (!n->type && !n->a)
        perr("`var %s` needs a type or an initializer", n->name);
    end_stmt();
    return n;
}

/* A condition or loop header: a newline-transparent region closed by an
 * explicit word (then-in-if.md D1/D2/D3). `closer` is T_THEN or T_DO;
 * `what` names the region in the migration message (D6), which is local
 * because the region scan stops at the head keyword's own line. */
static struct node *
parse_cond(int closer, const char *what)
{
    struct node *e;
    int outer = in_cond;

    lex_cond_begin();
    in_cond = 1;
    e = parse_expr();
    in_cond = outer;
    lex_cond_end();
    if (!at(closer))
        perr("%s ends with `%s`; add `%s` after it", what, tok_str(closer),
             tok_str(closer));
    lex_next();                     /* the closer */
    return e;
}

static struct node *
parse_if(int is_elseif)
{
    static const int terms[] = { T_ELSEIF, T_ELSE, T_ENDIF, 0 };
    struct node *n = nn(N_IF);

    expect(is_elseif ? T_ELSEIF : T_IF);
    if (accept(T_VAR)) {            /* if var i = fallible-expr */
        n->name = expect(T_IDENT).sval;
        expect(T_ASSIGN);
    }
    n->a = parse_cond(T_THEN, is_elseif ? "an `elseif` condition" : "an `if` condition");
    n->b = parse_stmts(terms);
    if (at(T_ELSEIF)) {
        n->c = parse_if(1);       /* the recursion carries any else + endif */
    } else if (accept(T_ELSE)) {
        static const int eterms[] = { T_ENDIF, 0 };
        /* `else` has no condition, so it takes no `then` (D1) */
        n->c = parse_stmts(eterms);
    }
    /* only the outermost `if` consumes the single trailing `endif` */
    if (!is_elseif) {
        expect(T_ENDIF);
        end_stmt();
    }
    return n;
}

static struct node *
parse_for(void)
{
    static const int terms[] = { T_ENDFOR, 0 };
    struct node *n = nn(N_FOR);

    expect(T_FOR);
    n->name = expect(T_IDENT).sval;
    expect(T_IN);
    /* the header is one region, so `do` closes the iterable or the
     * `to`-range, whichever the loop is (then-in-if.md D2) */
    lex_cond_begin();
    in_cond = 1;
    n->a = parse_expr();
    if (accept(T_TO))
        n->b = parse_expr();      /* range: a = lo, b = hi */
    in_cond = 0;
    if (!at(T_DO)) {
        lex_cond_end();
        perr("a `for` header ends with `do`; add `do` after it");
    }
    lex_cond_end();
    lex_next();
    n->c = parse_stmts(terms);
    expect(T_ENDFOR);
    end_stmt();
    return n;
}

static struct node *
parse_while(void)
{
    static const int terms[] = { T_ENDWHILE, 0 };
    struct node *n = nn(N_WHILE);

    expect(T_WHILE);
    if (accept(T_VAR)) {            /* while var i = fallible-expr */
        n->name = expect(T_IDENT).sval;
        expect(T_ASSIGN);
    }
    n->a = parse_cond(T_DO, "a `while` header");
    n->b = parse_stmts(terms);
    expect(T_ENDWHILE);
    end_stmt();
    return n;
}

/* one match label: a constant. A literal (optionally negated int, a 0f
 * fixed, true/false) or a name that must resolve to a module const or
 * an enum member (checked by the type checker). */
static struct node *
parse_match_label(void)
{
    struct token t = lex_peek();
    struct node *n;

    switch (t.kind) {
    case T_TINT: case T_TSTR: case T_TBOOL: case T_TDEC:
    case T_TFLOAT: case T_TOBJ:
        /* a type label, for `match typeof(x)` in a macro body
         * (typed-macros.md D3); a named type arrives as an IDENT below */
        if (!in_macro)
            perr("expected a constant match label");
        n = nn(N_TYPELIT);
        n->type = parse_type();
        return n;
    case T_MINUS:
        advance();
        n = nn(N_NUM);
        n->ival = -expect(T_NUMBER).nval;
        return n;
    case T_NUMBER:
        advance();
        n = nn(N_NUM);
        n->ival = t.nval;
        return n;
    case T_FLOAT_LIT:
        advance();
        n = nn(N_FLOAT);
        n->fval = t.fval;
        return n;
    case T_TRUE:
    case T_FALSE:
        advance();
        n = nn(N_BOOL);
        n->ival = (t.kind == T_TRUE);
        return n;
    case T_IDENT:
        advance();
        n = nn(N_NAME);
        n->name = t.sval;
        return n;
    default:
        perr("expected a constant match label");
    }
}

/* one match value: a label, or a range label .. label */
static struct node *
parse_match_val(void)
{
    struct node *e = parse_match_label();

    if (accept(T_TO)) {
        struct node *r = nn(N_RANGE);
        r->a = e;
        r->b = parse_match_label();
        return r;
    }
    return e;
}

static struct node *
parse_match_vals(void)
{
    struct list vals;

    list_init(&vals);
    list_add(&vals, parse_match_val());
    while (accept(T_COMMA))
        list_add(&vals, parse_match_val());
    return vals.head;
}

/* The subject runs from `match` to the first `when` (match-when.md D2), a
 * keyword boundary, so the newline after it is optional: `match x` on its own
 * line and `match x when ...` both read. */
static struct node *
parse_match_subject(void)
{
    struct node *subj = parse_expr();

    if (!at(T_WHEN))
        expect(T_NL);
    return subj;
}

/* Every arm opens with `when` (match-when.md D1). A label sitting where the
 * word belongs is the old arm spelling, so name the migration. */
static void
expect_when(void)
{
    if (!accept(T_WHEN))
        perr("a match arm opens with `when`: `when LABELS then BODY`");
}

/* an arm's statements: until the next `when`, `otherwise`, or `endmatch`.
 * Every arm opens with `when` (match-when.md D1/D3), so the boundary is one
 * token of lookahead and a body line can never be misread as an arm. */
static struct node *
parse_match_body(void)
{
    struct list body;

    list_init(&body);
    for (;;) {
        skip_nl();
        if (at(T_ENDMATCH) || at(T_OTHERWISE) || at(T_EOF) || at(T_WHEN))
            break;
        list_add(&body, parse_stmt());
    }
    return body.head;
}

static struct node *
parse_match(void)
{
    struct node *n = nn(N_MATCH);
    struct list arms;

    expect(T_MATCH);
    n->a = parse_match_subject();
    list_init(&arms);
    for (;;) {
        skip_nl();
        if (at(T_ENDMATCH) || at(T_EOF))
            break;
        if (at(T_OTHERWISE)) {      /* the default arm (fallback-words.md) */
            advance();
            n->c = parse_match_body();
            continue;
        }
        {
            struct node *arm = nn(N_MATCHARM);

            expect_when();
            arm->a = parse_match_vals();
            expect(T_THEN);
            arm->b = parse_match_body();
            list_add(&arms, arm);
        }
    }
    n->b = arms.head;
    expect(T_ENDMATCH);
    end_stmt();
    return n;
}

/* The match construct in expression position: the same shape, but an
 * arm body is one expression, and a match with no `otherwise` traps at
 * runtime (like select). Keyword-delimited through `endmatch`, so the
 * newlines between arms are skipped here. */
static struct node *
parse_match_expr(void)
{
    struct node *n = nn(N_MATCHEXPR);
    struct list arms;

    expect(T_MATCH);
    n->a = parse_match_subject();
    list_init(&arms);
    for (;;) {
        skip_nl();
        if (at(T_ENDMATCH) || at(T_EOF))
            break;
        if (at(T_OTHERWISE)) {      /* the default arm: `otherwise expr` */
            advance();
            n->c = parse_expr();
            continue;
        }
        {
            struct node *arm = nn(N_MATCHARM);

            expect_when();
            arm->a = parse_match_vals();
            expect(T_THEN);
            arm->b = parse_expr();
            list_add(&arms, arm);
        }
    }
    n->b = arms.head;
    expect(T_ENDMATCH);
    return n;
}

static struct node *
parse_return(void)
{
    struct node *n = nn(N_RETURN);

    expect(T_RETURN);
    if (!at(T_NL) && !at(T_EOF))
        n->a = parse_expr();
    end_stmt();
    return n;
}

static struct node *
parse_stmt(void)
{
    struct token t = lex_peek();
    struct node *n, *lhs;

    switch (t.kind) {
    case T_VAR:    return parse_var();
    case T_IF:     return parse_if(0);
    case T_FOR:    return parse_for();
    case T_WHILE:  return parse_while();
    case T_MATCH:   return parse_match();
    case T_RETURN: return parse_return();
    case T_BREAK:
        advance();
        end_stmt();
        return nn(N_BREAK);
    case T_CONTINUE:
        advance();
        end_stmt();
        return nn(N_CONTINUE);
    case T_TRACE:
        advance();
        n = nn(N_TRACE);
        n->a = parse_expr();
        end_stmt();
        return n;
    case T_FAIL:
        advance();
        end_stmt();
        return nn(N_FAIL);
    case T_YIELD:
        /* yield EXPR: produce a value and suspend until the next pump
         * (sources.md D4); legal only in a func returning `source of T`,
         * which the checker enforces */
        advance();
        n = nn(N_YIELD);
        n->a = parse_expr();
        end_stmt();
        return n;
    case T_DEFER:
        /* defer STMT: register one statement to run when the enclosing
         * block exits (defer.md). The statement consumes its own newline
         * (like the `on fail` handler), so no end_stmt() here; the
         * checker enforces what a deferred statement may be. */
        advance();
        n = nn(N_DEFER);
        n->a = parse_stmt();
        return n;
    case T_TRACE_COMMENT:
        advance();
        n = nn(N_TRACE_CMT);
        n->sval = t.sval;
        n->slen = t.slen;
        return n;
    default:
        break;
    }

    /* assignment or expression statement. The target/leading expression is a
     * postfix (an lvalue or a call), so a following `=` is the assignment
     * operator, not swallowed as an equality by the expression parser
     * (equality.md). A statement-leading full expression is a call in practice;
     * a comparison used as a bare statement, or a complex lvalue containing one,
     * takes parentheses. */
    lhs = parse_postfix();
    if (at(T_ASSIGN)) {
        advance();
        n = nn(N_ASSIGN);
        n->a = lhs;
        n->b = parse_expr();
    } else {
        n = nn(N_EXPR_STMT);
        n->a = lhs;
    }
    /* optional trailing `on fail <handler>` (fallible-consumers.md): the
     * statement-level twin of `else`. `on` is a contextual identifier, not
     * a reserved word; `fail` is already a keyword. The handler is a full
     * statement (a call, `break`, `return`, ...) that consumes its own
     * newline, so no end_stmt() follows on this path. */
    if (at_on_fail()) {
        struct node *of = nn(N_ONFAIL);
        advance();                    /* on */
        advance();                    /* fail */
        of->a = n;
        of->b = parse_stmt();
        return of;
    }
    end_stmt();
    return n;
}

/****************************************************************
 * Class members
 ****************************************************************/

/* `func(params) returns T ... endfunc` in expression position: an anonymous
 * function value (function-values.md D1). Its parameter and result types are
 * explicit (D3); a valueless func omits `returns`. The body is the same
 * statement list a named func has, closed by `endfunc`. */
static struct node *
parse_funclit(void)
{
    static const int fterms[] = { T_ENDFUNC, 0 };
    struct node *n = nn(N_FUNCLIT);
    int saved;

    expect(T_FUNC);
    expect(T_LPAREN);
    n->a = at(T_RPAREN) ? NULL : parse_params();
    expect(T_RPAREN);
    /* from here the body's newlines flow even inside a call's parens */
    saved = lex_body_begin();
    if (accept(T_RETURNS)) {
        n->type = parse_type();
        if (n->type->kind == ET_MAYBE)
            perr("a func value cannot be fallible here; a `maybe` result "
                 "is not lowered for a lambda yet");
    }
    expect(T_NL);
    n->b = parse_stmts(fterms);
    expect(T_ENDFUNC);
    lex_body_end(saved);
    return n;
}

static struct node *
parse_params(void)
{
    struct list ps;

    list_init(&ps);
    if (!at(T_RPAREN)) {
        for (;;) {
            struct node *p = nn(N_PARAM);
            if (accept(T_SHARED))       /* a by-reference param (shared-params.md) */
                p->flags |= NF_SHARED;
            p->name = expect(T_IDENT).sval;
            expect(T_IS);
            p->type = parse_type();
            if (accept(T_WITH)) {
                /* `p is Point with (x, y)`: a field-restricted by-reference
                 * view (record-slicing.md D4). The field mask rides ->c; the
                 * body may access only these fields, checked at compile time. */
                struct list fs;
                list_init(&fs);
                expect(T_LPAREN);
                if (!at(T_RPAREN)) {
                    struct node *f = nn(N_NAME);
                    f->name = expect(T_IDENT).sval;
                    list_add(&fs, f);
                    while (accept(T_COMMA)) {
                        f = nn(N_NAME);
                        f->name = expect(T_IDENT).sval;
                        list_add(&fs, f);
                    }
                }
                p->c = fs.head;
                expect(T_RPAREN);
            }
            if (accept(T_ASSIGN))       /* a default (must be a compile-time
                                         * constant; checked in typecheck) */
                p->a = parse_expr();
            list_add(&ps, p);
            if (!accept(T_COMMA))
                break;
        }
    }
    return ps.head;
}

/* verb/func: header then either a body + end<kind>, or a bare
 * signature (empty body, as in a generated .exi). */
static struct node *
parse_callable(int kind, int vis)
{
    static const int vterms[] = { T_ENDVERB, 0 };
    static const int fterms[] = { T_ENDFUNC, 0 };
    struct node *n = nn(kind);
    int endkw = (kind == N_VERB) ? T_ENDVERB : T_ENDFUNC;

    advance();                    /* verb / func */
    n->name = expect(T_IDENT).sval;
    n->flags = vis;
    if (kind == N_VERB && at(T_NL)) {
        /* no signature: it comes from a supported interface, parameter
         * names included (interface-support.md D4). All or nothing, so
         * `verb open()` still means "takes nothing". */
        n->flags |= NF_NOSIG;
        expect(T_NL);
        n->b = parse_stmts(vterms);
        expect(T_ENDVERB);
        end_stmt();
        return n;
    }
    expect(T_LPAREN);
    n->a = parse_params();
    expect(T_RPAREN);
    if (accept(T_RETURNS)) {
        n->type = parse_type();
        if (kind == N_VERB && n->type->kind == ET_MAYBE)
            perr("a verb cannot fail; send failure belongs to the send "
                 "ABI (make it a func, or return a sentinel)");
    } else if (at(T_CAN)) {
        if (kind == N_VERB)
            perr("a verb cannot fail; send failure belongs to the send "
                 "ABI");
        advance();
        expect(T_FAIL);
        n->flags |= NF_CANFAIL;
    }
    expect(T_NL);
    skip_nl();

    /* signature form (no body): next is a member/section/end keyword */
    switch (lex_peek().kind) {
    case T_VERB: case T_FUNC: case T_PUBLIC: case T_PRIVATE:
    case T_DISCLOSES: case T_ENDCLASS:
        return n;                 /* .exi signature */
    default:
        break;
    }
    n->b = parse_stmts(kind == N_VERB ? vterms : fterms);
    expect(endkw);
    end_stmt();
    return n;
}

static struct node *
parse_field(int vis)
{
    struct node *n = nn(N_FIELD);

    n->flags = vis;
    if (accept(T_TUNABLE))
        n->flags |= NF_TUNABLE;
    n->name = expect(T_IDENT).sval;
    expect(T_IS);
    n->type = parse_type();
    if (accept(T_ASSIGN))
        n->a = parse_expr();
    end_stmt();
    return n;
}

static struct node *
parse_discloses(void)
{
    struct node *n = nn(N_DISCLOSE);
    struct list items;

    expect(T_DISCLOSES);
    list_init(&items);
    for (;;) {
        struct node *e = nn(N_NAME);
        e->name = expect(T_IDENT).sval;
        if (accept(T_COLON))             /* recv:selector form */
            e->alias = expect(T_IDENT).sval;
        if (accept(T_LPAREN)) {          /* optional arg shape, skipped */
            int depth = 1;
            while (depth > 0 && !at(T_EOF)) {
                if (at(T_LPAREN)) depth++;
                else if (at(T_RPAREN)) depth--;
                advance();
            }
        }
        list_add(&items, e);
        if (!accept(T_COMMA))
            break;
    }
    n->a = items.head;
    end_stmt();
    return n;
}

static struct node *
parse_class(void)
{
    struct node *n = nn(N_CLASS);
    struct list members;
    int vis = -1;                 /* current visibility section */

    expect(T_CLASS);
    n->name = expect(T_IDENT).sval;
    if (accept(T_IS))
        n->alias = expect(T_IDENT).sval;   /* parent */
    expect(T_NL);
    list_init(&members);

    for (;;) {
        skip_nl();
        switch (lex_peek().kind) {
        case T_PUBLIC:
            advance();
            expect(T_NL);
            vis = NF_PUBLIC;
            continue;
        case T_PRIVATE:
            advance();
            expect(T_NL);
            vis = 0;
            continue;
        case T_DISCLOSES:
            list_add(&members, parse_discloses());
            continue;
        case T_SUPPORTS: {
            /* `supports Openable, Lockable` (interface-support.md D1): a
             * body directive like `include`, not a header clause. It brings
             * signatures and no members, so it takes no visibility. */
            advance();
            for (;;) {
                struct node *s = nn(N_SUPPORTS);
                s->name = expect(T_IDENT).sval;
                list_add(&members, s);
                if (!accept(T_COMMA))
                    break;
            }
            end_stmt();
            continue;
        }
        case T_ENDCLASS:
        case T_EOF:
            goto done;
        case T_TUNABLE:
        case T_IDENT:
            if (vis < 0)
                perr("field must be inside a `public` or `private` section");
            list_add(&members, parse_field(vis));
            continue;
        case T_VERB:
            /* a verb is the boundary: public, dispatched, in the .exi
             * (verbs.md); a private verb would still be sendable */
            if (vis < 0)
                perr("verb must be inside a `public` section");
            if (!(vis & NF_PUBLIC))
                perr("a verb is a message others can send; it belongs "
                     "under `public` (write `func` for a private helper)");
            list_add(&members, parse_callable(N_VERB, vis));
            continue;
        case T_FUNC:
            if (vis < 0)
                perr("func must be inside a `private` section");
            if (vis & NF_PUBLIC)
                perr("a func is a private helper; it belongs under "
                     "`private` (write `verb` for a public entry point)");
            list_add(&members, parse_callable(N_FUNC, vis));
            continue;
        default:
            perr("expected a member, `public`, `private`, or `endclass`");
        }
    }
done:
    n->a = members.head;
    expect(T_ENDCLASS);
    if (at(T_IDENT))              /* optional name echo */
        advance();
    end_stmt();
    return n;
}

static struct node *
parse_enum(void)
{
    struct node *n = nn(N_ENUM);
    struct list members;

    /* `enum Color [red green blue]`: a word list in the data-literal
     * style; the open bracket carries a large enum across lines. */
    expect(T_ENUM);
    n->name = expect(T_IDENT).sval;
    expect(T_LBRACK);
    list_init(&members);
    do {
        struct node *m = nn(N_NAME);
        m->name = expect(T_IDENT).sval;
        list_add(&members, m);
    } while (at(T_IDENT));
    expect(T_RBRACK);
    n->a = members.head;
    end_stmt();
    return n;
}

/* one `tags [ entry, ... ]` clause: each entry is a symbol followed by an
 * arbitrary number of atoms (record-annotations.md D3). The entries are kept
 * as opaque quoted forms (N_TAG nodes, atoms chained under ->a); the compiler
 * never interprets them, a macro walks them via `.tags`. */
static struct node *
parse_tags(void)
{
    struct list entries;

    expect(T_TAGS);
    expect(T_LBRACK);
    list_init(&entries);
    if (!at(T_RBRACK)) {
        for (;;) {
            struct node *entry = nn(N_TAG);
            struct list atoms;
            list_init(&atoms);
            for (;;) {                  /* a symbol then its atoms */
                struct token t = lex_peek();
                struct node *atom;
                if (t.kind == T_IDENT) {
                    advance();
                    atom = nn(N_NAME);
                    atom->name = t.sval;
                } else if (t.kind == T_STRING) {
                    advance();
                    atom = nn(N_STR);
                    atom->sval = t.sval;
                    atom->slen = t.slen;
                } else if (t.kind == T_NUMBER) {
                    advance();
                    atom = nn(N_NUM);
                    atom->ival = t.nval;
                } else {
                    break;
                }
                list_add(&atoms, atom);
            }
            if (!atoms.head)
                perr("a tag is a symbol followed by atoms");
            entry->a = atoms.head;
            list_add(&entries, entry);
            if (!accept(T_COMMA))
                break;
        }
    }
    expect(T_RBRACK);
    return entries.head;
}

/* One record field line: a comma list of entries terminated by a newline
 * (record-annotations.md D1). An entry is a field definition or a `tags [...]`
 * clause; a `tags` clause decorates the immediately preceding field (D2). */
static void
parse_record_line(struct list *fields)
{
    struct node *last = NULL;

    for (;;) {
        if (at(T_TAGS)) {
            struct node *tags = parse_tags();
            if (!last)
                perr("a `tags` clause must follow a field on its line");
            last->c = tags;             /* decorate the preceding field */
        } else {
            struct node *f = nn(N_FIELD);
            f->flags = NF_PUBLIC;
            if (accept(T_TUNABLE))
                f->flags |= NF_TUNABLE;
            f->name = expect(T_IDENT).sval;
            expect(T_IS);
            f->type = parse_type();
            if (accept(T_ASSIGN))
                f->a = parse_expr();
            list_add(fields, f);
            last = f;
        }
        if (!accept(T_COMMA))
            break;
    }
    end_stmt();
}

static struct node *
parse_record(void)
{
    struct node *n = nn(N_RECORD);
    struct list fields;

    expect(T_RECORD);
    n->name = expect(T_IDENT).sval;
    expect(T_NL);
    list_init(&fields);
    for (;;) {
        skip_nl();
        if (at(T_ENDRECORD) || at(T_EOF))
            break;
        parse_record_line(&fields);
    }
    n->a = fields.head;
    expect(T_ENDRECORD);
    if (at(T_IDENT))
        advance();
    end_stmt();
    return n;
}

/* One slot atom of a node-kind line, plus an optional `+` / `*` repetition
 * suffix. The atom is either a scalar type keyword (its token is kept in
 * ->ival so the checker matches leaf literals) or a bare word (->ival is
 * T_IDENT, ->name the word; the checker classifies it as `label`, `ref`, a
 * nested node kind, or a literal keyword atom). typed-data.md, stage 1. */
static struct node *
parse_shape_slot(void)
{
    struct node *s = nn(N_SHAPESLOT);
    struct token t = lex_peek();

    switch (t.kind) {
    case T_TSTR: case T_TINT: case T_TDEC: case T_TBOOL:
        advance();
        s->ival = t.kind;
        s->name = (char *)tok_str(t.kind);
        break;
    case T_TO:                          /* a keyword read as a word */
        advance();
        s->ival = T_IDENT;
        s->name = "to";
        break;
    default:
        s->ival = T_IDENT;
        s->name = expect(T_IDENT).sval;
        break;
    }
    if (at(T_PLUS) || at(T_STAR))
        s->op = advance().kind;         /* one-or-more / zero-or-more */
    return s;
}

/* A shape: a schema for symbolic data literals, a sum of node kinds
 * (typed-data.md). Each line is `Kind [ rhs ]`, mirroring the Logo
 * bracket of the `[Kind ...]` data it constrains (and the `enum Name
 * [...]` form). A bracketed body keeps the `+`/`*` slot suffixes off the
 * line end, where they would otherwise read as a continuation operator
 * (is_cont, lex.c). An rhs with `or` is a group (an alternation of kind
 * names); otherwise it is a node kind (a slot sequence). The first node
 * kind is the root the literal's head must match. */
static struct node *
parse_shape(void)
{
    struct node *n = nn(N_SHAPE);
    struct list kinds;

    expect(T_SHAPE);
    n->name = expect(T_IDENT).sval;
    expect(T_NL);
    list_init(&kinds);
    for (;;) {
        struct node *k;
        struct list slots;

        skip_nl();
        if (at(T_ENDSHAPE) || at(T_EOF))
            break;
        k = nn(N_SHAPEKIND);
        k->name = expect(T_IDENT).sval;
        expect(T_LBRACK);
        list_init(&slots);
        list_add(&slots, parse_shape_slot());
        if (at(T_OR)) {                 /* a group: `a or b or c` */
            k->flags |= NF_SHAPE_GROUP;
            while (accept(T_OR)) {
                struct node *alt = nn(N_SHAPESLOT);
                alt->ival = T_IDENT;
                alt->name = expect(T_IDENT).sval;
                list_add(&slots, alt);
            }
        } else {                        /* a node kind: a slot sequence */
            while (!at(T_RBRACK) && !at(T_EOF))
                list_add(&slots, parse_shape_slot());
        }
        expect(T_RBRACK);
        k->a = slots.head;
        end_stmt();
        list_add(&kinds, k);
    }
    n->a = kinds.head;
    expect(T_ENDSHAPE);
    if (at(T_IDENT))
        advance();
    end_stmt();
    return n;
}

/****************************************************************
 * Sections
 ****************************************************************/

static void
parse_use_section(struct list *out)
{
    expect(T_USE);
    expect(T_NL);
    skip_nl();
    while (at(T_IDENT)) {
        struct node *n = nn(N_USE);
        char path[256];
        int len = 0;
        struct token id = expect(T_IDENT);

        len += snprintf(path, sizeof path, "%s", id.sval);
        while (accept(T_DOT)) {
            id = expect(T_IDENT);
            len += snprintf(path + len, sizeof path - (size_t)len, ".%s",
                            id.sval);
        }
        n->name = arena_strndup(pa, path, (size_t)len);
        if (accept(T_AS)) {
            n->alias = expect(T_IDENT).sval;
        } else if (accept(T_LPAREN)) {
            struct list names;
            list_init(&names);
            for (;;) {
                struct node *nm = nn(N_NAME);
                nm->name = expect(T_IDENT).sval;
                list_add(&names, nm);
                if (!accept(T_COMMA))
                    break;
            }
            expect(T_RPAREN);
            n->a = names.head;
        }
        end_stmt();
        list_add(out, n);
        skip_nl();
    }
}

static void
parse_const_section(struct list *out)
{
    expect(T_CONST);
    expect(T_NL);
    skip_nl();
    while (at(T_IDENT)) {
        struct node *n = nn(N_CONST);
        n->name = expect(T_IDENT).sval;
        if (accept(T_IS))
            n->type = parse_type();
        expect(T_ASSIGN);
        n->a = parse_expr();
        end_stmt();
        list_add(out, n);
        skip_nl();
    }
}

/* A macro definition (meta.md): `macro name(params) ... endmacro`. Its
 * params bind the auto-quoted argument forms, so they are bare names with no
 * type. The body is meta-layer code run at compile time (the interpreter is
 * in typecheck.c); it is not resolved or checked here. */
static struct node *
parse_macro(void)
{
    static const int terms[] = { T_ENDMACRO, 0 };
    struct node *n = nn(N_MACRO);
    struct list ps;

    expect(T_MACRO);
    n->name = expect(T_IDENT).sval;
    expect(T_LPAREN);
    list_init(&ps);
    if (!at(T_RPAREN)) {
        for (;;) {
            struct node *p = nn(N_PARAM);
            p->name = expect(T_IDENT).sval;
            list_add(&ps, p);
            if (!accept(T_COMMA))
                break;
        }
    }
    n->a = ps.head;
    expect(T_RPAREN);
    expect(T_NL);
    in_macro = 1;
    n->b = parse_stmts(terms);
    in_macro = 0;
    expect(T_ENDMACRO);
    end_stmt();
    return n;
}

static void
parse_type_section(struct list *out)
{
    expect(T_TYPE);
    expect(T_NL);
    for (;;) {
        skip_nl();
        switch (lex_peek().kind) {
        case T_CLASS:  list_add(out, parse_class());  break;
        case T_ENUM:   list_add(out, parse_enum());   break;
        case T_RECORD: list_add(out, parse_record()); break;
        case T_SHAPE:  list_add(out, parse_shape());  break;
        case T_INTERFACE: list_add(out, parse_interface()); break;
        case T_IDENT:
            /* The bare-identifier declaration retired with interface-decl.md
             * D7. Nothing in a type section opens with a name now, so this
             * is either a misspelled declaration keyword or the old
             * interface form; name both rather than guess between them. */
            perr("a type declaration opens with `class`, `record`, `enum`, "
                 "`shape`, or `interface` (an interface is `interface %s "
                 "... endinterface`, a `verb` signature per line)",
                 lex_peek().sval);
        default:       return;
        }
    }
}

struct node *
parse_program(struct arena *a)
{
    struct node *file;
    struct list items;

    pa = a;
    pfile = lex_filename();
    file = nn(N_FILE);
    list_init(&items);

    for (;;) {
        skip_nl();
        switch (lex_peek().kind) {
        case T_EOF:
            goto done;
        case T_USE:
            parse_use_section(&items);
            break;
        case T_CONST:
            parse_const_section(&items);
            break;
        case T_TYPE:
            parse_type_section(&items);
            break;
        case T_MACRO:
            list_add(&items, parse_macro());
            skip_nl();
            break;
        default:
            perr("expected a section: `use`, `const`, `type`, or `macro`");
        }
    }
done:
    file->a = items.head;
    return file;
}
