/* typecheck.c : Excelsior static type checker.
 *
 * Runs after name resolution. It gives every expression a static type
 * (stored back in node->type) and checks the structural rules of the
 * option-A type system: monomorphic, no runtime tagging of atomic
 * values, one concrete signature per verb.
 *
 * The design (see core.md "Type system"):
 *   - int and float are untagged machine values; float is IEEE 754 double.
 *   - decimal is base-10 fixed-point: value x 10^-4 in an int32
 *     (numbers.md). A decimal literal is an untyped constant (ET_DEC)
 *     that adapts to decimal or float context and defaults to decimal.
 *   - Numeric widening is one-way: an int literal or value flows into
 *     float or decimal, never the reverse without an `as` cast. decimal
 *     and float do not mix implicitly.
 *   - prop is the one dynamic point: it is assignable to and from any
 *     type (a compiler-level tagged union).
 *   - nil is compatible with reference types (obj, err, class, list, prop).
 *
 * Leniency: a reference the resolver left external (an imported symbol or
 * a host-prelude call) has no known type. Such expressions take the
 * internal ET_ANY type, which is compatible with everything, so the
 * checker never reports a false error against a name it cannot yet model.
 * When those layers are modeled, ET_ANY narrows and the checks tighten.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "excelsior.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct arena *ta;
static const char *tfile;
static struct node *cur_mem;      /* the verb/func being checked */
static struct sym *cur_class_sym; /* the class owning it (for record UFCS) */
static int meta_depth;            /* macro expansion depth (record-introspection.md) */
static struct node *cur_file;     /* the program, for program-wide selector lookup */

/* shared singleton types */
static struct ex_type *ty_int, *ty_float, *ty_dec, *ty_bool, *ty_str;
static struct ex_type *ty_obj, *ty_err, *ty_prop, *ty_nil, *ty_any;
static struct ex_type *ty_declit;   /* an unresolved decimal literal */
static struct ex_type *ty_signal;   /* a `can fail` call: a valueless signal */

/* decimal is base-10 fixed-point: value x 10^-4 in an int32 (numbers.md) */
#define DEC_SCALE 10000

/* A teaching line appended to whatever type error fires next, set while
 * checking a region where the likely mistake is not what the error names
 * (see the if-expression condition in check_expr). NULL most of the time. */
static const char *terr_note;

static NORETURN void
terr(struct node *n, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (terr_note)
        die("%s:%d: %s\n  note: %s", tfile, n ? n->line : 0, buf, terr_note);
    die("%s:%d: %s", tfile, n ? n->line : 0, buf);
}

static struct ex_type *
mk_ty(int kind)
{
    struct ex_type *t = arena_zalloc(ta, sizeof *t);
    t->kind = kind;
    return t;
}

static struct ex_type *
list_of(struct ex_type *elem)
{
    struct ex_type *t = mk_ty(T_TLIST);
    t->inner = elem;
    return t;
}

static struct ex_type *ty_nothing;

static int
is_maybe(struct ex_type *t)
{
    return t && t->kind == ET_MAYBE;
}

static struct ex_type *
maybe_of(struct ex_type *inner)
{
    struct ex_type *t = mk_ty(ET_MAYBE);
    t->inner = inner;
    return t;
}

/* the payload type of a maybe; identity on plain types */
static struct ex_type *
strip_maybe(struct ex_type *t)
{
    if (is_maybe(t))
        return t->inner ? t->inner : ty_any;
    return t;
}

static struct ex_type *
named_type(struct sym *s)
{
    struct ex_type *t = mk_ty(T_IDENT);
    t->name = s ? s->name : "?";
    t->sym = s;
    return t;
}

/* two list-element types match: the scalar singletons compare by pointer,
 * a named type (record/enum/class) by its resolved symbol (named_type is
 * not interned, so two `Point`s are distinct ex_type objects) */
static int
list_elem_same(struct ex_type *a, struct ex_type *b)
{
    if (a == b)
        return 1;
    if (a && b && a->kind == T_IDENT && b->kind == T_IDENT)
        return a->sym && a->sym == b->sym;
    return 0;
}

/****************************************************************
 * Type predicates and relations
 ****************************************************************/

static int
is_any(struct ex_type *t)
{
    return !t || t->kind == ET_ANY;
}

static int
is_numeric(struct ex_type *t)
{
    return t && (t->kind == T_TINT || t->kind == T_TFLOAT ||
                 t->kind == T_TDEC || t->kind == ET_DEC);
}

static int
is_ref(struct ex_type *t)
{
    if (!t)
        return 0;
    switch (t->kind) {
    case T_TOBJ: case T_TERR: case T_TPROP: case ET_SLICE:
    case T_TLIST: case T_IDENT: case ET_NIL:
        return 1;
    default:
        return 0;
    }
}

static const char *
type_name(struct ex_type *t)
{
    /* rotate buffers so two composite names can share one message and
     * `list<%s>` never formats into the buffer it is reading from */
    static char bufs[4][128];
    static int which;
    char *buf = bufs[which++ & 3];

    if (!t)
        return "any";
    switch (t->kind) {
    case ET_ANY:   return "any";
    case ET_NIL:   return "nil";
    case ET_SIGNAL: return "a can-fail signal";
    case ET_DEC:   return "decimal";    /* an unresolved decimal literal */
    case T_TINT:   return "int";
    case T_TFLOAT: return "float";
    case T_TDEC:   return "decimal";
    case T_TVEC:   return "vec";
    case T_TMAT:   return "mat";
    case T_TSTR:   return "str";
    case T_TOBJ:   return "obj";
    case T_TBOOL:  return "bool";
    case T_TERR:   return "err";
    case T_TPROP:  return "prop";
    case T_TLIST:
        snprintf(buf, sizeof bufs[0], "list<%s>", type_name(t->inner));
        return buf;
    case ET_MAYBE:
        snprintf(buf, sizeof bufs[0], "maybe %s", type_name(t->inner));
        return buf;
    case ET_SLICE: {
        int n = snprintf(buf, sizeof bufs[0], "obj with (");
        for (struct node *v = t->verbs; v; v = v->next)
            n += snprintf(buf + n, sizeof bufs[0] - (size_t)n, "%s%s",
                          v == t->verbs ? "" : ", ", v->name);
        snprintf(buf + n, sizeof bufs[0] - (size_t)n, ")");
        return buf;
    }
    case T_IDENT:  return t->name ? t->name : "?";
    default:       return "?type?";
    }
}

/* structural equality, treating any/prop as wildcards */
static int
type_equal(struct ex_type *a, struct ex_type *b)
{
    if (is_any(a) || is_any(b))
        return 1;
    if (a->kind == T_TPROP || b->kind == T_TPROP)
        return 1;
    if (a->kind != b->kind)
        return 0;
    switch (a->kind) {
    case T_TLIST:
        if (!a->inner || !b->inner)
            return 1;               /* list of unknown element */
        return type_equal(a->inner, b->inner);
    case ET_MAYBE:
        return type_equal(a->inner, b->inner);
    case T_IDENT:
        if (a->sym && b->sym)
            return a->sym == b->sym;
        return a->name && b->name && ex_ci_eq(a->name, b->name);
    default:
        return 1;                   /* same builtin kind */
    }
}

/* the slice a type denotes, seeing through a named one (`Openable`) */
static struct ex_type *
slice_of(struct ex_type *t)
{
    if (!t)
        return NULL;
    if (t->kind == ET_SLICE)
        return t;
    if (t->kind == T_IDENT && t->sym && t->sym->kind == SYM_INTERFACE)
        return t->sym->type;
    return NULL;
}

/* A selector's declaration, found anywhere in the program. A selector is
 * interned by name and dispatched by that id at run time (one id per name),
 * so a verb name determines one signature program-wide. That is what lets a
 * slice name verbs without repeating their signatures: the slice says which
 * selectors may be sent, and the declaration says what they take and return
 * (object-slices.md D3). The first declaration found wins; verb_agree checks
 * at declaration time that there is nothing to disagree with. */
static struct sym *
verb_by_selector(const char *name)
{
    for (struct node *it = cur_file ? cur_file->a : NULL; it; it = it->next) {
        if (it->kind != N_CLASS || !it->sym)
            continue;
        {
            struct sym *m = sym_member(it->sym, name);
            if (m && m->kind == SYM_VERB)
                return m;
        }
    }
    return NULL;
}

/* a slice's verbs as a readable list, for an error message */
static const char *
slice_verbs(struct ex_type *sl)
{
    static char buf[256];
    int n = 0;

    for (struct node *v = sl->verbs; v; v = v->next)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s`%s`",
                      n ? ", " : "", v->name);
    return buf;
}

/* Does a class's verb match a signature an interface declares? Parameter
 * count, each parameter's type, and the return type (interface-decl.md D6). */
static int
sig_matches(struct node *want, struct sym *have)
{
    struct node *hp = have->decl ? have->decl->a : NULL;
    struct node *wp = want->a;

    for (; wp && hp; wp = wp->next, hp = hp->next)
        if (!type_equal(wp->type, hp->type))
            return 0;
    if (wp || hp)
        return 0;                       /* different parameter counts */
    if (!want->type && !have->type)
        return 1;                       /* both valueless */
    if (!want->type || !have->type)
        return 0;
    return type_equal(want->type, have->type);
}

/* Does `src` satisfy the verb set of slice `sl` (object-slices.md D2/D3)?
 * A class conforms when it has a verb of each name; a wider slice conforms
 * to a narrower one, which is how a slice narrows to a smaller slice. The
 * signature is not compared here, because a selector's signature is global
 * to the program (one id per name, dispatched by that id), so agreement is
 * enforced once at declaration rather than at every conversion. */
static int
slice_conforms(struct ex_type *sl, struct ex_type *src)
{
    struct ex_type *ssl = slice_of(src);
    struct sym *cls = (src && src->kind == T_IDENT && src->sym &&
                       src->sym->kind == SYM_CLASS) ? src->sym : NULL;

    for (struct node *v = sl->verbs; v; v = v->next) {
        if (cls) {
            struct sym *m = sym_member(cls, v->name);
            if (!m || m->kind != SYM_VERB)
                return 0;
            /* a declared interface states the signature, so that is what
             * conformance compares (interface-decl.md D6); an inline slice
             * carries names only and leans on the selector rule */
            if (v->kind == N_VERB && !sig_matches(v, m))
                return 0;
        } else if (ssl) {
            int found = 0;
            for (struct node *w = ssl->verbs; w && !found; w = w->next)
                found = ex_ci_eq(w->name, v->name);
            if (!found)
                return 0;
        } else {
            return 0;               /* untyped obj: narrow with `as` (D6) */
        }
    }
    return 1;
}

/* may a value of type src be stored where dst is expected? */
static int
assignable(struct ex_type *dst, struct ex_type *src)
{
    struct ex_type *sl;

    if (is_any(dst) || is_any(src))
        return 1;
    /* a slice accepts anything that responds to its verbs (D2); the other
     * direction is a widening to plain `obj`, which asks nothing */
    if ((sl = slice_of(dst)) != NULL)
        return slice_conforms(sl, src);
    if (dst->kind == T_TOBJ && (slice_of(src) || src->kind == T_IDENT))
        return 1;
    if (dst->kind == T_TPROP || src->kind == T_TPROP)
        return 1;                   /* prop absorbs and yields anything */
    if (is_maybe(dst) || is_maybe(src))
        /* capture widening into a maybe, and the runtime-checked unwrap
         * out of one, both compare payloads (fallible.md) */
        return assignable(strip_maybe(dst), strip_maybe(src));
    if (src->kind == ET_NIL)
        return is_ref(dst);
    if (is_numeric(dst) && is_numeric(src)) {
        if (dst->kind == src->kind)
            return 1;
        /* an unresolved decimal literal adapts to decimal or float
         * (numbers.md); the consumer resolves it via resolve_dec */
        if (src->kind == ET_DEC)
            return dst->kind == T_TDEC || dst->kind == T_TFLOAT;
        /* one-way widening: int flows into float or decimal */
        return src->kind == T_TINT &&
               (dst->kind == T_TFLOAT || dst->kind == T_TDEC);
    }
    return type_equal(dst, src);
}

/* The two absence words are different (fallible.md, R6): `nothing` is
 * "no result" (a `maybe T`), `nil` is "no object" (a ref). A literal
 * used for the wrong one gets a teaching error naming the right word, in
 * both directions. Called at every value-to-slot boundary, before the
 * generic assignability check, so it also closes the hole where a bare
 * `nothing` (a `maybe any`) silently stored into a plain ref. */
static void
absence_mixup(struct node *src, struct ex_type *dst)
{
    struct ex_type *d;

    if (!src || !dst)
        return;
    d = strip_maybe(dst);
    if (src->kind == N_NOTHING && !is_maybe(dst) &&
        !is_any(d) && d->kind != T_TPROP)
        terr(src, is_ref(d)
                ? "`nothing` is an absent result; for an absent %s use "
                  "`nil` (`nothing` needs a `maybe T` slot)"
                : "`nothing` is an absent result; store it in a `maybe %s`, "
                  "not a plain %s",
             type_name(d), type_name(d));
    if (src->kind == N_NIL && is_maybe(dst))
        terr(src, "`nil` is an absent object; for an absent result write "
                  "`nothing` (this slot is `maybe %s`)",
             type_name(d));
}

/* A `can fail` call is a valueless signal, not a value (can-fail.md, R14):
 * it may be consumed by `if`/`while` or a bare statement, but not stored,
 * negated, or combined. Called wherever a value is expected. */
static void
no_signal(struct node *n, struct ex_type *t)
{
    if (t && t->kind == ET_SIGNAL)
        /* The consumers named here must be the ones that actually take a
         * signal: `if` and `while` are bool-only and reject it
         * (fallible-consumers.md), so naming them sent the author in a
         * circle, from this error to want_bool's and back. */
        terr(n, "a `can fail` call is a signal, not a value: consume it with "
                "`action() on fail ...` or a bare call; for a boolean fact "
                "to store or combine, call a `returns bool` predicate "
                "instead");
}

/* Resolve an untyped decimal constant expression (ET_DEC) to a concrete
 * numeric type, rewriting node types in place (numbers.md). Only nodes
 * typed ET_DEC are touched; an int-typed subtree under a mixed
 * expression stays int (lowering widens it). Resolving a literal to
 * decimal folds its scaled value into n->ival, and a value with nonzero
 * digits past the fourth does not fit: exact or an error. */
static void
resolve_dec(struct node *n, struct ex_type *to)
{
    if (!n || !n->type || n->type->kind != ET_DEC)
        return;
    if (n->kind == N_FLOAT) {
        if (to->kind == T_TDEC) {
            double s = n->fval * (double)DEC_SCALE;
            double k = (s < 0) ? s - 0.5 : s + 0.5;
            long scaled = (long)k;
            if (k > 2147483647.0 || k < -2147483648.0)
                terr(n, "decimal value out of range "
                        "(about +/- 214748.3647); use float");
            if ((double)scaled / (double)DEC_SCALE != n->fval)
                terr(n, "decimal holds four fraction digits; %g does "
                        "not fit; round it yourself, or use float",
                     n->fval);
            n->ival = scaled;
        }
        n->type = to;
        return;
    }
    switch (n->kind) {
    case N_SELECT:
    case N_MATCHEXPR:
        for (struct node *br = n->b; br; br = br->next)
            resolve_dec(br->kind == N_MATCHARM ? br->b : br, to);
        resolve_dec(n->c, to);
        break;
    default:
        resolve_dec(n->a, to);
        resolve_dec(n->b, to);
        resolve_dec(n->c, to);
        break;
    }
    n->type = to;
}

