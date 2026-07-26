/* resolve.c : Excelsior name resolution and symbol table.
 *
 * Runs over the AST produced by parse.c. It builds a lexical scope
 * tree, binds every declaration to a symbol, and annotates each
 * reference (node->sym / ex_type->sym) with the declaration it names.
 *
 * Excelsior identifiers are case-insensitive over ASCII: a symbol is
 * keyed by its case-folded spelling, but keeps the first-seen spelling
 * for diagnostics. A leading `_` is part of the identifier (it never
 * marks a keyword), so it participates in matching like any letter.
 *
 * What is resolved now:
 *   - module scope: use bindings, consts, classes, enums, records
 *   - class member scopes: fields, verbs, funcs
 *   - enum / record member scopes
 *   - verb / func params, and block-scoped locals (var, for-loop var)
 *   - `self` inside methods, and `self.field` against the class members
 *   - `EnumName.member` against the enum's members
 *   - named types against module-level declarations
 *
 * Names that do not resolve locally are left with sym == NULL. They are
 * assumed to be external (imported from a used module, or provided by the
 * host prelude); this pass does not yet model those, so it does not treat
 * an unresolved reference as an error. Errors it does raise: duplicate
 * declarations in one scope, and `self` used outside a method.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "excelsior.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/****************************************************************
 * Scope
 ****************************************************************/

struct scope {
    struct scope *parent;
    struct sym *head;
    struct sym **tail;
};

static struct arena *ra;
static const char *rfile;
static struct scope *module_scope;

static NORETURN void
rerr(struct node *n, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    die("%s:%d: %s", rfile, n ? n->line : 0, buf);
}

int
ex_ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

/* look a member up in a class and, failing that, its inheritance chain */
struct sym *
sym_member(struct sym *cls, const char *name)
{
    for (struct sym *c = cls; c; c = c->base)
        if (c->members)
            for (struct sym *y = c->members->head; y; y = y->next)
                if (ex_ci_eq(y->name, name))
                    return y;
    return NULL;
}

static char *
fold(const char *s)
{
    size_t n = strlen(s);
    char *d = arena_alloc(ra, n + 1);

    for (size_t i = 0; i < n; i++)
        d[i] = (char)tolower((unsigned char)s[i]);
    d[n] = '\0';
    return d;
}

static struct scope *
scope_new(struct scope *parent)
{
    struct scope *s = arena_zalloc(ra, sizeof *s);

    s->parent = parent;
    s->tail = &s->head;
    return s;
}

static struct sym *
scope_find_local(struct scope *s, const char *key)
{
    for (struct sym *y = s->head; y; y = y->next)
        if (strcmp(y->key, key) == 0)
            return y;
    return NULL;
}

static struct sym *
scope_lookup(struct scope *s, const char *name)
{
    char *key = fold(name);

    for (; s; s = s->parent) {
        struct sym *y = scope_find_local(s, key);
        if (y)
            return y;
    }
    return NULL;
}

static struct sym *
scope_define(struct scope *s, int kind, const char *name,
             struct node *decl, struct ex_type *type)
{
    char *key = fold(name);
    struct sym *ex = scope_find_local(s, key);
    struct sym *y;

    if (ex) {
        if (ex->decl)
            rerr(decl, "`%s` is already defined (as `%s` at line %d)",
                 name, ex->name, ex->decl->line);
        rerr(decl, "`%s` is already defined", name);
    }
    y = arena_zalloc(ra, sizeof *y);
    y->kind = kind;
    y->name = arena_strdup(ra, name);
    y->key = key;
    y->decl = decl;
    y->type = type;
    *s->tail = y;
    s->tail = &y->next;
    return y;
}

/****************************************************************
 * Expression / statement resolution
 ****************************************************************/

static void resolve_expr(struct node *n, struct scope *sc, struct sym *cls);
static void resolve_stmt(struct node *n, struct scope *sc, struct sym *cls);