/* resolve toward a concrete destination type; decimal when the
 * destination gives no direction (the default, numbers.md) */
static void
resolve_dec_to(struct node *n, struct ex_type *dst)
{
    dst = strip_maybe(dst);
    resolve_dec(n, (dst && dst->kind == T_TFLOAT) ? ty_float : ty_dec);
}

/****************************************************************
 * Typed data literals (typed-data.md, the R5 pass)
 *
 * A `shape` is a schema for symbolic data literals: a sum of node kinds,
 * each a head word plus a sequence of typed slots. A symbolic literal
 * checks against a shape when a context type supplies one (a param,
 * field, ascription, or return); with none it stays `any`, the prior
 * behavior. Stage 1 to 3: head words, slot arity and scalar types, and
 * intra-tree label/target resolution. Holes and guard fragments (stage
 * 4) are deferred; a hole matches one slot without a type check yet.
 ****************************************************************/

/* a collected label declaration or `to` reference, with its source node */
struct shape_ref {
    const char *name;
    struct node *at;
    struct shape_ref *next;
};

struct shape_ctx {
    struct node *shape;                 /* the N_SHAPE declaration */
    struct shape_ref *labels, *labels_tail;
    struct shape_ref *refs, *refs_tail;
};

static void
shape_collect(struct shape_ref **head, struct shape_ref **tail,
              const char *name, struct node *at)
{
    struct shape_ref *r = arena_zalloc(ta, sizeof *r);
    r->name = name;
    r->at = at;
    if (*tail)
        (*tail)->next = r;
    else
        *head = r;
    *tail = r;
}

/* find a node-kind or group line by case-folded name */
static struct node *
shape_kind(struct node *shape, const char *name)
{
    for (struct node *k = shape->a; k; k = k->next)
        if (name && ex_ci_eq(k->name, name))
            return k;
    return NULL;
}

/* the head word of a data node `[head ...]`, or NULL if it has none */
static const char *
datalit_head(struct node *item)
{
    if (item && item->kind == N_DATALIT && item->a &&
        item->a->kind == N_NAME)
        return item->a->name;
    return NULL;
}

static const char *
shape_slot_desc(struct node *slot)
{
    static char buf[64];

    switch (slot->ival) {
    case T_TSTR:  return "a string";
    case T_TINT:  return "an int";
    case T_TDEC:  return "a decimal";
    case T_TBOOL: return "a bool";
    default:      break;
    }
    if (ex_ci_eq(slot->name, "label"))
        return "a label word";
    if (ex_ci_eq(slot->name, "ref"))
        return "a target word";
    snprintf(buf, sizeof buf, "a `%s`", slot->name);
    return buf;
}

static void shape_match_item(struct shape_ctx *c, struct node *item,
                             struct node *kind);

/* may `item` fill one `slot`? (structure only; interior checked on consume) */
static int
shape_slot_matches(struct shape_ctx *c, struct node *item, struct node *slot)
{
    struct node *k;
    const char *h;

    if (item->kind == N_HOLE)
        return 1;                       /* ${e}: one slot, typed at stage 4 */
    switch (slot->ival) {
    case T_TSTR:  return item->kind == N_STR;
    case T_TINT:  return item->kind == N_NUM;
    case T_TBOOL: return item->kind == N_BOOL;
    case T_TDEC:  return item->kind == N_NUM || item->kind == N_FLOAT;
    default:      break;                /* an IDENT-named slot */
    }
    if (ex_ci_eq(slot->name, "label") || ex_ci_eq(slot->name, "ref"))
        return item->kind == N_NAME;
    k = shape_kind(c->shape, slot->name);
    if (k) {                            /* a nested node of that kind/group */
        h = datalit_head(item);
        if (!h)
            return 0;
        if (k->flags & NF_SHAPE_GROUP) {
            for (struct node *alt = k->a; alt; alt = alt->next)
                if (ex_ci_eq(alt->name, h))
                    return 1;
            return 0;
        }
        return ex_ci_eq(k->name, h);
    }
    return item->kind == N_NAME && ex_ci_eq(item->name, slot->name);
}

/* record and recurse once `item` is known to fill `slot` */
static void
shape_consume(struct shape_ctx *c, struct node *item, struct node *slot)
{
    struct node *k;

    if (item->kind == N_HOLE || slot->ival != T_IDENT)
        return;                         /* hole or scalar leaf: nothing more */
    if (ex_ci_eq(slot->name, "label")) {
        for (struct shape_ref *r = c->labels; r; r = r->next)
            if (ex_ci_eq(r->name, item->name))
                terr(item, "duplicate label `%s` in this data "
                           "(each label names one node)", item->name);
        shape_collect(&c->labels, &c->labels_tail, item->name, item);
        return;
    }
    if (ex_ci_eq(slot->name, "ref")) {
        shape_collect(&c->refs, &c->refs_tail, item->name, item);
        return;
    }
    k = shape_kind(c->shape, slot->name);
    if (k) {
        if (k->flags & NF_SHAPE_GROUP)
            k = shape_kind(c->shape, datalit_head(item));   /* the alt */
        shape_match_item(c, item, k);
    }
    /* else: a literal keyword atom, already matched */
}

/* Report an item that does not fit where the schema expected one. A
 * sublist whose head is unknown is the misspelled-head case R5 names as
 * the flagship catch; a sublist whose head is a real kind but misplaced,
 * and a stray scalar, get their own message. */
static NORETURN void
shape_reject(struct shape_ctx *c, struct node *item, struct node *kind,
             struct node *slot)
{
    const char *h = datalit_head(item);

    if (h) {
        if (shape_kind(c->shape, h))
            terr(item->a, "a `%s` node is not allowed in a `%s` here",
                 h, kind->name);
        terr(item->a, "`%s` is not a known node kind (in shape `%s`)",
             h, c->shape->name);
    }
    if (slot)
        terr(item, "a `%s` node needs %s here", kind->name,
             shape_slot_desc(slot));
    terr(item, "extra item in a `%s` node", kind->name);
}

/* match a node's body items against a node kind's slot sequence. Greedy,
 * no backtracking: adequate while slots are type-distinguishable, which
 * dialog and stat-table grammars are (the exact grammar is settled here
 * per typed-data.md decision D6). */
static void
shape_match_body(struct shape_ctx *c, struct node *items, struct node *kind)
{
    struct node *item = items;

    for (struct node *slot = kind->a; slot; slot = slot->next) {
        int many = (slot->op == T_STAR || slot->op == T_PLUS);
        int min = (slot->op == T_STAR) ? 0 : 1;
        int count = 0;

        while ((many || count < 1) && item &&
               shape_slot_matches(c, item, slot)) {
            shape_consume(c, item, slot);
            item = item->next;
            count++;
        }
        if (count < min) {
            if (item)
                shape_reject(c, item, kind, slot);
            terr(kind, "a `%s` node needs %s here", kind->name,
                 shape_slot_desc(slot));
        }
    }
    if (item)
        shape_reject(c, item, kind, NULL);
}

static void
shape_match_item(struct shape_ctx *c, struct node *item, struct node *kind)
{
    const char *h;

    if (item->kind != N_DATALIT)
        terr(item, "expected a `%s` node here, written `[%s ...]`",
             kind->name, kind->name);
    h = datalit_head(item);
    if (!h)
        terr(item, "a data node needs a head word; `%s` was expected",
             kind->name);
    if (!ex_ci_eq(h, kind->name))
        terr(item->a, "`%s` is not a known node here; expected `%s`",
             h, kind->name);
    shape_match_body(c, item->a->next, kind);   /* body follows the head */
}

/* Check a symbolic data literal against a shape (typed-data.md). The
 * literal's head must be the shape's root (its first node kind); every
 * node, slot, and `to` reference is validated. */
static void
check_shape(struct node *lit, struct node *shape)
{
    struct shape_ctx c;
    struct node *root = shape->a;

    c.shape = shape;
    c.labels = c.labels_tail = NULL;
    c.refs = c.refs_tail = NULL;
    while (root && (root->flags & NF_SHAPE_GROUP))
        root = root->next;              /* the root is the first node kind */
    if (!root)
        terr(lit, "shape `%s` declares no node kind", shape->name);
    if (!datalit_head(lit))
        terr(lit, "a `%s` value is a data node `[%s ...]`, not an empty "
                  "or plain literal", shape->name, root->name);
    shape_match_item(&c, lit, root);
    for (struct shape_ref *r = c.refs; r; r = r->next) {
        int found = 0;
        for (struct shape_ref *l = c.labels; l; l = l->next)
            if (ex_ci_eq(l->name, r->name)) {
                found = 1;
                break;
            }
        if (!found)
            terr(r->at, "`%s` points to no label in this data "
                        "(a `to` target must name a labelled node)",
                 r->name);
    }
}

/* if `dst` is a shape and `n` a symbolic data literal, structurally check
 * the literal against the schema (typed-data.md). Returns 1 when it was a
 * shape boundary (checked, or left to the caller's error path). */
static int
maybe_check_shape(struct node *n, struct ex_type *dst)
{
    struct ex_type *d = strip_maybe(dst);

    if (n && n->kind == N_DATALIT && d && d->kind == T_IDENT &&
        d->sym && d->sym->kind == SYM_SHAPE) {
        check_shape(n, d->sym->decl);
        return 1;
    }
    return 0;
}

/* The int-division trap (numbers.md, decision 5): an int/int quotient
 * silently widening into decimal or float truncates before it widens
 * (`var half is decimal = 1 / 2` would be 0.0), so it is a compile
 * error at every implicit widening boundary. A teaching check, not a
 * soundness check: it inspects the widened expression's outermost
 * operator, and the explicit `as decimal` cast stays the sanctioned
 * spelling for a truncated quotient. resolve_dec_to stays separate so
 * the cast path does not fire it. */
static void
widen_to(struct node *n, struct ex_type *dst)
{
    struct ex_type *d = strip_maybe(dst);

    if (maybe_check_shape(n, dst))
        return;                         /* a data literal met its schema */
    if (d && (d->kind == T_TDEC || d->kind == T_TFLOAT) &&
        n && n->kind == N_BINOP && n->op == T_SLASH &&
        n->type && strip_maybe(n->type)->kind == T_TINT)
        terr(n, "integer division truncates: this quotient is an int "
                "(1 / 2 is 0, not 0.5); write a decimal operand "
                "(`1.0 / 2`) for %s division, or keep the truncated "
                "quotient explicitly with `as %s`",
             type_name(d), type_name(d));
    resolve_dec_to(n, dst);
}

/* the default boundary: an unresolved decimal becomes decimal */
static struct ex_type *
dec_default(struct node *n, struct ex_type *t)
{
    if (t && t->kind == ET_DEC) {
        resolve_dec(n, ty_dec);
        return ty_dec;
    }
    return t;
}

/* resolve an unresolved decimal comparand toward the other side */
static void
cmp_resolve_dec(struct node *n, struct ex_type **lp, struct ex_type **rp)
{
    struct ex_type *l = *lp, *r = *rp, *o, *to;

    if (l->kind != ET_DEC && r->kind != ET_DEC)
        return;
    o = (l->kind == ET_DEC) ? r : l;
    if (o->kind == ET_DEC || is_any(o))
        to = ty_dec;                    /* both literals, or unknown */
    else if (o->kind == T_TFLOAT)
        to = ty_float;
    else if (o->kind == T_TDEC || o->kind == T_TINT)
        to = ty_dec;                    /* an int comparand widens */
    else
        return;                         /* let the caller's check error */
    resolve_dec(n->a, to);
    resolve_dec(n->b, to);
    if (l->kind == ET_DEC)
        *lp = to;
    if (r->kind == ET_DEC)
        *rp = to;
}

/* Resolve a chooser's unresolved decimal branches to its shared type
 * (numbers.md): an all-literal chooser defaults to decimal, and a
 * literal branch adapts to a concrete decimal/float branch type. */
static struct ex_type *
chooser_resolve_dec(struct node *n, struct ex_type *t)
{
    struct ex_type *to;

    if (!t || (t->kind != ET_DEC && t->kind != T_TDEC &&
               t->kind != T_TFLOAT))
        return t;
    to = (t->kind == T_TFLOAT) ? ty_float : ty_dec;
    switch (n->kind) {
    case N_SELECT:
        for (struct node *br = n->b; br; br = br->next)
            resolve_dec(br, to);
        break;
    case N_MATCHEXPR:
        for (struct node *arm = n->b; arm; arm = arm->next)
            resolve_dec(arm->b, to);
        break;
    case N_THENELSE:
        resolve_dec(n->b, to);
        break;
    case N_OTHERWISE:
        resolve_dec(n->a, to);
        resolve_dec(n->b, to);
        break;
    default:
        return t;
    }
    resolve_dec(n->c, to);
    return (t->kind == ET_DEC) ? to : t;
}

/* result type of a binary arithmetic operator */
static struct ex_type *
arith_result(struct node *n, struct ex_type *l, struct ex_type *r)
{
    if (is_any(l) || is_any(r))
        return ty_any;
    if (!is_numeric(l) || !is_numeric(r))
        terr(n, "arithmetic needs numeric operands, got %s and %s",
             type_name(l), type_name(r));
    if (l->kind == T_TFLOAT || r->kind == T_TFLOAT) {
        if (l->kind == T_TDEC || r->kind == T_TDEC)
            terr(n, "cannot mix decimal and float; use an `as` cast");
        return ty_float;                /* an ET_DEC operand adapts */
    }
    if (l->kind == T_TDEC || r->kind == T_TDEC)
        return ty_dec;
    if (l->kind == ET_DEC || r->kind == ET_DEC)
        return ty_declit;               /* dec op dec / dec op int */
    return ty_int;
}

/****************************************************************
 * Expression checking
 ****************************************************************/

static struct ex_type *check_expr(struct node *n);
static void expand_macro(struct node *call, struct sym *mac);

/* a `shared` argument must be an lvalue of a qualifying place: a local, a
 * self field, or a forwarded parameter (shared-params.md D3/D4). A computed
 * value, an index, or a const cannot be passed by reference. */
static int
is_shared_lvalue(struct node *a)
{
    if (a->kind == N_NAME && a->sym &&
        (a->sym->kind == SYM_LOCAL || a->sym->kind == SYM_PARAM ||
         a->sym->kind == SYM_FIELD))
        return 1;
    return a->kind == N_FIELD_ACC && a->a->kind == N_SELF &&
           a->sym && a->sym->kind == SYM_FIELD;
}

static void
check_shared_arg(struct node *a, struct node *p, struct sym *fn)
{
    if (p && (p->flags & NF_SHARED) && !is_shared_lvalue(a))
        terr(a, "a `shared` argument to `%s` must be a variable or a self "
                "field (the caller's own place), not a computed value",
             fn->name);
}

static void
check_args(struct node *n, struct sym *fn)
{
    struct node *param = fn->decl ? fn->decl->a : NULL;
    struct node *arg = n->b;
    int np = 0, na = 0;

    for (struct node *p = param; p; p = p->next)
        np++;
    for (struct node *a = n->b; a; a = a->next)
        na++;
    (void)na;
    if (arg && arg->alias) {
        /* named call: each arg names a real parameter (no unknown, no
         * duplicate, all named). A parameter with a default may be omitted;
         * one without a default must be named (the safety rule). Matched by
         * name, not reordered; the lowering resolves each parameter. */
        for (struct node *a = n->b; a; a = a->next) {
            struct node *p = NULL, *g;
            struct ex_type *at;
            if (!a->alias)
                terr(a, "arguments to `%s` are all named or all positional",
                     fn->name);
            for (g = param; g; g = g->next)
                if (ex_ci_eq(g->name, a->alias)) { p = g; break; }
            if (!p)
                terr(a, "`%s` has no parameter `%s`", fn->name, a->alias);
            for (g = n->b; g != a; g = g->next)
                if (g->alias && ex_ci_eq(g->alias, a->alias))
                    terr(a, "argument `%s` given twice", a->alias);
            at = check_expr(a);
            no_signal(a, at);
            if (p) {
                absence_mixup(a, p->type);
                if (!assignable(p->type, at))
                    terr(a, "argument `%s` to `%s` is %s, expected %s",
                         a->alias, fn->name, type_name(at), type_name(p->type));
                widen_to(a, p->type);
                check_shared_arg(a, p, fn);
            }
        }
        for (struct node *p = param; p; p = p->next) {
            int have = 0;
            if (p->a)
                continue;               /* has a default */
            for (struct node *a = n->b; a; a = a->next)
                if (a->alias && ex_ci_eq(a->alias, p->name)) { have = 1; break; }
            if (!have)
                terr(n, "call to `%s` needs argument `%s` (it has no default)",
                     fn->name, p->name);
        }
        return;
    }
    /* positional: at most np args; a trailing parameter may be omitted only
     * when it has a default. */
    for (; arg && param; arg = arg->next, param = param->next) {
        struct ex_type *at = check_expr(arg);
        no_signal(arg, at);
        if (arg->alias)
            terr(arg, "arguments to `%s` are all named or all positional",
                 fn->name);
        absence_mixup(arg, param->type);
        if (!assignable(param->type, at))
            terr(arg, "argument to `%s` is %s, expected %s",
                 fn->name, type_name(at), type_name(param->type));
        widen_to(arg, param->type);
        check_shared_arg(arg, param, fn);
    }
    if (arg)
        terr(n, "call to `%s` expects at most %d argument%s, got %d",
             fn->name, np, np == 1 ? "" : "s", na);
    for (; param; param = param->next)
        if (!param->a)
            terr(n, "call to `%s` is missing argument `%s` (it has no default)",
                 fn->name, param->name);
}

/* A send resolves its selector against the receiver's class when the
 * receiver has a statically-known local class (self, or a class-typed
 * value). It then type-checks like a call and takes the verb's return
 * type. An untyped or external receiver (obj, prop, imported class) stays
 * dynamic: the selector is unresolved and the result is `any`. */
static struct ex_type *
check_send(struct node *n)
{
    struct ex_type *recv = check_expr(n->a);
    struct sym *cls = NULL;
    struct sym *v;

    if (recv && recv->kind == T_IDENT && recv->sym &&
        recv->sym->kind == SYM_CLASS)
        cls = recv->sym;

    {   /* a slice permits exactly the verbs it names (object-slices.md D5):
         * the holder's authority is the slice, not the runtime object */
        struct ex_type *sl = slice_of(recv);

        if (sl) {
            for (struct node *v = sl->verbs; v; v = v->next)
                if (ex_ci_eq(v->name, n->name)) {
                    /* a declared interface states the signature; an inline
                     * slice falls back to the selector, whose signature is
                     * program-global (interface-decl.md D4/D6) */
                    struct sym *decl = v->sym ? v->sym
                                              : verb_by_selector(n->name);
                    if (!decl) {
                        for (struct node *a = n->b; a; a = a->next)
                            dec_default(a, check_expr(a));
                        return ty_any;
                    }
                    n->sym = decl;
                    check_args(n, decl);
                    return decl->type ? decl->type : ty_any;
                }
            terr(n, "this is an object slice, which permits only %s; `%s` is "
                    "not one of them", slice_verbs(sl), n->name);
        }
    }

    if (!cls) {                     /* dynamic receiver: check args loosely */
        for (struct node *a = n->b; a; a = a->next)
            dec_default(a, check_expr(a));
        return ty_any;
    }
    v = sym_member(cls, n->name);
    if (!v)
        terr(n, "`%s` has no verb `%s`", cls->name, n->name);
    if (v->kind != SYM_VERB)
        terr(n, "`%s` is a %s, not a verb", n->name, sym_kind_name(v->kind));
    n->sym = v;
    check_args(n, v);
    return v->type ? v->type : ty_any;
}

static struct ex_type *
check_name(struct node *n)
{
    struct sym *s = n->sym;

    if (!s)
        return ty_any;              /* external / prelude */
    switch (s->kind) {
    case SYM_CONST: case SYM_PARAM: case SYM_LOCAL: case SYM_FIELD:
        return s->type ? s->type : ty_any;
    case SYM_VERB:
    case SYM_FUNC:
        terr(n, "`%s` is a %s; call it with `(...)`", n->name,
             s->kind == SYM_VERB ? "verb" : "func");
    case SYM_ENUM:
    case SYM_RECORD:
    case SYM_CLASS:
        terr(n, "`%s` is a type, not a value", n->name);
    case SYM_MODULE:
    case SYM_IMPORT:
        terr(n, "`%s` is an imported module, not a value", n->name);
    default:                        /* enum member reached bare: not a value */
        terr(n, "`%s` is not a value", n->name);
    }
}

/* a `.`-access whose base is a name that qualifies rather than a value:
 * `Element.fire` (enum), `it.Sunstone` (module), `SomeClass.x` */
static int
is_qualifier(struct sym *s)
{
    return s && (s->kind == SYM_ENUM || s->kind == SYM_MODULE ||
                 s->kind == SYM_IMPORT || s->kind == SYM_CLASS ||
                 s->kind == SYM_RECORD);
}

static struct ex_type *
check_member_value(struct node *n, struct sym *m)
{
    if (m->kind == SYM_ENUM_MEMBER)
        return named_type(n->a->sym);
    if (m->kind == SYM_VERB || m->kind == SYM_FUNC)
        terr(n, "`%s` is a verb, not a value; call it: recv.%s(...)",
             n->name, n->name);
    return m->type ? m->type : ty_any;
}

/* the field mask of a field-restricted parameter `p is R with (x, y)`
 * (record-slicing.md D4), or NULL when the base is not such a view */
static struct node *
param_field_mask(struct node *base)
{
    return (base->kind == N_NAME && base->sym &&
            base->sym->kind == SYM_PARAM && base->sym->decl)
         ? base->sym->decl->c : NULL;
}

static struct ex_type *
check_field(struct node *n)
{
    struct ex_type *base;
    struct sym *bsym = n->a->kind == N_NAME ? n->a->sym : NULL;

    /* module- / enum- / class-qualified: the base is not a value */
    if (is_qualifier(bsym)) {
        if (n->sym)
            return check_member_value(n, n->sym);
        return ty_any;              /* external member of an imported module */
    }

    /* a fallible base (a list/str index yields `maybe T`) is seen through to
     * its record/class for the member lookup; the failure is consumed at the
     * index site, so `xs[i].field` resolves the field on T */
    base = strip_maybe(check_expr(n->a));
    if (n->sym)                     /* self.field (resolved) */
        return check_member_value(n, n->sym);
    /* typed obj.field: look the field up in the object's class members */
    if (base && base->kind == T_IDENT && base->sym) {
        struct sym *m = sym_member(base->sym, n->name);
        if (m) {
            struct node *mask = param_field_mask(n->a);
            if (mask) {             /* a field-restricted view (D4) */
                int ok = 0;
                for (struct node *f = mask; f; f = f->next)
                    if (ex_ci_eq(f->name, n->name)) { ok = 1; break; }
                if (!ok)
                    terr(n, "`%s` is a view restricted to its listed fields; "
                            "`%s` is not one of them", n->a->name, n->name);
            }
            n->sym = m;
            return check_member_value(n, m);
        }
    }
    return ty_any;                  /* inherited / external field */
}

static struct ex_type *
check_index(struct node *n)
{
    struct ex_type *base = strip_maybe(check_expr(n->a));
    struct ex_type *idx = strip_maybe(check_expr(n->b));

    if (!is_any(idx) && idx->kind != T_TINT)
        terr(n->b, "index must be int, got %s", type_name(idx));
    if (is_any(base))
        return ty_any;
    if (base->kind == T_TLIST)
        return base->inner ? base->inner : ty_any;
    if (base->kind == T_TSTR)
        return ty_str;
    terr(n, "cannot index a %s", type_name(base));
}

static struct sym *set_enum_sym(struct ex_type *t);
static void check_set(struct node *n, struct sym *en, struct ex_type *st);

static struct ex_type *
check_binop(struct node *n)
{
    struct ex_type *l, *r;

    /* set membership (set-of.md D6): `x in s` / `x overlaps s`. The set
     * operand pins the element enum, so the left is checked in set-mode (its
     * members may be bare). `overlaps` is set-only; `in` falls through to
     * list/str membership when the right is not a set. */
    if (n->op == T_IN || n->op == T_OVERLAPS) {
        struct sym *sen = set_enum_sym(check_expr(n->b));
        if (sen) {
            check_set(n->a, sen, n->b->type);
            return ty_bool;
        }
        if (n->op == T_OVERLAPS)
            terr(n, "`overlaps` compares two sets, got %s",
                 type_name(n->b->type));
    }

    l = check_expr(n->a);
    r = check_expr(n->b);

    no_signal(n->a, l);                 /* a can-fail call is not a value */
    no_signal(n->b, r);
    if (is_maybe(l) || is_maybe(r)) {   /* failure propagates outward */
        n->flags |= NF_FALLIBLE;
        l = strip_maybe(l);
        r = strip_maybe(r);
    }
    switch (n->op) {
    case T_AND: case T_OR: case T_XOR:
        if (!is_any(l) && l->kind != T_TBOOL)
            terr(n->a, "`%s` needs bool operands, got %s",
                 tok_str(n->op), type_name(l));
        if (!is_any(r) && r->kind != T_TBOOL)
            terr(n->b, "`%s` needs bool operands, got %s",
                 tok_str(n->op), type_name(r));
        return ty_bool;
    case T_EQ: case T_NE:
        cmp_resolve_dec(n, &l, &r);
        if (!assignable(l, r) && !assignable(r, l))
            terr(n, "cannot compare %s with %s",
                 type_name(l), type_name(r));
        return ty_bool;
    case T_IN:                          /* element in list, or substr in str */
        if (is_any(r))
            return ty_bool;
        if (r->kind == T_TLIST) {
            if (r->inner && !is_any(l) && !assignable(r->inner, l))
                terr(n->a, "`in` element is %s, list holds %s",
                     type_name(l), type_name(r->inner));
            return ty_bool;
        }
        if (r->kind == T_TSTR) {
            if (!is_any(l) && l->kind != T_TSTR)
                terr(n->a, "`in` a str needs a str, got %s", type_name(l));
            return ty_bool;
        }
        terr(n, "`in` needs a list or str on the right, got %s",
             type_name(r));
    case T_LT: case T_LE: case T_GT: case T_GE:
        cmp_resolve_dec(n, &l, &r);
        if (!(is_numeric(l) && is_numeric(r)) &&
            !(l->kind == T_TSTR && r->kind == T_TSTR) &&
            !is_any(l) && !is_any(r))
            terr(n, "cannot order %s against %s",
                 type_name(l), type_name(r));
        return ty_bool;
    default:                        /* + - * / % */
        if ((n->op == T_PLUS) && !is_numeric(l) && !is_numeric(r) &&
            type_equal(l, r) &&
            (l->kind == T_TSTR || l->kind == T_TVEC || l->kind == T_TMAT ||
             l->kind == T_TLIST))
            return l;               /* str / list / vec / mat concat-add */
        {
            struct ex_type *rt = arith_result(n, l, r);
            /* division never stays an unresolved literal: it is a
             * fallible decimal divide (numbers.md) */
            if ((n->op == T_SLASH || n->op == T_PERCENT) &&
                rt->kind == ET_DEC)
                rt = ty_dec;
            if (rt->kind == T_TFLOAT) {
                resolve_dec(n->a, ty_float);
                resolve_dec(n->b, ty_float);
            } else if (rt->kind == T_TDEC) {
                resolve_dec(n->a, ty_dec);
                resolve_dec(n->b, ty_dec);
            }
            /* int/decimal divide and modulo can fail (zero divisor), so
             * they are fallible producers (runtime-errors.md); float is
             * untouched (IEEE gives inf/nan). A constant divisor is
             * decided at compile time: a nonzero literal cannot fail and
             * stays infallible (no `otherwise`/`if var` for `a / 2`); a
             * literal zero is a guaranteed fault, caught now as an error
             * rather than deferred to runtime (numbers.md). */
            if ((n->op == T_SLASH || n->op == T_PERCENT) &&
                (rt->kind == T_TINT || rt->kind == T_TDEC)) {
                struct node *d = n->b;
                int lit = d->kind == N_NUM || d->kind == N_FLOAT;
                int zero = (d->kind == N_NUM && d->ival == 0) ||
                           (d->kind == N_FLOAT && d->fval == 0.0);
                if (zero)
                    terr(n, "division by zero: the divisor is a constant 0");
                else if (!lit)
                    n->flags |= NF_FALLIBLE;
            }
            return rt;
        }
    }
}

/* match labels are constants: int/fixed/bool literals, or a name bound
 * to a module const. Each label (both ends of a range) must agree with
 * the subject's type. */
static int
is_enum_type(struct ex_type *t)
{
    return t && t->kind == T_IDENT && t->sym && t->sym->kind == SYM_ENUM;
}

/* `set of E`: the element enum's sym, or NULL if t is not a set (set-of.md) */
static struct sym *
set_enum_sym(struct ex_type *t)
{
    struct ex_type *in;
    if (!t || t->kind != T_TSET)
        return NULL;
    in = t->inner;
    return (in && in->kind == T_IDENT && in->sym &&
            in->sym->kind == SYM_ENUM) ? in->sym : NULL;
}

/* check a set-construction expression against its element enum: members
 * (bare in this set-pinned context, D4, or qualified), `empty`/`full`, and
 * the `+`/`-`/`^` set operators. Marks each node's type as the set type. */
static void
check_set(struct node *n, struct sym *en, struct ex_type *st)
{
    struct ex_type *t;

    if (n->kind == N_BINOP &&
        (n->op == T_PLUS || n->op == T_MINUS || n->op == T_CARET)) {
        check_set(n->a, en, st);
        check_set(n->b, en, st);
        n->type = st;
        return;
    }
    if (n->kind == N_NAME) {
        /* `empty` / `full` are contextual set literals (not keywords, so the
         * `set` list builtin and an `empty` variable keep their names); a
         * bare member pins to this set's enum (D4) */
        struct sym *m;
        if (ex_ci_eq(n->name, "empty") || ex_ci_eq(n->name, "full")) {
            n->kind = ex_ci_eq(n->name, "empty") ? N_EMPTY : N_FULL;
            n->type = st;
            return;
        }
        m = sym_member(en, n->name);
        if (m) {
            n->sym = m;
            n->type = st;
            return;
        }
    }
    t = check_expr(n);
    if (n->kind == N_FIELD_ACC && n->sym &&           /* qualified E.member */
        n->sym->kind == SYM_ENUM_MEMBER && n->sym->owner == en) {
        n->type = st;
        return;
    }
    if (set_enum_sym(t) == en)                        /* a set-valued var/call */
        return;
    if (is_enum_type(t) && t->sym == en)              /* a runtime enum value */
        return;                                       /* lowers to 1 << value */
    terr(n, "a `set of %s` is built from its members, `empty`, `full`, "
            "and `+` `-` `^`", en->name);
}