static void
resolve_type(struct ex_type *t, struct scope *sc)
{
    if (!t)
        return;
    if (t->kind == T_TLIST || t->kind == ET_MAYBE || t->kind == T_TSET) {
        resolve_type(t->inner, sc);
        return;
    }
    if (t->kind != T_IDENT)
        return;                     /* builtin type keyword */
    if (strchr(t->name, '.'))
        return;                     /* qualified: from a used module */

    struct sym *y = scope_lookup(sc, t->name);
    if (y && (y->kind == SYM_CLASS || y->kind == SYM_ENUM ||
              y->kind == SYM_RECORD || y->kind == SYM_SHAPE ||
              y->kind == SYM_INTERFACE))
        t->sym = y;
    /* else: assumed external, left unresolved */
}

static void
resolve_list(struct node *n, struct scope *sc, struct sym *cls)
{
    for (; n; n = n->next)
        resolve_expr(n, sc, cls);
}

static void
resolve_expr(struct node *n, struct scope *sc, struct sym *cls)
{
    if (!n)
        return;

    switch (n->kind) {
    case N_NAME:
        n->sym = scope_lookup(sc, n->name);
        break;
    case N_SELF:
        if (!cls)
            rerr(n, "`self` is only valid inside a class method");
        n->sym = cls;
        break;
    case N_FIELD_ACC:
        resolve_expr(n->a, sc, cls);
        if (n->a->kind == N_SELF && cls) {
            struct sym *f = sym_member(cls, n->name);   /* own or inherited */
            if (f)
                n->sym = f;
        } else if (n->a->sym && n->a->sym->kind == SYM_ENUM &&
                   n->a->sym->members) {
            struct sym *m = scope_find_local(n->a->sym->members,
                                             fold(n->name));
            if (m)
                n->sym = m;
        }
        break;
    case N_SEND:
        resolve_expr(n->a, sc, cls);
        resolve_list(n->b, sc, cls);
        break;                      /* selector resolved during typecheck */
    case N_CALL:
        resolve_expr(n->a, sc, cls);
        resolve_list(n->b, sc, cls);
        break;
    case N_BINOP:
    case N_INDEX:
    case N_RANGE:
        resolve_expr(n->a, sc, cls);
        resolve_expr(n->b, sc, cls);
        break;
    case N_WITH:                        /* p with (x, y): resolve the base; the
                                         * field names are checked against its
                                         * type, not resolved as values */
        resolve_expr(n->a, sc, cls);
        break;
    case N_CMPCHAIN:                    /* a < b < c: first + link list */
        resolve_expr(n->a, sc, cls);
        for (struct node *l = n->b; l; l = l->next)
            resolve_expr(l->a, sc, cls);
        break;
    case N_SLICE:
        resolve_expr(n->a, sc, cls);
        resolve_expr(n->b, sc, cls);
        resolve_expr(n->c, sc, cls);
        break;
    case N_UNOP:
    case N_TOSTR:
        resolve_expr(n->a, sc, cls);
        break;
    case N_THENELSE:                    /* cond then A else B */
        resolve_expr(n->a, sc, cls);
        resolve_expr(n->b, sc, cls);
        resolve_expr(n->c, sc, cls);
        break;
    case N_SELECT:                      /* select idx from b0, b1, ... else c */
        resolve_expr(n->a, sc, cls);
        for (struct node *br = n->b; br; br = br->next)
            resolve_expr(br, sc, cls);
        if (n->c)
            resolve_expr(n->c, sc, cls);
        break;
    case N_MATCHEXPR:                    /* case E of vals -> e ... else -> e */
        resolve_expr(n->a, sc, cls);
        for (struct node *arm = n->b; arm; arm = arm->next) {
            resolve_list(arm->a, sc, cls);          /* match values */
            resolve_expr(arm->b, sc, cls);
        }
        if (n->c)
            resolve_expr(n->c, sc, cls);
        break;
    case N_OTHERWISE:                   /* A otherwise B */
        resolve_expr(n->a, sc, cls);
        resolve_expr(n->b, sc, cls);
        break;
    case N_CAST:
        resolve_expr(n->a, sc, cls);
        resolve_type(n->type, sc);
        break;
    case N_ISTEST:
        resolve_expr(n->a, sc, cls);
        break;                      /* type name checked during typecheck */
    case N_DATALIT:
        /* quoted data: bare names are literal words, but a ${expr}
         * hole is code and its names resolve normally */
        for (struct node *it = n->a; it; it = it->next)
            if (it->kind == N_HOLE || it->kind == N_DATALIT)
                resolve_expr(it->kind == N_HOLE ? it->a : it, sc, cls);
        break;
    case N_QUOTE:
        break;                      /* quoted data: names are literal */
    default:
        break;                      /* literals carry nothing to resolve */
    }
}