static void
check_match_arm_labels(struct node *arm, struct ex_type *subj)
{
    int enum_subj = is_enum_type(subj);

    for (struct node *v = arm->a; v; v = v->next) {
        struct node *lab = v->kind == N_RANGE ? v->a : v;
        int second = 0;

        if (enum_subj) {
            /* an enum arm names bare members (enums.md D2/D3); no ranges,
             * since enums are unordered (D5) */
            struct sym *m;
            if (v->kind == N_RANGE)
                terr(v, "an enum match uses member labels, not a `to` range "
                        "(enum members are unordered)");
            if (v->kind != N_NAME)
                terr(v, "an enum match label is a member name");
            m = sym_member(subj->sym, v->name);
            if (!m)
                terr(v, "`%s` is not a member of %s", v->name,
                     type_name(subj));
            v->sym = m;
            v->type = subj;
            continue;
        }

        for (;;) {
            struct ex_type *vt;

            /* The R4 arm-boundary trap retired with the `when` arm word
             * (match-when.md D4): an arm is only ever a line that opens
             * with `when`, so a body statement is never stolen as a label
             * and the parenthesize hint has nothing left to explain. */
            if (lab->kind == N_NAME &&
                (!lab->sym || lab->sym->kind != SYM_CONST))
                terr(lab, "match label `%s` must be a constant", lab->name);
            vt = check_expr(lab);
            if (vt->kind == ET_DEC) {   /* a decimal label (numbers.md) */
                resolve_dec(lab, ty_dec);
                vt = ty_dec;
            }
            if (!assignable(subj, vt) && !assignable(vt, subj))
                terr(lab, "label %s does not match the subject type %s",
                     type_name(vt), type_name(subj));
            if (v->kind != N_RANGE || second)
                break;
            lab = v->b;
            second = 1;
        }
    }
}

static struct ex_type *
check_match_subject(struct node *n)
{
    struct ex_type *subj = strip_maybe(check_expr(n->a));

    subj = dec_default(n->a, subj);     /* a literal subject is decimal */
    if (!is_any(subj) && subj->kind != T_TINT && subj->kind != T_TBOOL &&
        subj->kind != T_TDEC && !is_enum_type(subj))
        terr(n->a, "a match subject must be int, bool, decimal, or an enum; "
                   "got %s", type_name(subj));
    return subj;
}

/* an enum `match` must cover every member or carry an `otherwise` (enums.md
 * D4); a gap is a compile error naming the missing members */
static void
check_enum_exhaustive(struct node *n, struct ex_type *subj,
                      struct node *arms, struct node *otherwise)
{
    struct node *mem;
    char missing[256];
    int len = 0, n_missing = 0;

    if (otherwise || !is_enum_type(subj))
        return;
    for (mem = subj->sym->decl ? subj->sym->decl->a : NULL; mem;
         mem = mem->next) {
        int covered = 0;
        for (struct node *arm = arms; arm && !covered; arm = arm->next)
            for (struct node *v = arm->a; v; v = v->next)
                if (v->kind == N_NAME && v->sym == mem->sym) {
                    covered = 1;
                    break;
                }
        if (!covered) {
            n_missing++;
            len += snprintf(missing + len, sizeof missing - len, "%s%s",
                            len ? ", " : "", mem->name);
            if (len >= (int)sizeof missing)
                break;
        }
    }
    if (n_missing)
        terr(n, "this match on %s is missing %s; cover %s or add `otherwise`",
             type_name(subj), n_missing == 1 ? "a member" : "members",
             missing);
}

static struct ex_type *
check_expr(struct node *n)
{
    struct ex_type *t;

    if (!n)
        return ty_any;

    switch (n->kind) {
    case N_NUM:   t = ty_int;   break;
    case N_FLOAT: t = ty_declit; break;     /* adapts; numbers.md */
    case N_STR:   t = ty_str;   break;
    case N_EMPTY:                           /* set identity literals: their */
    case N_FULL:  t = ty_any;   break;      /* set type comes from context */
    case N_TOSTR: {
        /* a string hole stringifies by static type; a list or obj has
         * no text form (the lowerer would print its machine word) */
        struct ex_type *ht = check_expr(n->a);
        if (is_maybe(ht)) {
            n->flags |= NF_FALLIBLE;
            ht = strip_maybe(ht);
        }
        ht = dec_default(n->a, ht);     /* a hole literal is decimal */
        if (!is_any(ht) && ht->kind != T_TSTR && ht->kind != T_TINT &&
            ht->kind != T_TBOOL && ht->kind != T_TFLOAT &&
            ht->kind != T_TDEC && !is_enum_type(ht))
            terr(n->a, "cannot write a %s into a string hole; only str, "
                       "int, bool, float, decimal, and enums have a text form",
                 type_name(ht));
        t = ty_str;
        break;
    }
    case N_THENELSE: {                  /* cond then A else B */
        /* The if-expression binds loosest, so everything left of `then` is
         * its condition. `x = 1 + a then b else c` therefore reads `1 + a`
         * as the condition and the error lands on the arithmetic, naming
         * something the author did not write. Carry the real explanation
         * through whatever fires while the condition is checked. */
        const char *outer_note = terr_note;
        struct ex_type *ct;
        struct ex_type *bt, *et;

        terr_note = "the if-expression binds loosest, so everything to the "
                    "left of `then` is its condition; parenthesize it when "
                    "it is not the whole expression, as in "
                    "`1 + (a then b else c)`";
        ct = check_expr(n->a);
        if (is_maybe(ct)) {
            n->flags |= NF_FALLIBLE;
            ct = strip_maybe(ct);
        }
        if (!is_any(ct) && ct->kind != T_TBOOL) {
            terr_note = outer_note;
            terr(n->a, "the condition of `then ... else` must be bool, got "
                       "%s; the if-expression binds loosest, so everything "
                       "to its left is the condition (parenthesize it when "
                       "it is not the whole expression)", type_name(ct));
        }
        terr_note = outer_note;
        bt = check_expr(n->b);
        et = check_expr(n->c);
        if (is_maybe(bt) || is_maybe(et))
            n->flags |= NF_FALLIBLE;
        bt = strip_maybe(bt);
        et = strip_maybe(et);
        if (!is_any(bt) && !is_any(et) &&
            !assignable(bt, et) && !assignable(et, bt))
            terr(n->c, "then/else branches must share a type; got %s "
                       "and %s", type_name(bt), type_name(et));
        t = is_any(bt) ? et : bt;
        break;
    }
    case N_SELECT: {                    /* select idx from ... else ... */
        struct ex_type *it = check_expr(n->a);
        struct ex_type *rt = NULL;

        if (is_maybe(it)) {
            n->flags |= NF_FALLIBLE;
            it = strip_maybe(it);
        }
        if (!is_any(it) && it->kind != T_TINT)
            terr(n->a, "a select index must be int, got %s", type_name(it));
        for (struct node *br = n->b; br; br = br->next) {
            struct ex_type *bt = check_expr(br);
            if (!rt || is_any(rt))
                rt = bt;
            else if (!is_any(bt) && !assignable(rt, bt) && !assignable(bt, rt))
                terr(br, "select branches must share a type; got %s and %s",
                     type_name(rt), type_name(bt));
        }
        if (n->c) {
            struct ex_type *et = check_expr(n->c);
            if (rt && !is_any(rt) && !is_any(et) &&
                !assignable(rt, et) && !assignable(et, rt))
                terr(n->c, "select else must match the branch type %s, got %s",
                     type_name(rt), type_name(et));
        }
        t = rt ? rt : ty_any;
        break;
    }
    case N_MATCHEXPR: {                  /* case E of vals -> e ... else -> e */
        struct ex_type *subj = check_match_subject(n);
        struct ex_type *rt = NULL;

        for (struct node *arm = n->b; arm; arm = arm->next) {
            struct ex_type *bt;

            check_match_arm_labels(arm, subj);
            bt = check_expr(arm->b);
            if (!rt || is_any(rt))
                rt = bt;
            else if (!is_any(bt) && !assignable(rt, bt) &&
                     !assignable(bt, rt))
                terr(arm->b, "match arms must share a type; got %s and %s",
                     type_name(rt), type_name(bt));
        }
        if (n->c) {
            struct ex_type *et = check_expr(n->c);
            if (rt && !is_any(rt) && !is_any(et) &&
                !assignable(rt, et) && !assignable(et, rt))
                terr(n->c, "the match else must share the arm type %s, got %s",
                     type_name(rt), type_name(et));
        }
        t = rt ? rt : ty_any;
        break;
    }
    case N_OTHERWISE: {                      /* A otherwise B: consume A's failure */
        struct ex_type *lt = check_expr(n->a);
        struct ex_type *rt = check_expr(n->b);

        if (lt && lt->kind == ET_SIGNAL)
            /* `otherwise` is a value fallback; a valueless `can fail`
             * action has no value to fall back to. Its failure is handled
             * by the statement-level `on fail` (fallback-words.md). */
            terr(n->a, "`otherwise` is a value fallback; a `can fail` action "
                       "has no value. Handle its failure with `on fail` or a "
                       "bare call");
        if (!is_any(lt) && !is_maybe(lt))
            terr(n->a, "the left side of `otherwise` cannot fail; there is "
                       "nothing to fall back from");
        lt = strip_maybe(lt);
        if (is_maybe(rt)) {             /* B's own failure propagates on */
            n->flags |= NF_FALLIBLE;
            rt = strip_maybe(rt);
        }
        if (!is_any(lt) && !is_any(rt) &&
            !assignable(lt, rt) && !assignable(rt, lt))
            terr(n->b, "`otherwise` fallback is %s, does not match %s",
                 type_name(rt), type_name(lt));
        t = is_any(lt) ? rt : lt;
        break;
    }
    case N_BOOL:  t = ty_bool;  break;
    case N_NIL:   t = ty_nil;   break;
    case N_TYPELIT:
        /* a type argument only means something to a macro, which binds it
         * before this runs (typed-macros.md D4) */
        terr(n, "a type is not a value here; naming a type as an argument is "
                "only meaningful to a macro that is parametric over it");
    case N_NOTHING: t = ty_nothing; break;
    case N_NAME:  t = check_name(n); break;
    case N_SELF:  t = named_type(n->sym); break;
    case N_FIELD_ACC: t = check_field(n); break;
    case N_INDEX:
        n->flags |= NF_FALLIBLE;    /* out of range fails (fallible.md) */
        t = check_index(n);
        break;
    case N_BINOP: t = check_binop(n); break;
    case N_UNOP:
        t = check_expr(n->a);
        no_signal(n->a, t);         /* `not validate(k)` etc. is not a value */
        if (is_maybe(t)) {
            n->flags |= NF_FALLIBLE;
            t = strip_maybe(t);
        }
        if (n->op == T_NOT) {
            if (!is_any(t) && t->kind != T_TBOOL)
                terr(n->a, "`not` needs a bool, got %s", type_name(t));
            t = ty_bool;
        } else if (!is_any(t) && !is_numeric(t)) {
            terr(n->a, "unary `%s` needs a numeric, got %s",
                 tok_str(n->op), type_name(t));
        }
        break;
    case N_CMPCHAIN: {
        /* `a < b < c` is `a < b and b < c` (each operand once, short-
         * circuit). Same-direction chains only: `<` with `<=`, `>` with
         * `>=`, or all `==`; `!=` does not chain (ambiguous meaning).
         * Operands are int, bool, or fixed; split str/float chains
         * with `and`. */
        struct ex_type *prev = check_expr(n->a);
        int dir = 0;

        if (is_maybe(prev))
            n->flags |= NF_FALLIBLE;
        prev = strip_maybe(prev);
        prev = dec_default(n->a, prev);     /* chain literals are decimal */
        for (struct node *l = n->b; l; l = l->next) {
            struct ex_type *cur = check_expr(l->a);
            if (is_maybe(cur))
                n->flags |= NF_FALLIBLE;
            cur = strip_maybe(cur);
            cur = dec_default(l->a, cur);
            int d = (l->op == T_LT || l->op == T_LE) ? -1
                  : (l->op == T_GT || l->op == T_GE) ? 1
                  : (l->op == T_EQ) ? 2 : 0;

            if (d == 0)
                terr(l->a, "`!=` cannot chain; write the comparisons "
                           "separately with `and`");
            if (dir == 0)
                dir = d;
            else if (dir != d)
                terr(l->a, "a comparison chain must run in one "
                           "direction; split it with `and`");
            if (!assignable(prev, cur) && !assignable(cur, prev))
                terr(l->a, "cannot compare %s with %s",
                     type_name(prev), type_name(cur));
            prev = cur;
        }
        for (struct node *l = n->b; l; l = l->next) {
            struct ex_type *ot = strip_maybe(l->a->type);
            if (ot && !is_any(ot) && ot->kind != T_TINT &&
                ot->kind != T_TBOOL && ot->kind != T_TDEC)
                terr(l->a, "comparison chains take int, bool, or decimal "
                           "operands (got %s); split with `and`",
                     type_name(ot));
        }
        {
            struct ex_type *ft = strip_maybe(n->a->type);
            if (ft && !is_any(ft) && ft->kind != T_TINT &&
                ft->kind != T_TBOOL && ft->kind != T_TDEC)
                terr(n->a, "comparison chains take int, bool, or decimal "
                           "operands (got %s); split with `and`",
                     type_name(ft));
        }
        t = ty_bool;
        break;
    }
    case N_ISTEST:
        check_expr(n->a);
        t = ty_bool;
        break;
    case N_CAST:
        check_expr(n->a);
        if (n->type && !maybe_check_shape(n->a, n->type))
            resolve_dec_to(n->a, n->type);      /* 1.5 as float / as int */
        t = n->type ? n->type : ty_any;         /* explicit escape hatch */
        break;
    case N_RANGE:
        t = check_expr(n->a);
        check_expr(n->b);
        break;
    case N_CALL: {
        struct node *arg = n->b;
        int n_args = 0;
        const char *bn;

        /* A form a macro emitted was built, not parsed in a scope, so a name
         * it wrote carries no symbol: the resolver ran before the form
         * existed. Resolve a callee here, at the call site, which is the
         * scope the expansion landed in (typed-macros.md D2). A sibling
         * member wins over a module name, matching ordinary lookup, and a
         * builtin resolves to nothing and stays a bare name below. */
        if (n->a->kind == N_NAME && !n->a->sym) {
            struct sym *y = cur_class_sym
                          ? sym_member(cur_class_sym, n->a->name) : NULL;
            if (!y)
                y = module_sym(n->a->name);
            if (y && (y->kind == SYM_MACRO || y->kind == SYM_FUNC ||
                      y->kind == SYM_VERB))
                n->a->sym = y;
        }
        bn = (n->a->kind == N_NAME && !n->a->sym)
           ? n->a->name : NULL;                /* bare unresolved builtin */
        /* a macro call expands at compile time into core forms, then those
         * forms are checked in its place (meta.md, expand-then-typecheck) */
        if (n->a->kind == N_NAME && n->a->sym &&
            n->a->sym->kind == SYM_MACRO) {
            /* The cap belongs here, around expansion AND the re-check: a
             * macro that emits a call to itself recurses through check_expr,
             * not inside expand_macro, so counting only the latter let a
             * non-terminating macro run the C stack out instead of
             * reporting. */
            if (++meta_depth > 256)
                terr(n, "macro expansion too deep (a non-terminating macro?)");
            expand_macro(n, n->a->sym);
            t = check_expr(n);
            meta_depth--;
            break;
        }
        /* get(rec, "field"): a compile-time field accessor (the target a
         * field-walking macro emits); the field name is a string literal,
         * resolved to the record's field here and read in lowering */
        if (bn && ex_ci_eq(bn, "get") && arg && arg->next &&
            !arg->next->next) {
            struct ex_type *rt = strip_maybe(check_expr(arg));
            struct node *fn = arg->next;
            struct sym *rec = (rt && rt->kind == T_IDENT && rt->sym &&
                               rt->sym->kind == SYM_RECORD) ? rt->sym : NULL;
            struct sym *m;
            if (fn->kind != N_STR)
                terr(fn, "get(record, \"field\") needs a literal field name");
            if (!rec) {
                if (!is_any(rt))
                    terr(n, "get(...) needs a record, got %s", type_name(rt));
                t = ty_any;
                break;
            }
            m = sym_member(rec, fn->sval);
            if (!m)
                terr(fn, "`%s` has no field `%s`", type_name(rt), fn->sval);
            t = m->type ? m->type : ty_any;
            break;
        }
        /* spawn(ClassName): provisional instantiation builtin. Returns the
         * class type, so a send to the result resolves. The class name is a
         * qualifier, not a value. Any other spawn (a template ref) falls
         * through to the generic dynamic-call handling. */
        int spawn_class =
            bn && ex_ci_eq(bn, "spawn") &&
            arg && !arg->next && arg->kind == N_NAME && arg->sym &&
            arg->sym->kind == SYM_CLASS;

        for (struct node *a = n->b; a; a = a->next)
            n_args++;

        /* named arguments (`name: v`) name a record field or a function/verb
         * parameter; a builtin, `spawn`, or a send takes positional arguments */
        {
            int allows_named = n->a->kind == N_NAME && n->a->sym &&
                (n->a->sym->kind == SYM_RECORD ||
                 n->a->sym->kind == SYM_FUNC || n->a->sym->kind == SYM_VERB);
            if (!allows_named)
                for (struct node *a = n->b; a; a = a->next)
                    if (a->alias)
                        terr(a, "`%s:` names an argument, but this call takes "
                             "positional arguments", a->alias);
        }

        t = ty_any;
        if (bn && ex_ci_eq(bn, "log")) {
            /* retired with the output design (output.md) */
            terr(n, "there is no `log`; send `tell` to a player "
                    "(`player.tell(\"...\")`), or use `trace` for "
                    "your own eyes");
        }
        if (bn && ex_ci_eq(bn, "len")) {
            /* renamed to `length` (list-ops.md): a plain word reads better
             * for a new reader than an abbreviation */
            terr(n, "there is no `len`; the length builtin is `length`");
        }
        if (spawn_class) {
            t = named_type(arg->sym);
        } else if (bn && ex_ci_eq(bn, "length") && n_args == 1) {
            struct ex_type *xt = check_expr(arg);        /* str or list */
            if (!is_any(xt) && xt->kind != T_TSTR && xt->kind != T_TLIST)
                terr(arg, "length expects a str or list, got %s",
                     type_name(xt));
            t = ty_int;
        } else if (bn && ex_ci_eq(bn, "bytes") && n_args == 1) {
            /* text-encoding.md R7 D5: the raw byte size of a str or array,
             * beside len (the unit count). Returns int. */
            struct ex_type *xt = check_expr(arg);        /* str or list */
            if (!is_any(xt) && xt->kind != T_TSTR && xt->kind != T_TLIST)
                terr(arg, "bytes expects a str or list, got %s", type_name(xt));
            t = ty_int;
        } else if (bn && ex_ci_eq(bn, "append") && n_args == 2) {
            struct ex_type *lt = check_expr(arg);        /* list<T> */
            struct ex_type *vt = check_expr(arg->next);  /* T */
            if (!is_any(lt) && lt->kind != T_TLIST)
                terr(arg, "append expects a list, got %s", type_name(lt));
            if (lt && lt->kind == T_TLIST && lt->inner &&
                !assignable(lt->inner, vt))
                terr(arg->next, "append value is %s, list holds %s",
                     type_name(vt), type_name(lt->inner));
            t = lt;                                       /* a new list<T> */
        } else if (bn && ex_ci_eq(bn, "set") && n_args == 3) {
            struct ex_type *lt = check_expr(arg);        /* list<T> */
            struct ex_type *it = check_expr(arg->next);  /* index */
            struct ex_type *vt = check_expr(arg->next->next); /* T */
            if (!is_any(lt) && lt->kind != T_TLIST)
                terr(arg, "set expects a list, got %s", type_name(lt));
            if (!is_any(it) && it->kind != T_TINT)
                terr(arg->next, "set index must be int, got %s",
                     type_name(it));
            if (lt && lt->kind == T_TLIST && lt->inner &&
                !assignable(lt->inner, vt))
                terr(arg->next->next, "set value is %s, list holds %s",
                     type_name(vt), type_name(lt->inner));
            t = lt;                                       /* a new list<T> */
        } else if (bn && ex_ci_eq(bn, "delete") && n_args == 2) {
            struct ex_type *lt = check_expr(arg);        /* list<T> */
            struct ex_type *it = check_expr(arg->next);  /* index */
            if (!is_any(lt) && lt->kind != T_TLIST)
                terr(arg, "delete expects a list, got %s", type_name(lt));
            if (!is_any(it) && it->kind != T_TINT)
                terr(arg->next, "delete index must be int, got %s",
                     type_name(it));
            t = lt;                                       /* a new list<T> */
        } else if (bn && ex_ci_eq(bn, "prepend") && n_args == 2) {
            struct ex_type *lt = check_expr(arg);        /* list<T> */
            struct ex_type *vt = check_expr(arg->next);  /* T */
            if (!is_any(lt) && lt->kind != T_TLIST)
                terr(arg, "prepend expects a list, got %s", type_name(lt));
            if (lt && lt->kind == T_TLIST && lt->inner &&
                !assignable(lt->inner, vt))
                terr(arg->next, "prepend value is %s, list holds %s",
                     type_name(vt), type_name(lt->inner));
            t = lt;                                       /* a new list<T> */
        } else if (bn && ex_ci_eq(bn, "insert") && n_args == 3) {
            struct ex_type *lt = check_expr(arg);        /* list<T> */
            struct ex_type *it = check_expr(arg->next);  /* index */
            struct ex_type *vt = check_expr(arg->next->next); /* T */
            if (!is_any(lt) && lt->kind != T_TLIST)
                terr(arg, "insert expects a list, got %s", type_name(lt));
            if (!is_any(it) && it->kind != T_TINT)
                terr(arg->next, "insert index must be int, got %s",
                     type_name(it));
            if (lt && lt->kind == T_TLIST && lt->inner &&
                !assignable(lt->inner, vt))
                terr(arg->next->next, "insert value is %s, list holds %s",
                     type_name(vt), type_name(lt->inner));
            t = lt;                                       /* a new list<T> */
        } else if (bn && ex_ci_eq(bn, "reverse") && n_args == 1) {
            struct ex_type *lt = check_expr(arg);        /* list<T> */
            if (!is_any(lt) && lt->kind != T_TLIST)
                terr(arg, "reverse expects a list, got %s", type_name(lt));
            t = lt;                                       /* a new list<T> */
        } else if (bn && (ex_ci_eq(bn, "first") || ex_ci_eq(bn, "last")) &&
                   n_args == 1) {
            /* point access, fallible on the empty list (list-ops.md) */
            struct ex_type *lt = check_expr(arg);        /* list<T> -> T */
            if (!is_any(lt) && lt->kind != T_TLIST)
                terr(arg, "%s expects a list, got %s", bn, type_name(lt));
            n->flags |= NF_FALLIBLE;
            t = (lt && lt->kind == T_TLIST && lt->inner) ? lt->inner : ty_any;
        } else if (bn && ex_ci_eq(bn, "rest") && n_args == 1) {
            /* all but the first element; clamps to [] (list-ops.md) */
            struct ex_type *lt = check_expr(arg);        /* list<T> */
            if (!is_any(lt) && lt->kind != T_TLIST)
                terr(arg, "rest expects a list, got %s", type_name(lt));
            t = lt;
        } else if (bn && ex_ci_eq(bn, "find") && n_args == 2) {
            struct ex_type *st = strip_maybe(check_expr(arg));
            struct ex_type *ht = strip_maybe(check_expr(arg->next));
            if (!is_any(st) && st->kind != T_TSTR)
                terr(arg, "find expects a str to look for, got %s",
                     type_name(st));
            if (!is_any(ht) && ht->kind != T_TSTR)
                terr(arg->next, "find expects a str to search, got %s",
                     type_name(ht));
            t = maybe_of(ty_int);   /* 1-based position, or nothing */
        } else if (n->a->kind == N_NAME && n->a->sym &&
            n->a->sym->kind == SYM_RECORD) {
            /* record construction (records.md): named `Point(x: 3, y: 4)`
             * (order-independent) or positional `Point(3, 4)`, not mixed. A
             * field with a default may be omitted. */
            struct node *fields = n->a->sym->decl ? n->a->sym->decl->a : NULL;
            struct node *ar = n->b;
            int named = ar && ar->alias;
            if (named) {
                for (struct node *a = ar; a; a = a->next) {
                    struct node *f = NULL, *g;
                    if (!a->alias)
                        terr(a, "record fields are all named or all positional");
                    for (g = fields; g; g = g->next)
                        if (ex_ci_eq(g->name, a->alias)) { f = g; break; }
                    if (!f)
                        terr(a, "`%s` has no field `%s`", n->a->name, a->alias);
                    for (g = ar; g != a; g = g->next)
                        if (g->alias && ex_ci_eq(g->alias, a->alias))
                            terr(a, "field `%s` given twice", a->alias);
                    {
                        struct ex_type *at = check_expr(a);
                        if (f && f->type && !is_any(at) &&
                            !assignable(f->type, at))
                            terr(a, "field `%s` is %s, got %s", a->alias,
                                 type_name(f->type), type_name(at));
                    }
                }
                for (struct node *f = fields; f; f = f->next) {
                    int have = 0;
                    if (f->a)
                        continue;       /* has a default */
                    for (struct node *a = ar; a; a = a->next)
                        if (a->alias && ex_ci_eq(a->alias, f->name))
                            { have = 1; break; }
                    if (!have)
                        terr(n, "field `%s` of `%s` has no value and no "
                             "default", f->name, n->a->name);
                }
            } else {
                struct node *f = fields;
                for (; f && ar; f = f->next, ar = ar->next) {
                    struct ex_type *at = check_expr(ar);
                    if (f->type && !is_any(at) && !assignable(f->type, at))
                        terr(ar, "field `%s` is %s, got %s", f->name,
                             type_name(f->type), type_name(at));
                }
                if (ar)
                    terr(ar, "too many fields for `%s`", n->a->name);
                for (; f; f = f->next)
                    if (!f->a)
                        terr(n, "field `%s` of `%s` has no value and no "
                             "default", f->name, n->a->name);
            }
            t = named_type(n->a->sym);
        } else if (n->a->kind == N_NAME && n->a->sym &&
            (n->a->sym->kind == SYM_FUNC || n->a->sym->kind == SYM_VERB)) {
            check_args(n, n->a->sym);
            if (n->a->sym->decl &&
                (n->a->sym->decl->flags & NF_CANFAIL))
                t = ty_signal;      /* valueless signal, not a bool (R14) */
            else
                t = n->a->sym->type ? n->a->sym->type : ty_any;
        } else if (n->a->kind == N_FIELD_ACC) {
            /* One dot (verbs.md): recv.name(args) is a verb send when
             * name is a verb of the receiver's class, and always when
             * the receiver is a dynamic obj (cross-object field access
             * is not part of the actor model). A field holding a
             * callable stays a call-through-field. Verbs and fields
             * cannot collide in a class, so this is unambiguous. */
            struct node *fa = n->a;
            struct ex_type *rt = check_expr(fa->a);
            struct sym *m = NULL;

            /* record UFCS (records.md D4): `p.norm(args)` is `norm(p, args)`
             * when `norm` is not a field of the record but is a func in this
             * class. Field-first resolution: a field wins over the func. */
            if (rt && rt->kind == T_IDENT && rt->sym &&
                rt->sym->kind == SYM_RECORD && !sym_member(rt->sym, fa->name)) {
                struct sym *fn = cur_class_sym
                    ? sym_member(cur_class_sym, fa->name) : NULL;
                if (!fn || fn->kind != SYM_FUNC) {
                    terr(n, "`%s` is not a field of the record, and no func "
                         "`%s` is in scope to receive it", fa->name, fa->name);
                    t = ty_any;
                } else {
                    struct node *recv = fa->a;
                    recv->next = n->b;      /* prepend the receiver */
                    n->b = recv;
                    fa->kind = N_NAME;      /* the callee is now the func */
                    fa->a = NULL;
                    fa->sym = fn;
                    fa->name = fn->name;
                    check_args(n, fn);
                    t = fn->type ? fn->type : ty_any;
                }
                break;
            }

            if (rt && rt->kind == T_IDENT && rt->sym &&
                rt->sym->kind == SYM_CLASS)
                m = sym_member(rt->sym, fa->name);
            if ((m && m->kind == SYM_VERB) ||
                (!m && (is_any(rt) || rt->kind == T_TOBJ ||
                        slice_of(rt) != NULL))) {
                n->kind = N_SEND;       /* morph into a send in place */
                n->name = fa->name;
                n->a = fa->a;
                t = check_send(n);
            } else {
                check_expr(n->a);       /* call-through-field */
                for (struct node *a = n->b; a; a = a->next)
                    dec_default(a, check_expr(a));
            }
        } else {
            check_expr(n->a);
            for (struct node *a = n->b; a; a = a->next)
                dec_default(a, check_expr(a));
        }
        break;
    }
    case N_SEND:
        t = check_send(n);
        break;
    case N_SLICE:
        t = check_expr(n->a);
        check_expr(n->b);
        check_expr(n->c);
        if (is_any(t) || (t->kind != T_TLIST && t->kind != T_TSTR))
            t = ty_any;
        break;
    case N_WITH: {
        /* p with (x, y): a record field slice (record-slicing.md). The base
         * is a record; each named field must exist. The slice types as the
         * record itself, so an assignment or `=` against it is a normal
         * same-type check; the lowering restricts to the named fields. */
        struct ex_type *bt = check_expr(n->a);
        if (bt && bt->kind == T_IDENT && bt->sym &&
            bt->sym->kind == SYM_RECORD) {
            for (struct node *f = n->b; f; f = f->next)
                if (!sym_member(bt->sym, f->name))
                    terr(f, "`%s` has no field `%s`", type_name(bt), f->name);
            t = bt;
        } else {
            if (!is_any(bt))
                terr(n, "`with (...)` slices a record, got %s", type_name(bt));
            t = ty_any;
        }
        break;
    }
    case N_DATALIT: {
        /* A data literal used as a value: homogeneous constant elements
         * make a list<T> (T one of int, bool, fixed, str), so an
         * ascription mismatch is a normal type error instead of garbage
         * at runtime. A literal containing words or nesting is quoted
         * data (dialog trees, stat tables): it stays `any` so it can
         * flow to host sends; the prop world will type it for real
         * (TODO.md). Mixed constants and float elements are errors
         * here; the lowerer's checks remain only as a backstop. */
        struct ex_type *elem = NULL;
        int symbolic = 0;

        for (struct node *it = n->a; it; it = it->next)
            if (it->kind == N_NAME || it->kind == N_DATALIT)
                symbolic = 1;
        if (symbolic) {
            /* quoted data, not a plain value; but a ${} hole's
             * expression is code and still checks */
            for (struct node *it = n->a; it; it = it->next)
                if (it->kind == N_HOLE || it->kind == N_DATALIT)
                    check_expr(it->kind == N_HOLE ? it->a : it);
            t = ty_any;
            break;
        }
        for (struct node *it = n->a; it; it = it->next) {
            struct ex_type *et;

            switch (it->kind) {
            case N_NUM:   et = ty_int;   break;
            case N_BOOL:  et = ty_bool;  break;
            case N_STR:   et = ty_str;   break;
            case N_FLOAT:               /* a decimal element (numbers.md) */
                it->type = ty_declit;
                resolve_dec(it, ty_dec);
                et = ty_dec;
                break;
            case N_HOLE: {
                /* the hole's type joins the element type, canonicalized
                 * to the singleton so the pointer join below works */
                struct ex_type *ht = check_expr(it->a);
                ht = dec_default(it->a, ht);
                if (is_any(ht)) {
                    et = NULL;      /* unknown: joins with anything */
                    break;
                }
                switch (ht->kind) {
                case T_TINT:   et = ty_int;   break;
                case T_TBOOL:  et = ty_bool;  break;
                case T_TDEC:   et = ty_dec;   break;
                case T_TSTR:   et = ty_str;   break;
                case T_IDENT:               /* a record element (list<Point>) */
                    if (ht->sym && ht->sym->kind == SYM_RECORD) {
                        et = ht;
                        break;
                    }
                    terr(it->a, "a list element must be int, bool, decimal, "
                                "str, or a record; got %s", type_name(ht));
                case T_TFLOAT:
                    terr(it->a, "float elements in a list are not "
                                "supported");
                default:
                    terr(it->a, "a list element must be int, bool, decimal, "
                                "str, or a record; got %s", type_name(ht));
                }
                break;
            }
            default:
                terr(it, "unexpected element in a list literal");
            }
            if (!et)
                continue;
            if (!elem)
                elem = et;
            else if (!list_elem_same(elem, et))
                terr(it, "a list literal must be homogeneous; got %s "
                         "after %s (mixed data belongs in quoted "
                         "contexts)", type_name(et), type_name(elem));
        }
        t = elem ? list_of(elem) : ty_any;  /* []: type from context */
        break;
    }
    case N_QUOTE:
    default:
        t = ty_any;
        break;
    }
    /* choosers resolve their unresolved decimal branches against the
     * shared branch type (numbers.md) */
    if (n->kind == N_SELECT || n->kind == N_MATCHEXPR ||
        n->kind == N_THENELSE || n->kind == N_OTHERWISE)
        t = chooser_resolve_dec(n, t);
    /* a node marked fallible yields a maybe of its plain result: the
     * central half of Icon propagation (fallible.md) */
    if ((n->flags & NF_FALLIBLE) && !is_maybe(t) && !is_any(t))
        t = maybe_of(t);
    n->type = t;
    return t;
}