static void
resolve_stmts(struct node *list, struct scope *sc, struct sym *cls)
{
    for (struct node *n = list; n; n = n->next)
        resolve_stmt(n, sc, cls);
}

static void
resolve_stmt(struct node *n, struct scope *sc, struct sym *cls)
{
    if (!n)
        return;

    switch (n->kind) {
    case N_VAR:
        resolve_type(n->type, sc);
        resolve_expr(n->a, sc, cls);        /* init sees the outer name */
        n->sym = scope_define(sc, SYM_LOCAL, n->name, n, n->type);
        break;
    case N_ASSIGN:
        resolve_expr(n->a, sc, cls);
        resolve_expr(n->b, sc, cls);
        break;
    case N_IF: {
        struct scope *then_sc = scope_new(sc);
        resolve_expr(n->a, sc, cls);
        if (n->name)                    /* if var i = expr: i in the arm */
            n->sym = scope_define(then_sc, SYM_LOCAL, n->name, n, NULL);
        resolve_stmts(n->b, then_sc, cls);
        if (n->c) {
            if (n->c->kind == N_IF)
                resolve_stmt(n->c, sc, cls);
            else
                resolve_stmts(n->c, scope_new(sc), cls);
        }
        break;
    }
    case N_FOR: {
        struct scope *body_sc = scope_new(sc);
        resolve_expr(n->a, sc, cls);
        resolve_expr(n->b, sc, cls);
        n->sym = scope_define(body_sc, SYM_LOCAL, n->name, n, NULL);
        resolve_stmts(n->c, body_sc, cls);
        break;
    }
    case N_WHILE: {
        struct scope *body_sc = scope_new(sc);
        resolve_expr(n->a, sc, cls);
        if (n->name)                    /* while var i = expr */
            n->sym = scope_define(body_sc, SYM_LOCAL, n->name, n, NULL);
        resolve_stmts(n->b, body_sc, cls);
        break;
    }
    case N_MATCH:
        resolve_expr(n->a, sc, cls);
        for (struct node *arm = n->b; arm; arm = arm->next) {
            resolve_list(arm->a, sc, cls);          /* match values */
            resolve_stmts(arm->b, scope_new(sc), cls);
        }
        if (n->c)
            resolve_stmts(n->c, scope_new(sc), cls);
        break;
    case N_RETURN:
    case N_TRACE:
    case N_EXPR_STMT:
        resolve_expr(n->a, sc, cls);
        break;
    case N_ONFAIL:                      /* stmt on fail handler */
        resolve_stmt(n->a, sc, cls);
        resolve_stmt(n->b, sc, cls);
        break;
    case N_BREAK:
    case N_CONTINUE:
    case N_TRACE_CMT:
        break;
    default:
        resolve_expr(n, sc, cls);               /* bare expression */
        break;
    }
}

/****************************************************************
 * Declaration collection
 ****************************************************************/

static void
bind_use(struct scope *mod, struct node *u)
{
    if (u->a) {                                 /* selective import list */
        for (struct node *c = u->a; c; c = c->next)
            c->sym = scope_define(mod, SYM_IMPORT,
                                  c->alias ? c->alias : c->name, c, NULL);
        return;
    }
    if (u->alias) {
        u->sym = scope_define(mod, SYM_MODULE, u->alias, u, NULL);
        return;
    }
    /* bare `use a.b.c` binds the last path component */
    const char *dot = strrchr(u->name, '.');
    u->sym = scope_define(mod, SYM_MODULE, dot ? dot + 1 : u->name, u, NULL);
}