/****************************************************************
 * The meta layer: a compile-time macro interpreter (meta.md,
 * record-introspection.md). A `macro` call expands here into core forms.
 * The body is a tiny meta-language: `var`/assignment bind meta-values, a
 * `for` over `fieldsof(T)` unrolls per field, and `quote`/`quasi` build the
 * output forms with `${}` splices. The introspection primitives `typeof`,
 * `fieldsof`, and `fieldtype` are evaluated here, at compile time, and never
 * reach runtime.
 ****************************************************************/

/* The meta value domain (meta-values.md D2): atoms a macro computes with
 * (MV_NUM, MV_TEXT, MV_BOOL), forms it carries and emits (MV_AST), and the
 * descriptions the introspection primitives yield (MV_TYPE, MV_SEQ,
 * MV_FIELD). */
enum mv_kind { MV_AST, MV_TYPE, MV_SEQ, MV_FIELD, MV_TEXT, MV_BOOL, MV_NUM };

struct mval {
    int kind;
    int bval;                   /* MV_BOOL */
    long num;                   /* MV_NUM: a compile-time int (D5) */
    struct node *ast;           /* MV_AST: a built form; MV_FIELD: its tag list */
    struct ex_type *type;       /* MV_TYPE, and MV_AST's arg type, MV_FIELD.type */
    char *text;                 /* MV_TEXT, MV_FIELD.name */
    struct mval *items;         /* MV_SEQ: descriptor list, linked by ->next */
    struct mval *next;
};

struct menv {
    char *name;
    struct mval *val;
    struct menv *next;
};

static struct node *meta_call_site;  /* the call being expanded, for error() */

static struct mval *
mv_new(int kind)
{
    struct mval *v = arena_zalloc(ta, sizeof *v);
    v->kind = kind;
    return v;
}

static struct mval *
menv_lookup(struct menv *e, const char *name)
{
    for (; e; e = e->next)
        if (ex_ci_eq(e->name, name))
            return e->val;
    return NULL;
}

/* rebind an existing name or push a new one (a fresh cell, so an outer scope
 * a loop shadows is restored when the frame is dropped) */
static struct menv *
menv_bind(struct menv *e, char *name, struct mval *val)
{
    struct menv *c = arena_alloc(ta, sizeof *c);
    c->name = name;
    c->val = val;
    c->next = e;
    return c;
}

static void
menv_set(struct menv *e, const char *name, struct mval *val)
{
    for (; e; e = e->next)
        if (ex_ci_eq(e->name, name)) {
            e->val = val;
            return;
        }
}

/* deep-copy a resolved value form (a spliced ${arg}): keep sym/type so it
 * stays checked; copy so multiple splice sites do not share one node. The
 * `->next` chains are preserved (they carry a call's argument list); the
 * top-level operand's sibling is fixed up by the caller (form_build). */
static struct node *
val_copy(struct node *n)
{
    struct node *c;
    if (!n)
        return NULL;
    c = arena_alloc(ta, sizeof *c);
    *c = *n;
    c->a = val_copy(n->a);
    c->b = val_copy(n->b);
    c->c = val_copy(n->c);
    c->next = val_copy(n->next);
    return c;
}

static struct mval *
mv_bool(int b)
{
    struct mval *v = mv_new(MV_BOOL);
    v->bval = b ? 1 : 0;
    return v;
}

static struct mval *
mv_num(long n)
{
    struct mval *v = mv_new(MV_NUM);
    v->num = n;
    return v;
}

/* An atom is a value the macro computes with, a form is one it can only
 * carry and emit (meta-values.md D2). */
static int
mv_is_atom(struct mval *v)
{
    return v->kind == MV_NUM || v->kind == MV_TEXT || v->kind == MV_BOOL;
}

/* Compile-time type equality, the dispatch test a typed macro is built on
 * (typed-macros.md). Nominal, matching the base layer: two named types are
 * the same type when they are the same declaration, never when they merely
 * have the same shape. A `maybe T` is compared through, since a fallible
 * value is still a value of T for dispatch. */
static int
meta_type_eq(struct ex_type *a, struct ex_type *b)
{
    a = strip_maybe(a);
    b = strip_maybe(b);
    if (!a || !b)
        return 0;
    if (a->kind != b->kind)
        return 0;
    if (a->kind == T_IDENT)             /* record / class / enum: by decl */
        return a->sym && a->sym == b->sym;
    if (a->kind == T_TLIST || a->kind == T_TSET)
        return meta_type_eq(a->inner, b->inner);
    return 1;                           /* a builtin is its kind */
}

/* a tag atom (a symbol, string, or number) as compile-time text, so a macro
 * can compare it (`t.head = "json"`) or splice it (`t.rest.first`) */
static char *
atom_text(struct node *a)
{
    char buf[32];
    if (!a)
        return "";
    if (a->kind == N_NAME)
        return a->name;
    if (a->kind == N_STR)
        return a->sval;
    if (a->kind == N_NUM) {
        snprintf(buf, sizeof buf, "%ld", a->ival);
        return arena_strdup(ta, buf);
    }
    return "";
}

static struct mval *meta_eval(struct node *e, struct menv *env);

/* A node whose `->type` the author WROTE, rather than one the checker
 * inferred. form_build clears an inferred type so the spliced form re-checks
 * at the call site, but clearing a written one loses what the template said:
 * a quoted `x as float` silently became a cast to `any`, which assigns to
 * anything. Only the cast qualifies while `quote` takes an expression; a
 * declaration-quoting `quote` would add the declaring forms here. */
static int
node_type_is_declared(int kind)
{
    return kind == N_CAST;
}

/* Substitute a `${T}` type hole with the type it evaluates to
 * (typed-macros.md D4), so a template can be parametric over a type. Returns
 * the type unchanged when it holds no hole. */
static struct ex_type *
form_type(struct ex_type *t, struct menv *env)
{
    struct mval *v;

    if (!t)
        return NULL;
    if (t->kind != ET_TYPEHOLE) {
        if (t->inner) {                 /* list of ${T}, set of ${T} */
            struct ex_type *c = arena_alloc(ta, sizeof *c);
            *c = *t;
            c->inner = form_type(t->inner, env);
            return c;
        }
        return t;
    }
    v = meta_eval(t->hole, env);
    if (v->kind != MV_TYPE)
        terr(t->hole, "a `${}` in a type position needs a type");
    return v->type;
}

/* build a form from a quote/quasi template: copy the template, splicing each
 * `${meta}` hole with the evaluated value (an AST value as a sub-form, a text
 * value as a string literal). Template nodes get an inferred sym/type cleared
 * so the spliced result re-resolves and re-checks at the call site. */
static struct node *
form_build(struct node *f, struct menv *env)
{
    struct node *c;
    if (!f)
        return NULL;
    if (f->kind == N_HOLE) {
        struct mval *v = meta_eval(f->a, env);
        struct node *s;
        if (v->kind == MV_AST) {
            s = val_copy(v->ast);
        } else if (mv_is_atom(v)) {
            /* an atom splices as the literal it is (meta-values.md D4) */
            s = arena_zalloc(ta, sizeof *s);
            s->line = f->line;
            if (v->kind == MV_TEXT) {
                s->kind = N_STR;
                s->sval = v->text;
                s->slen = (int)strlen(v->text);
            } else if (v->kind == MV_NUM) {
                s->kind = N_NUM;
                s->ival = v->num;
            } else {
                s->kind = N_BOOL;
                s->ival = v->bval;
            }
        } else {
            terr(f, "a ${} splice needs a form or an atom (a number, text, "
                    "or bool), not a type or sequence");
        }
        s->next = form_build(f->next, env);   /* keep the sibling chain */
        return s;
    }
    c = arena_alloc(ta, sizeof *c);
    *c = *f;
    c->sym = NULL;
    c->type = node_type_is_declared(f->kind) ? form_type(f->type, env) : NULL;
    c->a = form_build(f->a, env);
    c->b = form_build(f->b, env);
    c->c = form_build(f->c, env);
    c->next = form_build(f->next, env);
    return c;
}

static const char *
mv_kind_name(struct mval *v)
{
    switch (v->kind) {
    case MV_NUM:  return "a number";
    case MV_TEXT: return "a symbol or string";
    case MV_BOOL: return "a true/false value";
    case MV_TYPE: return "a type";
    case MV_SEQ:  return "a sequence";
    case MV_FIELD: return "a field descriptor";
    default:      return "a form";
    }
}

/* Does a `match` label match the subject (typed-macros.md D3)? The label is a
 * meta expression, so a type label is just a type value and the compile-time
 * type dispatch falls out of the same machinery the atom subjects use. A
 * `lo to hi` range is numbers only, ordering being what a range needs. */
static int
meta_label_matches(struct node *lab, struct mval *subj, struct menv *env)
{
    struct mval *l;

    if (lab->kind == N_RANGE) {
        struct mval *lo = meta_eval(lab->a, env);
        struct mval *hi = meta_eval(lab->b, env);
        if (subj->kind != MV_NUM || lo->kind != MV_NUM || hi->kind != MV_NUM)
            terr(lab, "a meta `to` range needs numbers");
        return subj->num >= lo->num && subj->num <= hi->num;
    }
    l = meta_eval(lab, env);
    if (l->kind != subj->kind)
        terr(lab, "this label is %s but the subject is %s", mv_kind_name(l),
             mv_kind_name(subj));
    switch (subj->kind) {
    case MV_TYPE: return meta_type_eq(l->type, subj->type);
    case MV_NUM:  return l->num == subj->num;
    case MV_TEXT: return ex_ci_eq(l->text, subj->text);
    case MV_BOOL: return l->bval == subj->bval;
    default:
        terr(lab, "a meta `match` subject is a type or an atom, not %s",
             mv_kind_name(subj));
    }
}

/* the arm whose label list matches, or NULL for the `otherwise` / no-match
 * case, which the callers tell apart */
static struct node *
meta_match_arm(struct node *n, struct mval *subj, struct menv *env)
{
    for (struct node *arm = n->b; arm; arm = arm->next)
        for (struct node *lab = arm->a; lab; lab = lab->next)
            if (meta_label_matches(lab, subj, env))
                return arm;
    return NULL;
}

/* fieldsof(T): the record's fields in declaration order as descriptors */
static struct mval *
meta_fieldsof(struct node *at, struct ex_type *t)
{
    struct mval *seq = mv_new(MV_SEQ), *tail = NULL;
    struct sym *rec = (t && t->kind == T_IDENT && t->sym &&
                       t->sym->kind == SYM_RECORD) ? t->sym : NULL;
    struct node *f;
    if (!rec)
        terr(at, "fieldsof(...) needs a record type");
    for (f = rec->decl ? rec->decl->a : NULL; f; f = f->next) {
        struct mval *d = mv_new(MV_FIELD);
        d->text = f->name;
        d->type = f->type;
        d->ast = f->c;                  /* the field's tag list (or NULL) */
        if (tail)
            tail->next = d;
        else
            seq->items = d;
        tail = d;
    }
    return seq;
}

/* verbsof(T): a class's verbs in declaration order as descriptors
 * (record-introspection.md, the class sibling of fieldsof, for a dispatch-
 * table or proxy macro). A verb descriptor carries `.name`, `.returns` (the
 * return type), and `.params` (a sequence of name/type descriptors). Only the
 * class's own verbs; inherited verbs are deferred. */
static struct mval *
meta_verbsof(struct node *at, struct ex_type *t)
{
    struct mval *seq = mv_new(MV_SEQ), *tail = NULL;
    struct sym *cls = (t && t->kind == T_IDENT && t->sym &&
                       t->sym->kind == SYM_CLASS) ? t->sym : NULL;
    struct node *m;
    if (!cls)
        terr(at, "verbsof(...) needs a class type");
    for (m = cls->decl ? cls->decl->a : NULL; m; m = m->next) {
        struct mval *d, *ptail = NULL;
        struct node *p;
        if (m->kind != N_VERB)
            continue;
        d = mv_new(MV_FIELD);
        d->text = m->name;
        d->type = m->type;              /* return type (may be NULL) */
        for (p = m->a; p; p = p->next) {    /* param descriptors in ->items */
            struct mval *pd = mv_new(MV_FIELD);
            pd->text = p->name;
            pd->type = p->type;
            if (ptail)
                ptail->next = pd;
            else
                d->items = pd;
            ptail = pd;
        }
        if (tail)
            tail->next = d;
        else
            seq->items = d;
        tail = d;
    }
    return seq;
}

/* membersof(T): an enum's members in declaration order as descriptors
 * (record-introspection.md, the enum sibling of fieldsof, for an enum-driven
 * macro). A member descriptor carries `.name`; the members are a small closed
 * set (enums.md). */
static struct mval *
meta_membersof(struct node *at, struct ex_type *t)
{
    struct mval *seq = mv_new(MV_SEQ), *tail = NULL;
    struct sym *en = (t && t->kind == T_IDENT && t->sym &&
                      t->sym->kind == SYM_ENUM) ? t->sym : NULL;
    struct node *m;
    if (!en)
        terr(at, "membersof(...) needs an enum type");
    for (m = en->decl ? en->decl->a : NULL; m; m = m->next) {
        struct mval *d = mv_new(MV_FIELD);
        d->text = m->name;
        if (tail)
            tail->next = d;
        else
            seq->items = d;
        tail = d;
    }
    return seq;
}

static struct mval *
meta_eval(struct node *e, struct menv *env)
{
    if (!e)
        return mv_new(MV_AST);
    switch (e->kind) {
    case N_QUOTE:
    case N_QUASI: {
        struct mval *v = mv_new(MV_AST);
        v->ast = form_build(e->a, env);
        return v;
    }
    case N_NAME: {
        struct mval *v = menv_lookup(env, e->name);
        if (v)
            return v;
        /* a declared record, class, or enum names its type, stamped by the
         * resolver; a meta binding of the same name shadows it, having been
         * looked up first (typed-macros.md) */
        if (e->sym && (e->sym->kind == SYM_RECORD ||
                       e->sym->kind == SYM_CLASS || e->sym->kind == SYM_ENUM)) {
            v = mv_new(MV_TYPE);
            v->type = named_type(e->sym);
            return v;
        }
        terr(e, "`%s` is not a meta value in scope", e->name);
    }
    case N_STR: {
        struct mval *v = mv_new(MV_TEXT);
        v->text = e->sval;
        return v;
    }
    case N_BOOL:
        return mv_bool(e->ival);
    case N_NUM:                         /* an atom, like text and bool (D1) */
        return mv_num(e->ival);
    case N_TYPELIT: {                   /* a builtin type names itself */
        struct mval *v = mv_new(MV_TYPE);
        v->type = e->type;
        return v;
    }
    case N_MATCHEXPR: {                 /* the same dispatch, yielding a value */
        struct mval *subj = meta_eval(e->a, env);
        struct node *arm = meta_match_arm(e, subj, env);

        if (arm)
            return meta_eval(arm->b, env);
        if (!e->c)
            terr(e, "no arm of this meta `match` matches, and there is no "
                    "`otherwise`");
        return meta_eval(e->c, env);
    }
    case N_UNOP:
        if (e->op == T_NOT) {
            struct mval *v = meta_eval(e->a, env);
            if (v->kind != MV_BOOL)
                terr(e, "meta `not` needs a true/false value");
            return mv_bool(!v->bval);
        }
        terr(e, "`%s` is not a meta-layer operator", tok_str(e->op));
    case N_BINOP: {
        struct mval *l = meta_eval(e->a, env);
        struct mval *r = meta_eval(e->b, env);

        /* arithmetic and ordering over numbers, and `+` over text
         * (meta-values.md D5); the range checks are compile errors, since a
         * macro runs before there is a runtime for a fault to land in */
        if (l->kind == MV_NUM && r->kind == MV_NUM) {
            long a = l->num, b = r->num;
            switch (e->op) {
            case T_PLUS: case T_MINUS: case T_STAR: {
                long v = e->op == T_PLUS ? a + b :
                         e->op == T_MINUS ? a - b : a * b;
                if (v < -2147483647L - 1 || v > 2147483647L)
                    terr(e, "this compile-time arithmetic overflows an int");
                return mv_num(v);
            }
            case T_SLASH:
            case T_PERCENT:
                if (b == 0)
                    terr(e, "compile-time division by zero");
                return mv_num(e->op == T_SLASH ? a / b : a % b);
            case T_LT:  return mv_bool(a < b);
            case T_LE:  return mv_bool(a <= b);
            case T_GT:  return mv_bool(a > b);
            case T_GE:  return mv_bool(a >= b);
            case T_EQ:  return mv_bool(a == b);
            case T_NE:  return mv_bool(a != b);
            default: break;
            }
        }
        if (e->op == T_PLUS && l->kind == MV_TEXT && r->kind == MV_TEXT) {
            struct mval *v = mv_new(MV_TEXT);   /* a generated name */
            size_t ll = strlen(l->text), rl = strlen(r->text);
            char *s = arena_alloc(ta, ll + rl + 1);
            memcpy(s, l->text, ll);
            memcpy(s + ll, r->text, rl + 1);
            v->text = s;
            return v;
        }
        switch (e->op) {
        case T_EQ:
        case T_NE: {
            int eq;
            if (l->kind == MV_TYPE && r->kind == MV_TYPE) {
                eq = meta_type_eq(l->type, r->type);
                return mv_bool(e->op == T_EQ ? eq : !eq);
            }
            if (l->kind == MV_BOOL && r->kind == MV_BOOL) {
                eq = (l->bval == r->bval);
                return mv_bool(e->op == T_EQ ? eq : !eq);
            }
            if (l->kind != MV_TEXT || r->kind != MV_TEXT)
                terr(e, "a meta `%s` compares two atoms of the same kind "
                        "(two numbers, two symbols or strings, or two bools) "
                        "or two types", e->op == T_EQ ? "=" : "<>");
            eq = ex_ci_eq(l->text, r->text);
            return mv_bool(e->op == T_EQ ? eq : !eq);
        }
        case T_AND:
            return mv_bool(l->kind == MV_BOOL && l->bval &&
                           r->kind == MV_BOOL && r->bval);
        case T_OR:
            return mv_bool((l->kind == MV_BOOL && l->bval) ||
                           (r->kind == MV_BOOL && r->bval));
        case T_IN: {
            /* `text in seq`: is a symbol/string present in a sequence */
            struct mval *it;
            if (l->kind != MV_TEXT || r->kind != MV_SEQ)
                terr(e, "meta `in` tests a symbol against a sequence");
            for (it = r->items; it; it = it->next)
                if (it->kind == MV_TEXT && ex_ci_eq(it->text, l->text))
                    return mv_bool(1);
            return mv_bool(0);
        }
        default:
            /* an operator that exists but not across these kinds reads very
             * differently from one that does not exist at all */
            if (mv_is_atom(l) && mv_is_atom(r))
                terr(e, "a meta `%s` needs two atoms of the same kind (two "
                        "numbers, or two texts for `+`)", tok_str(e->op));
            terr(e, "`%s` is not a meta-layer operator", tok_str(e->op));
        }
    }
    case N_FIELD_ACC: {
        struct mval *b = meta_eval(e->a, env);
        if (b->kind == MV_AST && b->ast && b->ast->kind == N_TAG) {
            /* a tag form: `.head` is its leading symbol, `.rest` the atoms
             * after it (record-annotations.md), both as text */
            struct node *atoms = b->ast->a;
            if (ex_ci_eq(e->name, "head")) {
                struct mval *v = mv_new(MV_TEXT);
                v->text = atom_text(atoms);
                return v;
            }
            if (ex_ci_eq(e->name, "rest")) {
                struct mval *seq = mv_new(MV_SEQ), *tail = NULL;
                struct node *a;
                for (a = atoms ? atoms->next : NULL; a; a = a->next) {
                    struct mval *tv = mv_new(MV_TEXT);
                    tv->text = atom_text(a);
                    if (tail)
                        tail->next = tv;
                    else
                        seq->items = tv;
                    tail = tv;
                }
                return seq;
            }
            terr(e, "a tag has `.head` and `.rest`, not `.%s`", e->name);
        }
        if (b->kind == MV_SEQ) {
            /* `.first` picks the first item; `.count` is the length */
            if (ex_ci_eq(e->name, "first")) {
                if (!b->items)
                    terr(e, "`.first` on an empty sequence");
                return b->items;
            }
            terr(e, "a sequence has `.first`, not `.%s`", e->name);
        }
        if (b->kind != MV_FIELD)
            terr(e, "`.%s` reads a field descriptor or a tag", e->name);
        if (ex_ci_eq(e->name, "name")) {
            struct mval *v = mv_new(MV_TEXT);
            v->text = b->text;
            return v;
        }
        if (ex_ci_eq(e->name, "type") || ex_ci_eq(e->name, "returns")) {
            /* a field's type, or a verb descriptor's return type (verbsof) */
            struct mval *v = mv_new(MV_TYPE);
            v->type = b->type;
            return v;
        }
        if (ex_ci_eq(e->name, "params")) {
            /* a verb descriptor's parameters (verbsof), each a name/type
             * descriptor, held in ->items */
            struct mval *seq = mv_new(MV_SEQ);
            seq->items = b->items;
            return seq;
        }
        if (ex_ci_eq(e->name, "tags")) {
            /* the field's tag list as a compile-time sequence of tag forms
             * (record-annotations.md D5); each item is an N_TAG form a macro
             * inspects (head/rest access waits on the interpreter increment) */
            struct mval *seq = mv_new(MV_SEQ), *tail = NULL;
            struct node *t;
            for (t = b->ast; t; t = t->next) {
                struct mval *tv = mv_new(MV_AST);
                tv->ast = t;
                if (tail)
                    tail->next = tv;
                else
                    seq->items = tv;
                tail = tv;
            }
            return seq;
        }
        terr(e, "a descriptor has `.name`, `.type`/`.returns`, `.params`, "
                "and `.tags`, not `.%s`", e->name);
    }
    case N_CALL: {
        const char *cn = e->a->kind == N_NAME ? e->a->name : NULL;
        struct node *a0 = e->b;
        if (cn && ex_ci_eq(cn, "error") && a0 && !a0->next) {
            /* the quality lever of the no-bounds model (typed-macros.md D6):
             * a macro rejects an argument in its own words, instead of
             * letting a raw post-expansion type error surface */
            struct mval *m = meta_eval(a0, env);
            if (m->kind != MV_TEXT)
                terr(e, "error(...) takes a message");
            /* report at the call, not in the macro body: the author who
             * needs the message is the one who wrote the argument */
            terr(meta_call_site ? meta_call_site : e, "%s", m->text);
        }
        if (cn && ex_ci_eq(cn, "typeof") && a0 && !a0->next) {
            struct mval *av = meta_eval(a0, env);
            struct mval *v = mv_new(MV_TYPE);
            /* a form yields its static type; an atom yields the type that
             * literal would have, so the bridge is total over the value
             * domain (meta-values.md D3) */
            if (av->kind == MV_AST)
                v->type = av->type ? av->type : ty_any;
            else if (av->kind == MV_NUM)
                v->type = ty_int;
            else if (av->kind == MV_TEXT)
                v->type = ty_str;
            else if (av->kind == MV_BOOL)
                v->type = ty_bool;
            else
                terr(e, "typeof(...) needs a value");
            return v;
        }
        if (cn && ex_ci_eq(cn, "fieldsof") && a0 && !a0->next) {
            struct mval *tv = meta_eval(a0, env);
            if (tv->kind != MV_TYPE)
                terr(e, "fieldsof(...) needs a type, not a value; a "
                        "value's type is `typeof(v)`");
            return meta_fieldsof(e, tv->type);
        }
        if (cn && ex_ci_eq(cn, "verbsof") && a0 && !a0->next) {
            struct mval *tv = meta_eval(a0, env);
            if (tv->kind != MV_TYPE)
                terr(e, "verbsof(...) needs a type, not a value; a "
                        "value's type is `typeof(v)`");
            return meta_verbsof(e, tv->type);
        }
        if (cn && ex_ci_eq(cn, "membersof") && a0 && !a0->next) {
            struct mval *tv = meta_eval(a0, env);
            if (tv->kind != MV_TYPE)
                terr(e, "membersof(...) needs a type, not a value; a "
                        "value's type is `typeof(v)`");
            return meta_membersof(e, tv->type);
        }
        if (cn && ex_ci_eq(cn, "fieldtype") && a0 && a0->next &&
            !a0->next->next) {
            /* fieldtype(T, "name"): the field's type, or a hard error when
             * absent (the fallible-consumer integration is deferred) */
            struct mval *tv = meta_eval(a0, env);
            struct mval *nv = meta_eval(a0->next, env);
            struct sym *rec, *m;
            struct mval *v;
            if (tv->kind != MV_TYPE || nv->kind != MV_TEXT)
                terr(e, "fieldtype(type, \"name\") takes a type and a "
                        "name; a value's type is `typeof(v)`");
            rec = (tv->type && tv->type->kind == T_IDENT && tv->type->sym &&
                   tv->type->sym->kind == SYM_RECORD) ? tv->type->sym : NULL;
            if (!rec)
                terr(e, "fieldtype(...) needs a record type");
            m = sym_member(rec, nv->text);
            if (!m)
                terr(e, "`%s` has no field `%s`", type_name(tv->type),
                     nv->text);
            v = mv_new(MV_TYPE);
            v->type = m->type ? m->type : ty_any;
            return v;
        }
        terr(e, "`%s` is not a meta-layer operation",
             cn ? cn : "this call");
    }
    default:
        /* name what the layer holds, and point at `quote` for the author who
         * meant a piece of program rather than a value (meta-values.md D7) */
        terr(e, "a macro body computes with numbers, text, bools, types, and "
                "sequences; for a piece of program, write `quote ...`");
    }
    return mv_new(MV_AST);      /* unreachable (terr does not return) */
}

/* run one macro-body statement; the last expression statement is the result */
static void
meta_exec(struct node *s, struct menv **env, struct mval **result)
{
    switch (s->kind) {
    case N_VAR:
        *env = menv_bind(*env, s->name, meta_eval(s->a, *env));
        break;
    case N_ASSIGN:
        if (s->a->kind != N_NAME)
            terr(s, "a meta assignment targets a name");
        menv_set(*env, s->a->name, meta_eval(s->b, *env));
        break;
    case N_IF: {
        struct mval *c;
        if (s->name)
            terr(s, "`if var` is not valid in a macro body");
        c = meta_eval(s->a, *env);
        if (c->kind != MV_BOOL)
            terr(s->a, "a meta `if` needs a true/false condition");
        if (c->bval) {
            for (struct node *b = s->b; b; b = b->next)
                meta_exec(b, env, result);
        } else if (s->c) {
            if (s->c->kind == N_IF)         /* elseif chain */
                meta_exec(s->c, env, result);
            else                            /* else block: a statement list */
                for (struct node *b = s->c; b; b = b->next)
                    meta_exec(b, env, result);
        }
        break;
    }
    case N_MATCH: {
        /* compile-time multi-way dispatch: one arm runs, the others are not
         * even built (typed-macros.md D3) */
        struct mval *subj = meta_eval(s->a, *env);
        struct node *arm = meta_match_arm(s, subj, *env);
        struct node *body = arm ? arm->b : s->c;

        if (!arm && !s->c)
            terr(s, "no arm of this meta `match` matches, and there is no "
                    "`otherwise`");
        for (struct node *b = body; b; b = b->next)
            meta_exec(b, env, result);
        break;
    }
    case N_FOR: {
        struct mval *seq;
        struct mval *it;
        /* the range check comes first: `for i in 1 to 3` would otherwise
         * die evaluating `1` as a meta value, reporting the generic "not
         * valid in meta-layer code" and never reaching this message */
        if (s->b)
            terr(s, "a meta `for` iterates a field sequence, not a range");
        seq = meta_eval(s->a, *env);
        if (seq->kind != MV_SEQ)
            terr(s, "a meta `for` iterates `fieldsof(T)`");
        for (it = seq->items; it; it = it->next) {
            struct menv *frame = menv_bind(*env, s->name, it);
            for (struct node *b = s->c; b; b = b->next)
                meta_exec(b, &frame, result);
        }
        break;
    }
    case N_EXPR_STMT: {
        struct mval *v = meta_eval(s->a, *env);
        if (v->kind != MV_AST)
            terr(s, "a macro must yield a form");
        *result = v;
        break;
    }
    default:
        terr(s, "this statement is not valid in a macro body");
    }
}

/* expand a macro call in place: bind the auto-quoted args, run the body, and
 * overwrite the call node with the resulting form (checked by the caller) */