/* Pass 1b: build the member scope for a container declaration. */
static void
collect_members(struct node *decl, struct scope *mod)
{
    struct scope *ms = scope_new(mod);

    decl->sym->members = ms;
    switch (decl->kind) {
    case N_CLASS:
        for (struct node *m = decl->a; m; m = m->next) {
            switch (m->kind) {
            case N_FIELD:
                m->sym = scope_define(ms, SYM_FIELD, m->name, m, m->type);
                break;
            case N_VERB:
                m->sym = scope_define(ms, SYM_VERB, m->name, m, m->type);
                break;
            case N_FUNC:
                m->sym = scope_define(ms, SYM_FUNC, m->name, m, m->type);
                break;
            default:
                break;                          /* disclose: no binding */
            }
        }
        break;
    case N_ENUM: {
        long ord = 0;                       /* 0-based position (enums.md D1) */
        for (struct node *e = decl->a; e; e = e->next) {
            e->ival = ord++;
            e->sym = scope_define(ms, SYM_ENUM_MEMBER, e->name, e, NULL);
        }
        break;
    }
    case N_RECORD:
        for (struct node *f = decl->a; f; f = f->next)
            f->sym = scope_define(ms, SYM_FIELD, f->name, f, f->type);
        break;
    default:
        break;
    }
    for (struct sym *y = ms->head; y; y = y->next)
        y->owner = decl->sym;
}

/* Pass 1c: link each class to its parent and chain the member scopes, so
 * inherited members resolve (both bare names and self.field). */
static void
link_parents(struct node *file, struct scope *mod)
{
    for (struct node *it = file->a; it; it = it->next) {
        struct sym *parent;

        if (it->kind != N_CLASS || !it->alias)
            continue;               /* no `is Parent` clause */
        parent = scope_find_local(mod, fold(it->alias));
        if (!parent || parent->kind != SYM_CLASS)
            continue;               /* external parent: inherited via host */
        for (struct sym *c = parent; c; c = c->base)
            if (c == it->sym)
                rerr(it, "inheritance cycle through `%s`", it->name);
        it->sym->base = parent;
        it->sym->members->parent = parent->members;
    }
}

/* Pass 2: resolve the bodies and types of a container declaration. */
static void
resolve_container(struct node *decl, struct scope *mod)
{
    struct scope *ms = decl->sym->members;

    if (decl->kind != N_CLASS) {
        for (struct node *f = decl->a; f; f = f->next)
            resolve_type(f->type, mod);
        return;
    }
    for (struct node *m = decl->a; m; m = m->next) {
        switch (m->kind) {
        case N_FIELD:
            resolve_type(m->type, mod);
            resolve_expr(m->a, mod, NULL);      /* default: no self */
            break;
        case N_VERB:
        case N_FUNC: {
            struct scope *fn = scope_new(ms);
            for (struct node *p = m->a; p; p = p->next) {
                resolve_type(p->type, mod);
                p->sym = scope_define(fn, SYM_PARAM, p->name, p, p->type);
            }
            resolve_type(m->type, mod);         /* return type */
            resolve_stmts(m->b, fn, decl->sym);
            break;
        }
        default:
            break;
        }
    }
}

/* Look a name up at module scope after resolution has run. The checker uses
 * this on a form a macro emitted: such a form was built, not parsed in a
 * scope, so a name it writes carries no symbol (typed-macros.md D2). */
struct sym *
module_sym(const char *name)
{
    return module_scope ? scope_lookup(module_scope, name) : NULL;
}

/* Do two verb declarations have the same signature? Used to tell a real
 * disagreement between two sources from two spellings of one contract. */
static int
same_sig(struct node *a, struct node *b)
{
    struct node *pa = a->a, *pb = b->a;

    for (; pa && pb; pa = pa->next, pb = pb->next)
        if (!pa->type || !pb->type || pa->type->kind != pb->type->kind)
            return 0;
    if (pa || pb)
        return 0;
    if (!a->type || !b->type)
        return !a->type && !b->type;
    return a->type->kind == b->type->kind;
}

static struct node *
copy_params(struct node *p)
{
    struct node *head = NULL, **tail = &head;

    for (; p; p = p->next) {
        struct node *c = arena_zalloc(ra, sizeof *c);
        *c = *p;
        c->next = NULL;
        c->sym = NULL;              /* bound afresh in this class's scope */
        *tail = c;
        tail = &c->next;
    }
    return head;
}

/* Fill a `verb name` written with no signature from the one source that
 * declares it (interface-support.md D4/D5): a supported interface, or the
 * parent, which is the more specific source. Two supported interfaces that
 * disagree is the peers-error case (capabilities.md D4); the class resolves
 * it by writing the signature it means. */
static void
fill_signatures(struct node *cls)
{
    for (struct node *m = cls->a; m; m = m->next) {
        struct node *from = NULL;
        const char *fromwhat = NULL;
        struct sym *pv;

        if (m->kind != N_VERB || !(m->flags & NF_NOSIG))
            continue;
        pv = cls->sym->base ? sym_member(cls->sym->base, m->name) : NULL;
        if (pv && pv->kind == SYM_VERB && pv->decl) {
            from = pv->decl;                /* the parent wins (D5) */
            fromwhat = cls->sym->base->name;
        }
        for (struct node *s = cls->a; s && !from; s = s->next) {
            struct sym *iv;
            if (s->kind != N_SUPPORTS || !s->sym)
                continue;
            iv = sym_member(s->sym, m->name);
            if (!iv || !iv->decl)
                continue;
            from = iv->decl;
            fromwhat = s->name;
        }
        if (!from)
            rerr(m, "`%s` has no signature here, and no supported interface "
                    "or parent declares a verb `%s`", m->name, m->name);
        /* a second, disagreeing source is the peers case */
        for (struct node *s = cls->a; s; s = s->next) {
            struct sym *iv;
            if (s->kind != N_SUPPORTS || !s->sym)
                continue;
            iv = sym_member(s->sym, m->name);
            if (!iv || !iv->decl || iv->decl == from)
                continue;
            if (!same_sig(iv->decl, from))
                rerr(m, "`%s` and `%s` declare `%s` differently; write the "
                        "signature this class means", fromwhat, s->name,
                     m->name);
        }
        m->a = copy_params(from->a);
        m->type = from->type;
        if (m->sym)
            m->sym->type = m->type;         /* collect_members ran already */
    }
}

/* A macro body is meta code and is deliberately not resolved: its names are
 * meta names the expander binds. One exception is annotation, not resolution.
 * A name that matches a declared record, class, or enum gets that symbol
 * stamped on it, so a typed macro can name a type to compare against
 * (`typeof(v) = Vec3`, typed-macros.md). Nothing errors here: an unmatched
 * name stays bare and the expander reports it, and a meta binding of the same
 * name still wins, since the expander checks its own environment first. */
static void
stamp_type_names(struct node *n)
{
    for (; n; n = n->next) {
        if (n->kind == N_NAME && !n->sym) {
            struct sym *y = scope_lookup(module_scope, n->name);
            if (y && (y->kind == SYM_RECORD || y->kind == SYM_CLASS ||
                      y->kind == SYM_ENUM))
                n->sym = y;
        }
        stamp_type_names(n->a);
        stamp_type_names(n->b);
        stamp_type_names(n->c);
    }
}