static void
expand_macro(struct node *call, struct sym *mac)
{
    struct menv *env = NULL;
    struct mval *result = NULL;
    struct node *p = mac->decl ? mac->decl->a : NULL;
    struct node *arg = call->b;
    struct node *outer_site = meta_call_site;

    meta_call_site = call;              /* where a macro's own error lands */
    if (++meta_depth > 256)
        terr(call, "macro expansion too deep (a non-terminating macro?)");
    for (; p && arg; p = p->next, arg = arg->next) {
        struct mval *v;
        struct sym *ts = arg->kind == N_NAME ? arg->sym : NULL;

        /* a type argument binds as a type, not as a form: a macro
         * parametric over a type takes the type itself (typed-macros.md D4) */
        if (arg->kind == N_TYPELIT) {
            v = mv_new(MV_TYPE);
            v->type = arg->type;
        } else if (ts && (ts->kind == SYM_RECORD || ts->kind == SYM_CLASS ||
                          ts->kind == SYM_ENUM)) {
            v = mv_new(MV_TYPE);
            v->type = named_type(ts);
        } else {
            v = mv_new(MV_AST);
            v->ast = arg;                   /* the auto-quoted argument form */
            /* a macro argument is a boundary, so an untyped decimal constant
             * takes its default there (numbers.md); otherwise a macro asking
             * whether `9.5` is a decimal would be told no */
            v->type = dec_default(arg, check_expr(arg));
        }
        env = menv_bind(env, p->name, v);
    }
    if (p || arg)
        terr(call, "macro `%s` expects a different number of arguments",
             mac->name);
    for (struct node *s = mac->decl->b; s; s = s->next)
        meta_exec(s, &env, &result);
    if (!result)
        terr(call, "macro `%s` produced no form", mac->name);
    *call = *result->ast;                   /* become the expansion */
    meta_depth--;
    meta_call_site = outer_site;
}

/****************************************************************
 * Statement checking
 ****************************************************************/

static void check_stmts(struct node *list, struct sym *ret);

static void
want_bool(struct node *cond, const char *ctx)
{
    struct ex_type *t = check_expr(cond);
    /* `if` and `while` test bools; failure has its own consumers
     * (fallible-consumers.md). A fallible condition is rejected so a
     * failure can never ride the boolean construct and collapse with
     * `false`. */
    if (t && t->kind == ET_SIGNAL)
        terr(cond, "a `can fail` action has no bool to test; handle its "
                   "failure inline with `on fail`, or with a bare call, "
                   "not `%s`", ctx);
    if (is_maybe(t))
        terr(cond, "a fallible value cannot be an `%s` condition; bind it "
                   "with `%s var x = ...` or supply a fallback with `otherwise`",
             ctx, ctx);
    if (!is_any(t) && t->kind != T_TBOOL)
        terr(cond, "%s condition must be bool, got %s", ctx, type_name(t));
}

/* element type produced by iterating `expr` in a for loop */
static struct ex_type *
iter_element(struct node *n)
{
    struct ex_type *t;

    if (n->b) {                     /* range: lo to hi */
        struct ex_type *lo = strip_maybe(check_expr(n->a));
        struct ex_type *hi = strip_maybe(check_expr(n->b));
        if (!is_numeric(lo) && !is_any(lo))
            terr(n->a, "range bound must be numeric, got %s", type_name(lo));
        if (!is_numeric(hi) && !is_any(hi))
            terr(n->b, "range bound must be numeric, got %s", type_name(hi));
        return is_any(lo) ? ty_int : lo;
    }
    t = strip_maybe(check_expr(n->a));
    if (is_any(t))
        return ty_any;
    if (t->kind == T_TLIST)
        return t->inner ? t->inner : ty_any;
    if (t->kind == T_TSTR)
        return ty_str;
    terr(n->a, "cannot iterate over %s", type_name(t));
}

static void
check_stmt(struct node *n, struct sym *ret)
{
    if (!n)
        return;

    switch (n->kind) {
    case N_VAR: {
        struct ex_type *it;
        struct sym *sen = n->type ? set_enum_sym(n->type) : NULL;
        if (sen && n->a) {          /* var s is set of E = <members> */
            check_set(n->a, sen, n->type);
            break;
        }
        it = n->a ? check_expr(n->a) : NULL;
        no_signal(n->a, it);        /* `var ok = validate(k)` captures nothing */
        if (n->type) {
            absence_mixup(n->a, n->type);
            if (it && !assignable(n->type, it))
                terr(n, "cannot initialize `%s` (%s) from %s",
                     n->name, type_name(n->type), type_name(it));
            if (it)
                widen_to(n->a, n->type);
        } else if (it) {
            /* inference never captures: a fallible initializer infers
             * the payload type (trap or propagate on failure); storing
             * the outcome is the explicit `is maybe T` spelling
             * (fallible.md). An unresolved decimal defaults to decimal
             * (numbers.md). */
            it = dec_default(n->a, it);
            n->type = is_any(it) ? ty_any : strip_maybe(it);    /* infer */
            if (n->sym)
                n->sym->type = n->type;
        } else {
            terr(n, "`%s` needs a type or an initializer", n->name);
        }
        break;
    }
    case N_ASSIGN: {
        struct ex_type *lt = check_expr(n->a);
        struct ex_type *rt;
        struct sym *sen = set_enum_sym(lt);
        if (sen) {                  /* s = <members> : a set assignment */
            check_set(n->b, sen, lt);
            break;
        }
        rt = check_expr(n->b);
        no_signal(n->b, rt);
        absence_mixup(n->b, lt);
        if (!assignable(lt, rt))
            terr(n, "cannot assign %s to %s",
                 type_name(rt), type_name(lt));
        widen_to(n->b, lt);
        break;
    }
    case N_IF:
        if (n->name) {              /* if var i = fallible-expr */
            struct ex_type *et = check_expr(n->a);
            if (et && et->kind == ET_SIGNAL)
                terr(n->a, "a `can fail` call has no value to bind; handle "
                           "it with `%s(...) on fail ...` or a bare call",
                     n->a->a->name);
            if (!is_any(et) && !is_maybe(et))
                terr(n->a, "`if var` needs an expression that can fail; "
                           "this one cannot");
            if (n->sym)
                n->sym->type = strip_maybe(et);
        } else {
            want_bool(n->a, "if");
        }
        check_stmts(n->b, ret);
        if (n->c) {
            if (n->c->kind == N_IF)
                check_stmt(n->c, ret);
            else
                check_stmts(n->c, ret);
        }
        break;
    case N_WHILE:
        if (n->name) {              /* while var i = fallible-expr */
            struct ex_type *et = check_expr(n->a);
            if (et && et->kind == ET_SIGNAL)
                terr(n->a, "a `can fail` call has no value to bind; handle "
                           "it with `%s(...) on fail ...` or a bare call",
                     n->a->a->name);
            if (!is_any(et) && !is_maybe(et))
                terr(n->a, "`while var` needs an expression that can "
                           "fail; this one cannot");
            if (n->sym)
                n->sym->type = strip_maybe(et);
        } else {
            want_bool(n->a, "while");
        }
        check_stmts(n->b, ret);
        break;
    case N_FOR: {
        struct ex_type *elem = iter_element(n);
        if (n->sym)
            n->sym->type = elem;
        check_stmts(n->c, ret);
        break;
    }
    case N_MATCH: {
        struct ex_type *subj = check_match_subject(n);
        for (struct node *arm = n->b; arm; arm = arm->next) {
            check_match_arm_labels(arm, subj);
            check_stmts(arm->b, ret);
        }
        if (n->c)
            check_stmts(n->c, ret);
        check_enum_exhaustive(n, subj, n->b, n->c);
        break;
    }
    case N_RETURN:
        if (n->a) {
            struct ex_type *rt = check_expr(n->a);
            if (!ret)
                terr(n, "this verb has no return type; `return` takes no value");
            no_signal(n->a, rt);
            absence_mixup(n->a, ret->type);
            if (!assignable(ret->type, rt))
                terr(n, "returns %s, expected %s",
                     type_name(rt), type_name(ret->type));
            widen_to(n->a, ret->type);
        } else if (ret) {
            terr(n, "must return a %s", type_name(ret->type));
        }
        break;
    case N_FAIL:
        if (!cur_mem ||
            !((cur_mem->flags & NF_CANFAIL) || is_maybe(cur_mem->type)))
            terr(n, "`fail` is only legal inside a `returns maybe` or "
                    "`can fail` func");
        break;
    case N_TRACE: {
        /* trace routes by static type to the author channel (output.md) */
        struct ex_type *tt = strip_maybe(check_expr(n->a));
        tt = dec_default(n->a, tt);
        if (!is_any(tt) && tt->kind != T_TSTR && tt->kind != T_TINT &&
            tt->kind != T_TFLOAT && tt->kind != T_TDEC &&
            tt->kind != T_TBOOL)
            terr(n->a, "trace takes a str, int, float, decimal, or bool; "
                       "got %s", type_name(tt));
        break;
    }
    case N_EXPR_STMT:
        check_expr(n->a);
        break;
    case N_ONFAIL: {
        /* `stmt on fail handler` (fallible-consumers.md): the left must
         * carry a fallible producer, or the handler is dead. The handler
         * consumes that failure and is any statement. */
        struct node *p = n->a->kind == N_ASSIGN ? n->a->b : n->a->a;
        struct ex_type *pt;
        check_stmt(n->a, ret);
        pt = p ? p->type : NULL;
        if (!pt || (!is_any(pt) && !is_maybe(pt) && pt->kind != ET_SIGNAL))
            terr(n, "`on fail` needs an action that can fail; this one "
                    "cannot. Drop `on fail`, or use a `can fail` or "
                    "fallible action");
        check_stmt(n->b, ret);
        break;
    }
    case N_BREAK:
    case N_CONTINUE:
    case N_TRACE_CMT:
        break;
    default:
        check_expr(n);
        break;
    }
}

static void
check_stmts(struct node *list, struct sym *ret)
{
    for (struct node *n = list; n; n = n->next)
        check_stmt(n, ret);
}

/****************************************************************
 * Program checking
 ****************************************************************/

/* a compile-time constant usable as a parameter default: a literal. General
 * const-expression folding is the deferred field-default-folding work. */
static int
is_const_literal(struct node *e)
{
    return e && (e->kind == N_NUM || e->kind == N_FLOAT ||
                 e->kind == N_STR || e->kind == N_BOOL);
}

static void
check_class(struct node *cls)
{
    cur_class_sym = cls->sym;           /* for record UFCS (p.norm() -> norm(p)) */

    /* A class that names an interface is checked against it here, at the
     * class (interface-support.md D3), so drift is reported where it happens
     * rather than wherever a value of the class later meets the interface. */
    for (struct node *m = cls->a; m; m = m->next) {
        if (m->kind != N_SUPPORTS || !m->sym || !m->sym->type)
            continue;
        for (struct node *v = m->sym->type->verbs; v; v = v->next) {
            struct sym *have = sym_member(cls->sym, v->name);
            if (!have || have->kind != SYM_VERB)
                terr(m, "`%s` supports `%s`, which declares a verb `%s`, "
                        "and this class has none", cls->name, m->name,
                     v->name);
            if (!sig_matches(v, have))
                terr(have->decl ? have->decl : m,
                     "`%s` does not match the signature `%s` declares for "
                     "it", v->name, m->name);
        }
    }

    for (struct node *m = cls->a; m; m = m->next) {
        switch (m->kind) {
        case N_FIELD:
            if (m->a) {
                struct ex_type *dt = check_expr(m->a);
                no_signal(m->a, dt);
                absence_mixup(m->a, m->type);
                if (is_maybe(m->type) && m->a->kind != N_NOTHING)
                    terr(m, "a maybe field defaults to `nothing` only "
                            "(for now)");
                if (!assignable(m->type, dt))
                    terr(m, "field `%s` default is %s, expected %s",
                         m->name, type_name(dt), type_name(m->type));
                widen_to(m->a, m->type);
            }
            break;
        case N_VERB:
        case N_FUNC:
            /* `shared` is func-only (shared-params.md D5): a verb crosses the
             * actor boundary via a send, where a by-reference place is
             * meaningless, so the reference never leaves the actor */
            if (m->kind == N_VERB)
                for (struct node *p = m->a; p; p = p->next)
                    if (p->flags & NF_SHARED)
                        terr(p, "`shared` is not allowed on a verb parameter "
                                "(a send crosses the actor boundary); use a "
                                "func, or return the value");
            /* a field-restricted view `p is R with (x, y)` needs a record
             * type, and each masked field must exist (record-slicing.md D4) */
            for (struct node *p = m->a; p; p = p->next)
                if (p->c) {
                    struct sym *rec = (p->type && p->type->kind == T_IDENT &&
                        p->type->sym && p->type->sym->kind == SYM_RECORD)
                        ? p->type->sym : NULL;
                    if (!rec)
                        terr(p, "`with (...)` restricts a record parameter's "
                                "fields, but `%s` is not a record",
                             type_name(p->type));
                    for (struct node *f = p->c; f; f = f->next)
                        if (!sym_member(rec, f->name))
                            terr(f, "`%s` has no field `%s`",
                                 type_name(p->type), f->name);
                }
            /* a parameter default must be a compile-time constant, so it is
             * consistent and visible in the .exi interface (records.md) */
            for (struct node *p = m->a; p; p = p->next)
                if (p->a) {
                    struct ex_type *dt = check_expr(p->a);
                    if (!is_const_literal(p->a))
                        terr(p, "parameter `%s` default must be a compile-time "
                             "constant (a literal, for now)", p->name);
                    if (p->type && !is_any(dt) && !assignable(p->type, dt))
                        terr(p, "parameter `%s` default is %s, expected %s",
                             p->name, type_name(dt), type_name(p->type));
                    widen_to(p->a, p->type);
                }
            /* m->sym carries the declared return type in ->type */
            cur_mem = m;
            check_stmts(m->b, m->type ? m->sym : NULL);
            cur_mem = NULL;
            break;
        default:
            break;
        }
    }
}

void
typecheck_program(struct arena *a, struct node *file)
{
    ta = a;
    tfile = lex_filename();
    cur_file = file;

    ty_any = mk_ty(ET_ANY);
    ty_nothing = maybe_of(ty_any);
    ty_nil = mk_ty(ET_NIL);
    ty_int = mk_ty(T_TINT);
    ty_float = mk_ty(T_TFLOAT);
    ty_bool = mk_ty(T_TBOOL);
    ty_str = mk_ty(T_TSTR);
    ty_obj = mk_ty(T_TOBJ);
    ty_err = mk_ty(T_TERR);
    ty_prop = mk_ty(T_TPROP);
    ty_dec = mk_ty(T_TDEC);
    ty_declit = mk_ty(ET_DEC);
    ty_signal = mk_ty(ET_SIGNAL);
    (void)ty_obj;
    (void)ty_err;
    (void)ty_prop;

    for (struct node *it = file->a; it; it = it->next) {
        switch (it->kind) {
        case N_CONST:
            if (it->a) {
                struct ex_type *vt = check_expr(it->a);
                no_signal(it->a, vt);
                if (it->type) {
                    absence_mixup(it->a, it->type);
                    if (!assignable(it->type, vt))
                        terr(it, "const `%s` is %s, expected %s",
                             it->name, type_name(vt), type_name(it->type));
                    widen_to(it->a, it->type);
                } else {
                    vt = dec_default(it->a, vt);
                    it->type = is_any(vt) ? ty_any : vt;
                    if (it->sym)
                        it->sym->type = it->type;
                }
            }
            break;
        case N_CLASS:
            check_class(it);
            break;
        default:
            break;
        }
    }
}