void
resolve_program(struct arena *a, struct node *file)
{
    ra = a;
    rfile = lex_filename();
    module_scope = scope_new(NULL);

    /* Pass 1: top-level names, so bodies can reference later decls. */
    for (struct node *it = file->a; it; it = it->next) {
        switch (it->kind) {
        case N_USE:
            bind_use(module_scope, it);
            break;
        case N_CONST:
            it->sym = scope_define(module_scope, SYM_CONST, it->name, it,
                                   it->type);
            break;
        case N_CLASS:
            it->sym = scope_define(module_scope, SYM_CLASS, it->name, it,
                                   NULL);
            break;
        case N_ENUM:
            it->sym = scope_define(module_scope, SYM_ENUM, it->name, it,
                                   NULL);
            break;
        case N_RECORD:
            it->sym = scope_define(module_scope, SYM_RECORD, it->name, it,
                                   NULL);
            break;
        case N_SHAPE:
            /* a data-literal schema; a named type with no members and no
             * body to resolve, checked structurally against literals in
             * the type checker (typed-data.md) */
            it->sym = scope_define(module_scope, SYM_SHAPE, it->name, it,
                                   NULL);
            break;
        case N_INTERFACE: {
            /* An interface (interface-decl.md): a contract other classes
             * satisfy. Its verbs get a member scope like a class's, so
             * sym_member finds them and check_args takes them, which is what
             * lets the declared signature be the one conformance compares. */
            struct sym *y = scope_define(module_scope, SYM_INTERFACE,
                                         it->name, it, it->type);
            it->sym = y;
            it->type->sym = y;          /* the slice knows its interface */
            y->members = scope_new(module_scope);
            for (struct node *v = it->type->verbs; v; v = v->next)
                v->sym = scope_define(y->members, SYM_VERB, v->name, v,
                                      v->type);
            for (struct sym *m = y->members->head; m; m = m->next)
                m->owner = y;
            break;
        }
        case N_MACRO:
            /* a meta-layer macro (meta.md): the name is bound so a call can
             * find it, but the body is meta-code run by the expander at
             * compile time, so it is not resolved here (its quoted forms
             * resolve at the call site after splicing) */
            it->sym = scope_define(module_scope, SYM_MACRO, it->name, it,
                                   NULL);
            break;
        default:
            break;
        }
    }

    /* Pass 1b: member scopes for containers. */
    for (struct node *it = file->a; it; it = it->next)
        if (it->kind == N_CLASS || it->kind == N_ENUM ||
            it->kind == N_RECORD)
            collect_members(it, module_scope);

    /* Pass 1c: link inheritance now that all member scopes exist. */
    link_parents(file, module_scope);

    /* Pass 1d: bind each `supports` to its interface, then fill any verb
     * written without a signature (interface-support.md). Runs after the
     * parent link, so the parent can be the source, and before bodies are
     * resolved, so a filled parameter is in scope like any other. */
    for (struct node *it = file->a; it; it = it->next) {
        if (it->kind != N_CLASS)
            continue;
        for (struct node *m = it->a; m; m = m->next) {
            if (m->kind != N_SUPPORTS)
                continue;
            m->sym = scope_lookup(module_scope, m->name);
            if (!m->sym || m->sym->kind != SYM_INTERFACE)
                rerr(m, "`%s` is not an interface; `supports` names one "
                        "(`interface %s ... endinterface`)", m->name, m->name);
        }
        fill_signatures(it);
    }

    /* Pass 2: resolve types and bodies. */
    for (struct node *it = file->a; it; it = it->next) {
        switch (it->kind) {
        case N_CONST:
            resolve_type(it->type, module_scope);
            resolve_expr(it->a, module_scope, NULL);
            break;
        case N_CLASS:
        case N_ENUM:
        case N_RECORD:
            resolve_container(it, module_scope);
            break;
        case N_MACRO:
            stamp_type_names(it->b);
            break;
        case N_INTERFACE:
            /* the signatures name types, which resolve like any other */
            for (struct node *v = it->type->verbs; v; v = v->next) {
                resolve_type(v->type, module_scope);
                for (struct node *p = v->a; p; p = p->next)
                    resolve_type(p->type, module_scope);
            }
            break;
        default:
            break;
        }
    }
}

/****************************************************************
 * Symbol table dump (verification aid)
 ****************************************************************/

const char *
sym_kind_name(int kind)
{
    switch (kind) {
    case SYM_CLASS:       return "class";
    case SYM_ENUM:        return "enum";
    case SYM_RECORD:      return "record";
    case SYM_SHAPE:       return "shape";
    case SYM_ENUM_MEMBER: return "enum-member";
    case SYM_CONST:       return "const";
    case SYM_FIELD:       return "field";
    case SYM_VERB:        return "verb";
    case SYM_FUNC:        return "func";
    case SYM_PARAM:       return "param";
    case SYM_LOCAL:       return "local";
    case SYM_MODULE:      return "module";
    case SYM_IMPORT:      return "import";
    default:              return "?";
    }
}

static void
dump_scope(struct scope *s, int depth)
{
    for (struct sym *y = s ? s->head : NULL; y; y = y->next) {
        for (int i = 0; i < depth; i++)
            printf("  ");
        printf("%-12s %s\n", sym_kind_name(y->kind), y->name);
        if (y->members)
            dump_scope(y->members, depth + 1);
    }
}

void
dump_symbols(struct node *file)
{
    (void)file;
    printf("module symbols:\n");
    dump_scope(module_scope, 1);
}
