/* parse.c : the BASIC parser (doc/languages/basic.md).

   Phase 1 bring-up: it accepts module-scope statements, `DIM name AS INT`,
   assignment, `PRINT`, and `END`, over the arithmetic core of the expression
   grammar (`+ - * DIV MOD`, unary sign, parentheses, integer literals, and
   scalar variables). The full grammar, control flow, PROC/FN, FLOAT/STRING
   variables, and the aggregates, lands in later 0.48.0 phases. The scanner is
   pull-based (lexer_next); the parser keeps one token of lookahead. */

#include <stdlib.h>
#include <string.h>

#include "basic.h"
#include "util.h"
#include "target.h"

/* A named compile-time integer constant (from CONST or ENUM). Constants have no
   runtime storage; a reference folds to its value at parse time, so a constant
   must be declared before it is used (the one-pass rule). */
struct const_ent {
    char             *name;
    long              val;
    struct const_ent *next;
};

/* A module brought in by `IMPORT M` (qualified): its members are reached dotted,
   as `M.name`, so the interface is kept to resolve a qualified reference. */
struct mod_ent {
    char            *name;        /* the module's qualifier */
    struct bmi      *bmi;         /* its interface */
    struct mod_ent  *next;
};

struct parser {
    struct arena     *arena;
    Lexer             lx;
    Token             cur;        /* current (unconsumed) token */
    const char       *filename;
    struct const_ent *consts;     /* defined integer constants */
    struct mod_ent   *modules;    /* qualified-imported modules (IMPORT M) */
};

/* Import search directories (-I), borrowed from the driver. Consulted in order
   to resolve a module name M to the archive <dir>/M.a. */
static const char **g_import_dirs;
static int          g_nimport_dirs;

void
basic_set_import_dirs(const char **dirs, int ndirs)
{
    g_import_dirs = dirs;
    g_nimport_dirs = ndirs;
}

static void
perr(struct parser *p, const char *msg)
{
    die("%s:%d: %s", p->filename, p->cur.line, msg);
}

static void
advance(struct parser *p)
{
    p->cur = lexer_next(&p->lx);
    if (p->cur.kind == TK_ERROR)
        die("%s:%d: %s", p->filename, p->cur.line,
            p->lx.error ? p->lx.error : "lex error");
}

static int
is_kw(struct parser *p, int kw)
{
    return p->cur.kind == TK_KEYWORD && p->cur.keyword == kw;
}

static void
expect_kw(struct parser *p, int kw, const char *what)
{
    if (!is_kw(p, kw))
        perr(p, what);
    advance(p);
}

static void
expect(struct parser *p, TokenKind k, const char *what)
{
    if (p->cur.kind != k)
        perr(p, what);
    advance(p);
}

static struct node *
mknode(struct parser *p, int kind)
{
    struct node *n = arena_zalloc(p->arena, sizeof(*n));
    n->kind = kind;
    n->line = p->cur.line;
    return n;
}

/* Copy an identifier lexeme into an arena NUL-terminated string. */
static char *
ident_text(struct parser *p)
{
    char *s = arena_alloc(p->arena, p->cur.length + 1);
    memcpy(s, p->cur.lexeme, p->cur.length);
    s[p->cur.length] = '\0';
    return s;
}

/* Look up a constant by its source spelling (lexeme + length). */
static struct const_ent *
const_lookup(struct parser *p, const char *s, size_t len)
{
    struct const_ent *c;

    for (c = p->consts; c; c = c->next)
        if (strlen(c->name) == len && memcmp(c->name, s, len) == 0)
            return c;
    return NULL;
}

/* Register a constant under an already-copied (NUL-terminated) name. */
static void
const_add(struct parser *p, char *name, long val)
{
    struct const_ent *c;

    if (const_lookup(p, name, strlen(name)))
        perr(p, "constant is already defined");
    c = arena_zalloc(p->arena, sizeof(*c));
    c->name = name;
    c->val = val;
    c->next = p->consts;
    p->consts = c;
}

/* Qualified-import helpers, defined with the import machinery below but needed
   here so a type spec can name a module's opaque type as `Module.Type`. */
static struct mod_ent *mod_lookup(struct parser *p, const char *name);
static char *qual_name(struct parser *p, const char *mod, const char *name);

/* If the current token is a scalar type keyword, return its TY_DECL_* and
   advance past it; otherwise return -1 and consume nothing. */
static int
scalar_decl_type(struct parser *p)
{
    int t;

    if (is_kw(p, KW_INT))         t = TY_DECL_INT;
    else if (is_kw(p, KW_FLOAT))  t = TY_DECL_FLOAT;
    else if (is_kw(p, KW_BYTE))   t = TY_DECL_BYTE;
    else if (is_kw(p, KW_STRING)) t = TY_DECL_STRING;
    else                          return -1;
    advance(p);
    return t;
}

/* A full type spec after AS: a scalar type, a local record/opaque type named by
   a bare identifier, or a qualified-imported opaque type named `Module.Type`.
   A record type is returned as TY_DECL_RECORD with *recname set (the dotted key
   for the qualified form, matching how the opaque type was registered on
   import). -1 if none of these. */
static int
decl_type_spec(struct parser *p, char **recname)
{
    int t = scalar_decl_type(p);
    char *name;
    struct mod_ent *m;

    if (t >= 0) {
        if (recname)
            *recname = NULL;
        return t;
    }
    if (p->cur.kind != TK_IDENT)
        return -1;
    name = ident_text(p);
    advance(p);
    if (p->cur.kind == TK_DOT && (m = mod_lookup(p, name)) != NULL) {
        char *member;
        advance(p);   /* consume '.' */
        if (p->cur.kind != TK_IDENT)
            perr(p, "expected an opaque type name after 'Module.'");
        member = ident_text(p);
        advance(p);
        if (recname)
            *recname = qual_name(p, m->name, member);
        return TY_DECL_RECORD;
    }
    if (recname)
        *recname = name;
    return TY_DECL_RECORD;
}

/* ---- expressions ---- */

static struct node *parse_expr(struct parser *p);
static struct node *parse_args(struct parser *p);
static struct node *parse_string(struct parser *p);

/* Qualified module references (IMPORT M; then M.name). Defined with the import
   machinery below, used from parse_primary and parse_statement above it. */
static struct mod_ent *mod_lookup(struct parser *p, const char *name);
static struct node *parse_qualified_ref(struct parser *p, struct mod_ent *m);

/* Map a built-in function keyword to its BF_* id, or -1 if not a built-in. */
static int
builtin_of_kw(int kw)
{
    switch (kw) {
    case KW_LEN:   return BF_LEN;
    case KW_ASC:   return BF_ASC;
    case KW_INSTR: return BF_INSTR;
    case KW_CHR:   return BF_CHR;
    case KW_STR:   return BF_STR;
    case KW_LEFT:  return BF_LEFT;
    case KW_RIGHT: return BF_RIGHT;
    case KW_MID:   return BF_MID;
    case KW_UCASE: return BF_UCASE;
    case KW_LCASE: return BF_LCASE;
    case KW_ABS:   return BF_ABS;
    case KW_SGN:   return BF_SGN;
    case KW_MIN:   return BF_MIN;
    case KW_MAX:   return BF_MAX;
    case KW_CLAMP: return BF_CLAMP;
    case KW_INT:   return BF_INT;
    case KW_FLOAT: return BF_FLOAT;
    case KW_FLOOR: return BF_FLOOR;
    case KW_CEIL:  return BF_CEIL;
    case KW_ROUND: return BF_ROUND;
    case KW_LERP:  return BF_LERP;
    case KW_SQR:   return BF_SQR;
    case KW_SIN:   return BF_SIN;
    case KW_COS:   return BF_COS;
    case KW_TAN:   return BF_TAN;
    case KW_ATN:   return BF_ATN;
    case KW_ATN2:  return BF_ATN2;
    case KW_SIND:  return BF_SIND;
    case KW_COSD:  return BF_COSD;
    case KW_TAND:  return BF_TAND;
    case KW_VAL:       return BF_VAL;
    case KW_VALINT:    return BF_VALINT;
    case KW_SPACE:     return BF_SPACE;
    case KW_STRING_FN: return BF_STRING;
    case KW_HEX:       return BF_HEX;
    case KW_BIN:       return BF_BIN;
    case KW_FIX:       return BF_FIX;
    case KW_ZERO:      return BF_ZERO;
    case KW_RJUST:     return BF_RJUST;
    case KW_LJUST:     return BF_LJUST;
    case KW_RND:       return BF_RND;
    case KW_SEED:      return BF_SEED;
    case KW_TIME:      return BF_TIME;
    default:       return -1;
    }
}

/* Primary = INTEGER_LITERAL | '(' Expression ')' | IDENTIFIER
           | Builtin '(' args ')' | STRING_LITERAL . */
static struct node *
parse_primary(struct parser *p)
{
    struct node *n;

    if (p->cur.kind == TK_KEYWORD) {
        int bf = builtin_of_kw(p->cur.keyword);
        if (bf < 0)
            perr(p, "that keyword is not valid in an expression");
        n = mknode(p, N_BUILTIN);
        n->op = bf;
        advance(p);
        /* RND and TIME take no arguments and may be written bare, without an
           empty parenthesis pair. */
        if ((bf == BF_RND || bf == BF_TIME) && p->cur.kind != TK_LPAREN)
            n->a = NULL;
        else
            n->a = parse_args(p);
        return n;
    }
    if (p->cur.kind == TK_INT_LIT) {
        n = mknode(p, N_INT);
        n->ival = (long)p->cur.int_val;
        advance(p);
        return n;
    }
    if (p->cur.kind == TK_LPAREN) {
        advance(p);
        n = parse_expr(p);
        expect(p, TK_RPAREN, "expected ')'");
        return n;
    }
    if (p->cur.kind == TK_FLOAT_LIT) {
        n = mknode(p, N_FLT);
        n->fval = p->cur.float_val;
        advance(p);
        return n;
    }
    if (p->cur.kind == TK_IDENT) {
        char  *name = ident_text(p);
        size_t nlen = p->cur.length;
        struct const_ent *c;
        struct mod_ent *m;
        advance(p);

        /* name '.' member, where name is a qualified-imported module, is a
           reference to that module's export (a folded CONST or an FN call). */
        if (p->cur.kind == TK_DOT && (m = mod_lookup(p, name)) != NULL)
            return parse_qualified_ref(p, m);

        /* name '(' ... ')' is a function call. */
        if (p->cur.kind == TK_LPAREN) {
            n = mknode(p, N_CALL);
            n->name = name;
            n->a = parse_args(p);
            return n;
        }
        /* a defined constant folds to its value */
        c = const_lookup(p, name, nlen);
        if (c) {
            n = mknode(p, N_INT);
            n->ival = c->val;
            return n;
        }
        /* PI is a predeclared FLOAT constant, unless the program shadows it. */
        if (nlen == 2 && (name[0] == 'P' || name[0] == 'p') &&
            (name[1] == 'I' || name[1] == 'i')) {
            n = mknode(p, N_FLT);
            n->fval = 3.14159265358979323846;
            return n;
        }
        n = mknode(p, N_VAR);
        n->name = name;
        /* record field access: var.field (one level this phase) */
        while (p->cur.kind == TK_DOT) {
            struct node *f = mknode(p, N_FIELD);
            advance(p);
            if (p->cur.kind != TK_IDENT)
                perr(p, "expected a field name after '.'");
            f->a = n;
            f->name = ident_text(p);
            advance(p);
            n = f;
        }
        return n;
    }
    if (p->cur.kind == TK_STRING_LIT)
        return parse_string(p);
    perr(p, "expected an expression");
    return NULL;   /* not reached */
}

/* PowExpr = Primary [ '^' SignExpr ] .  Right-associative. */
static struct node *parse_sign(struct parser *p);

static struct node *
parse_pow(struct parser *p)
{
    struct node *base = parse_primary(p);

    if (p->cur.kind == TK_CARET) {
        struct node *n = mknode(p, N_BINOP);
        n->op = BOP_POW;
        n->a = base;
        advance(p);
        n->b = parse_sign(p);   /* right operand recurses through the sign level */
        return n;
    }
    return base;
}

/* SignExpr = [ '+' | '-' | 'BNOT' ] PowExpr . */
static struct node *
parse_sign(struct parser *p)
{
    if (p->cur.kind == TK_MINUS) {
        struct node *n = mknode(p, N_NEG);
        advance(p);
        n->a = parse_sign(p);
        return n;
    }
    if (p->cur.kind == TK_PLUS) {
        advance(p);
        return parse_sign(p);   /* unary plus is a no-op */
    }
    if (is_kw(p, KW_BNOT)) {
        struct node *n = mknode(p, N_BNOT);
        advance(p);
        n->a = parse_sign(p);
        return n;
    }
    return parse_pow(p);
}

/* MulExpr = SignExpr { MulOp SignExpr } .
   MulOp = '*' | 'DIV' | 'MOD' | 'BAND' | 'BOR' | 'BXOR' | 'SHL' | 'SHR' .
   '/' (FLOAT division) is handled in the FLOAT phase. */
static struct node *
parse_mul(struct parser *p)
{
    struct node *left = parse_sign(p);

    for (;;) {
        int op;
        if (p->cur.kind == TK_STAR)       op = BOP_MUL;
        else if (is_kw(p, KW_DIV))        op = BOP_DIV;
        else if (is_kw(p, KW_MOD))        op = BOP_MOD;
        else if (is_kw(p, KW_BAND))       op = BOP_BAND;
        else if (is_kw(p, KW_BOR))        op = BOP_BOR;
        else if (is_kw(p, KW_BXOR))       op = BOP_BXOR;
        else if (is_kw(p, KW_SHL))        op = BOP_SHL;
        else if (is_kw(p, KW_SHR))        op = BOP_SHR;
        else if (p->cur.kind == TK_SLASH) op = BOP_SLASH;
        else
            break;
        {
            struct node *n = mknode(p, N_BINOP);
            n->op = op;
            n->a = left;
            advance(p);
            n->b = parse_sign(p);
            left = n;
        }
    }
    return left;
}

/* AddExpr = MulExpr { ( '+' | '-' ) MulExpr } . */
static struct node *
parse_add(struct parser *p)
{
    struct node *left = parse_mul(p);

    for (;;) {
        int op;
        if (p->cur.kind == TK_PLUS)
            op = BOP_ADD;
        else if (p->cur.kind == TK_MINUS)
            op = BOP_SUB;
        else
            break;
        {
            struct node *n = mknode(p, N_BINOP);
            n->op = op;
            n->a = left;
            advance(p);
            n->b = parse_mul(p);
            left = n;
        }
    }
    return left;
}

/* RelExpr = AddExpr [ RelOp AddExpr ] .  Non-associative. */
static struct node *
parse_rel(struct parser *p)
{
    struct node *left = parse_add(p);
    int op;

    switch (p->cur.kind) {
    case TK_EQ: op = BOP_EQ; break;
    case TK_NE: op = BOP_NE; break;
    case TK_LT: op = BOP_LT; break;
    case TK_LE: op = BOP_LE; break;
    case TK_GT: op = BOP_GT; break;
    case TK_GE: op = BOP_GE; break;
    default: return left;
    }
    {
        struct node *n = mknode(p, N_BINOP);
        n->op = op;
        n->a = left;
        advance(p);
        n->b = parse_add(p);
        return n;
    }
}

/* NotExpr = 'NOT' NotExpr | RelExpr . */
static struct node *
parse_not(struct parser *p)
{
    if (is_kw(p, KW_NOT)) {
        struct node *n = mknode(p, N_NOT);
        advance(p);
        n->a = parse_not(p);
        return n;
    }
    return parse_rel(p);
}

/* AndExpr = NotExpr { 'AND' NotExpr } . */
static struct node *
parse_and(struct parser *p)
{
    struct node *left = parse_not(p);

    while (is_kw(p, KW_AND)) {
        struct node *n = mknode(p, N_BINOP);
        n->op = BOP_AND;
        n->a = left;
        advance(p);
        n->b = parse_not(p);
        left = n;
    }
    return left;
}

/* Expression = AndExpr { 'OR' AndExpr } . */
static struct node *
parse_expr(struct parser *p)
{
    struct node *left = parse_and(p);

    while (is_kw(p, KW_OR)) {
        struct node *n = mknode(p, N_BINOP);
        n->op = BOP_OR;
        n->a = left;
        advance(p);
        n->b = parse_and(p);
        left = n;
    }
    return left;
}

/* An argument list '(' [ Expression { ',' Expression } ] ')', returned as a
   chain of expression nodes linked by ->next. */
static struct node *
parse_args(struct parser *p)
{
    struct node *head = NULL;
    struct node **tail = &head;

    expect(p, TK_LPAREN, "expected '(' before an argument list");
    if (p->cur.kind != TK_RPAREN) {
        for (;;) {
            struct node *e = parse_expr(p);
            *tail = e;
            tail = &e->next;
            if (p->cur.kind == TK_COMMA) {
                advance(p);
                continue;
            }
            break;
        }
    }
    expect(p, TK_RPAREN, "expected ')' after an argument list");
    return head;
}

/* Decode a string literal's raw bytes, turning each "" into one ". */
static struct node *
parse_string(struct parser *p)
{
    struct node *n = mknode(p, N_STR);
    const char *s = p->cur.lexeme;
    size_t raw = p->cur.length;
    char *out = arena_alloc(p->arena, raw + 1);
    size_t j = 0;

    for (size_t i = 0; i < raw; i++) {
        if (s[i] == '"' && i + 1 < raw && s[i + 1] == '"') {
            out[j++] = '"';
            i++;
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    n->sval = out;
    n->slen = (int)j;
    advance(p);
    return n;
}

/* Fold a parsed expression to a compile-time integer. Constant references have
   already become N_INT in parse_primary, so a surviving N_VAR (or any node that
   is not a constant operator) means the expression is not constant. */
static long
fold_const(struct parser *p, struct node *n)
{
    long a, b;

    switch (n->kind) {
    case N_INT:
        return n->ival;
    case N_FLT:
        perr(p, "FLOAT constant expressions are not yet supported");
        return 0;   /* not reached */
    case N_NEG:
        return -fold_const(p, n->a);
    case N_NOT:
        return fold_const(p, n->a) == 0 ? 1 : 0;
    case N_BNOT:
        return ~fold_const(p, n->a);
    case N_BINOP:
        a = fold_const(p, n->a);
        b = fold_const(p, n->b);
        switch (n->op) {
        case BOP_ADD:  return a + b;
        case BOP_SUB:  return a - b;
        case BOP_MUL:  return a * b;
        case BOP_DIV:
            if (b == 0)
                perr(p, "division by zero in a constant expression");
            return a / b;
        case BOP_MOD:
            if (b == 0)
                perr(p, "division by zero in a constant expression");
            return a % b;
        case BOP_BAND: return a & b;
        case BOP_BOR:  return a | b;
        case BOP_BXOR: return a ^ b;
        case BOP_SHL:  return a << b;
        case BOP_SHR:  return a >> b;
        case BOP_EQ:   return a == b;
        case BOP_NE:   return a != b;
        case BOP_LT:   return a < b;
        case BOP_LE:   return a <= b;
        case BOP_GT:   return a > b;
        case BOP_GE:   return a >= b;
        case BOP_AND:  return (a != 0 && b != 0);
        case BOP_OR:   return (a != 0 || b != 0);
        case BOP_POW: {
            long r = 1;
            if (b < 0)
                return 0;
            while (b-- > 0)
                r *= a;
            return r;
        }
        }
        break;
    default:
        break;
    }
    perr(p, "expected a constant expression");
    return 0;   /* not reached */
}

/* ---- statements ---- */

/* A statement ends at a newline, a ':' separator, or end of file. ELSE also
   ends one: in a single-line IF the then-branch runs up to ELSE, so the branch
   statement must stop there rather than swallow it. */
static int
at_terminator(struct parser *p)
{
    return p->cur.kind == TK_NEWLINE || p->cur.kind == TK_COLON ||
           p->cur.kind == TK_EOF ||
           (p->cur.kind == TK_KEYWORD && p->cur.keyword == KW_ELSE);
}

/* PrintStmt = 'PRINT' [ PrintItem { PrintSep PrintItem } [ PrintSep ] ] . */
static struct node *
parse_print(struct parser *p)
{
    struct node *n = mknode(p, N_PRINT);
    struct node **tail = &n->a;

    advance(p);   /* 'PRINT' */
    while (!at_terminator(p)) {
        struct node *item = mknode(p, N_PRINTITEM);
        item->a = parse_expr(p);
        *tail = item;
        tail = &item->next;

        if (p->cur.kind == TK_SEMI || p->cur.kind == TK_COMMA) {
            item->op = p->cur.kind;
            advance(p);
            if (at_terminator(p)) {
                n->ival = 1;   /* trailing separator suppresses the newline */
                break;
            }
        } else {
            break;
        }
    }
    return n;
}

/* DimStmt = 'DIM' DimItem { ',' DimItem } .  Scalar INT or FLOAT so far;
   arrays and the aggregate types land in a later phase. */
static struct node *
parse_dim(struct parser *p, struct node ***tail)
{
    struct node *first = NULL;

    advance(p);   /* 'DIM' */
    for (;;) {
        struct node *n = mknode(p, N_DIM);
        if (p->cur.kind != TK_IDENT)
            perr(p, "expected a variable name after DIM");
        int t;
        n->name = ident_text(p);
        advance(p);
        /* optional array bounds: DIM a(64) AS T, DIM g(80, 25) AS T */
        if (p->cur.kind == TK_LPAREN) {
            struct node **bt = &n->a;
            advance(p);
            for (;;) {
                struct node *b = mknode(p, N_INT);
                b->ival = fold_const(p, parse_expr(p));
                *bt = b;
                bt = &b->next;
                if (p->cur.kind == TK_COMMA) {
                    advance(p);
                    continue;
                }
                break;
            }
            expect(p, TK_RPAREN, "expected ')' after the array bounds");
        }
        expect_kw(p, KW_AS, "expected AS in a DIM");
        {
            char *rn;
            t = decl_type_spec(p, &rn);
            if (t < 0)
                perr(p, "a DIM variable needs a type (INT, FLOAT, BYTE, "
                        "STRING, or a record type)");
            n->op = t;
            n->sval = rn;               /* record type name, or NULL */
        }
        if (!first)
            first = n;
        else {
            **tail = n;
            *tail = &n->next;
        }
        if (p->cur.kind == TK_COMMA) {
            advance(p);
            continue;
        }
        break;
    }
    return first;
}

/* ConstStmt = 'CONST' IDENTIFIER '=' ConstExpr { ',' IDENTIFIER '=' ConstExpr } .
   Each value folds to an integer constant and emits no runtime node. When the
   CONST is exported (EXPORT CONST in a module), each constant also gets an
   N_CONST node appended through *tail so the interface writer can see its value;
   a non-exported CONST passes exported = 0 and tail = NULL. */
static void
parse_const(struct parser *p, int exported, struct node ***tail)
{
    advance(p);   /* 'CONST' */
    for (;;) {
        char *name;
        long val;
        if (p->cur.kind != TK_IDENT)
            perr(p, "expected a constant name after CONST");
        name = ident_text(p);
        advance(p);
        expect(p, TK_EQ, "expected '=' in a CONST");
        val = fold_const(p, parse_expr(p));
        const_add(p, name, val);
        if (exported) {
            struct node *c = mknode(p, N_CONST);
            c->name = name;
            c->ival = val;
            c->is_export = 1;
            **tail = c;
            *tail = &c->next;
        }
        if (p->cur.kind == TK_COMMA) {
            advance(p);
            continue;
        }
        break;
    }
}

/* One EnumItem = IDENTIFIER [ '=' ConstExpr ] .  *next is the value to assign
   when there is no '='; it is advanced past this item. */
static void
parse_enum_item(struct parser *p, long *next)
{
    char *name;

    if (p->cur.kind != TK_IDENT)
        perr(p, "expected an enum item name");
    name = ident_text(p);
    advance(p);
    if (p->cur.kind == TK_EQ) {
        advance(p);
        *next = fold_const(p, parse_expr(p));
    }
    const_add(p, name, *next);
    (*next)++;
}

/* EnumStmt = 'ENUM' EnumItem { ',' EnumItem }
            | 'ENUM' NEWLINE { EnumItem { ',' EnumItem } NEWLINE } 'ENDENUM' .
   The items are INT constants counting up from 0; an item with '=' restarts the
   count. Emits no runtime node. */
static void
parse_enum(struct parser *p)
{
    long next = 0;

    advance(p);   /* 'ENUM' */
    if (p->cur.kind == TK_NEWLINE) {
        /* block form: one item or comma list per line, until ENDENUM */
        advance(p);
        while (!is_kw(p, KW_ENDENUM)) {
            if (p->cur.kind == TK_NEWLINE) {   /* blank line */
                advance(p);
                continue;
            }
            parse_enum_item(p, &next);
            while (p->cur.kind == TK_COMMA) {
                advance(p);
                parse_enum_item(p, &next);
            }
            if (p->cur.kind != TK_NEWLINE)
                perr(p, "expected a newline after an enum item");
            advance(p);
        }
        advance(p);   /* 'ENDENUM' */
        return;
    }
    /* inline form */
    parse_enum_item(p, &next);
    while (p->cur.kind == TK_COMMA) {
        advance(p);
        parse_enum_item(p, &next);
    }
}

/* ---- control flow ---- */

static void parse_statement(struct parser *p, struct node ***tail);

/* The keywords that close a nested statement list. None can begin a statement,
   so parse_stmt_list stops (without consuming) when it reaches one and lets the
   enclosing compound statement consume its own closer. */
static int
is_block_end(int kw)
{
    switch (kw) {
    case KW_ENDIF: case KW_ELSE: case KW_ELSEIF:
    case KW_ENDWHILE: case KW_UNTIL: case KW_NEXT:
    case KW_ENDCASE: case KW_WHEN:
    case KW_ENDPROC: case KW_ENDFN:
        return 1;
    default:
        return 0;
    }
}

/* Parse statements until a block-closing keyword or end of file, returning the
   list head. Blank lines and ':' separators are skipped. */
static struct node *
parse_stmt_list(struct parser *p)
{
    struct node *head = NULL;
    struct node **tail = &head;

    for (;;) {
        if (p->cur.kind == TK_NEWLINE || p->cur.kind == TK_COLON) {
            advance(p);
            continue;
        }
        if (p->cur.kind == TK_EOF)
            break;
        if (p->cur.kind == TK_KEYWORD && is_block_end(p->cur.keyword))
            break;
        parse_statement(p, &tail);
        if (!at_terminator(p))
            perr(p, "expected end of statement");
    }
    return head;
}

/* Parse exactly one statement (the branch of a single-line IF), returning it. */
static struct node *
parse_one_stmt(struct parser *p)
{
    struct node *head = NULL;
    struct node **tail = &head;

    parse_statement(p, &tail);
    return head;
}

/* IfStmt. The block and single-line forms are told apart by a NEWLINE right
   after THEN. Block ELSEIF is desugared to a nested N_IF hung on the else
   branch, so there is one node shape for both. */
static struct node *
parse_if(struct parser *p)
{
    struct node *n = mknode(p, N_IF);

    advance(p);                                 /* IF */
    n->a = parse_expr(p);
    expect_kw(p, KW_THEN, "expected THEN after an IF condition");

    if (p->cur.kind != TK_NEWLINE) {
        /* single-line form */
        n->b = parse_one_stmt(p);
        if (is_kw(p, KW_ELSE)) {
            advance(p);
            n->c = parse_one_stmt(p);
        }
        return n;
    }

    /* block form */
    advance(p);                                 /* NEWLINE */
    n->b = parse_stmt_list(p);
    {
        struct node *cur = n;
        while (is_kw(p, KW_ELSEIF)) {
            struct node *e = mknode(p, N_IF);
            advance(p);                         /* ELSEIF */
            e->a = parse_expr(p);
            expect_kw(p, KW_THEN, "expected THEN after an ELSEIF condition");
            if (p->cur.kind != TK_NEWLINE)
                perr(p, "expected a newline after ELSEIF ... THEN");
            advance(p);
            e->b = parse_stmt_list(p);
            cur->c = e;
            cur = e;
        }
        if (is_kw(p, KW_ELSE)) {
            advance(p);                         /* ELSE */
            if (p->cur.kind == TK_NEWLINE)
                advance(p);
            cur->c = parse_stmt_list(p);
        }
    }
    expect_kw(p, KW_ENDIF, "expected ENDIF");
    return n;
}

/* WhileStmt = 'WHILE' Expression NEWLINE StatementList 'ENDWHILE' . */
static struct node *
parse_while(struct parser *p)
{
    struct node *n = mknode(p, N_WHILE);

    advance(p);                                 /* WHILE */
    n->a = parse_expr(p);
    if (p->cur.kind != TK_NEWLINE)
        perr(p, "expected a newline after a WHILE condition");
    advance(p);
    n->b = parse_stmt_list(p);
    expect_kw(p, KW_ENDWHILE, "expected ENDWHILE");
    return n;
}

/* RepeatStmt = 'REPEAT' NEWLINE StatementList 'UNTIL' Expression . */
static struct node *
parse_repeat(struct parser *p)
{
    struct node *n = mknode(p, N_REPEAT);

    advance(p);                                 /* REPEAT */
    if (p->cur.kind != TK_NEWLINE)
        perr(p, "expected a newline after REPEAT");
    advance(p);
    n->b = parse_stmt_list(p);
    expect_kw(p, KW_UNTIL, "expected UNTIL");
    n->a = parse_expr(p);
    return n;
}

/* ForStmt = 'FOR' IDENT '=' Expr 'TO' Expr [ 'STEP' Expr ] NEWLINE
             StatementList 'NEXT' [ IDENT ] .  FOR EACH is a later phase. */
static struct node *
parse_for(struct parser *p)
{
    struct node *n = mknode(p, N_FOR);

    advance(p);                                 /* FOR */
    if (is_kw(p, KW_EACH)) {
        advance(p);                             /* EACH */
        n->kind = N_FOREACH;
        if (p->cur.kind != TK_IDENT)
            perr(p, "expected an element variable after FOR EACH");
        n->name = ident_text(p);
        advance(p);
        expect_kw(p, KW_IN, "expected IN in a FOR EACH");
        if (p->cur.kind != TK_IDENT)
            perr(p, "expected a collection name after IN");
        n->a = mknode(p, N_VAR);
        n->a->name = ident_text(p);
        advance(p);
        if (p->cur.kind != TK_NEWLINE)
            perr(p, "expected a newline after a FOR EACH header");
        advance(p);
        n->d = parse_stmt_list(p);
        expect_kw(p, KW_NEXT, "expected NEXT");
        if (p->cur.kind == TK_IDENT) {
            char *nm = ident_text(p);
            if (strcmp(nm, n->name) != 0)
                perr(p, "the NEXT variable does not match the FOR EACH variable");
            advance(p);
        }
        return n;
    }
    if (p->cur.kind != TK_IDENT)
        perr(p, "expected a control variable after FOR");
    n->name = ident_text(p);
    advance(p);
    expect(p, TK_EQ, "expected '=' in a FOR");
    n->a = parse_expr(p);
    expect_kw(p, KW_TO, "expected TO in a FOR");
    n->b = parse_expr(p);
    if (is_kw(p, KW_STEP)) {
        advance(p);
        n->c = parse_expr(p);
    }
    if (p->cur.kind != TK_NEWLINE)
        perr(p, "expected a newline after a FOR header");
    advance(p);
    n->d = parse_stmt_list(p);
    expect_kw(p, KW_NEXT, "expected NEXT");
    if (p->cur.kind == TK_IDENT) {
        char *nm = ident_text(p);
        if (strcmp(nm, n->name) != 0)
            perr(p, "the NEXT variable does not match the FOR variable");
        advance(p);
    }
    return n;
}

/* ExitStmt = 'EXIT' ( 'WHILE' | 'REPEAT' | 'FOR' ) . */
static struct node *
parse_exit(struct parser *p)
{
    struct node *n = mknode(p, N_EXIT);

    advance(p);                                 /* EXIT */
    if (is_kw(p, KW_WHILE))
        n->op = EXIT_WHILE;
    else if (is_kw(p, KW_REPEAT))
        n->op = EXIT_REPEAT;
    else if (is_kw(p, KW_FOR))
        n->op = EXIT_FOR;
    else
        perr(p, "EXIT must name WHILE, REPEAT, or FOR");
    advance(p);
    return n;
}

/* WhenItem = Expression [ 'TO' Expression ] | RelOp Expression . */
static struct node *
parse_when_item(struct parser *p)
{
    struct node *item = mknode(p, N_WHENITEM);
    int rel = -1;

    switch (p->cur.kind) {
    case TK_NE: rel = BOP_NE; break;
    case TK_LT: rel = BOP_LT; break;
    case TK_LE: rel = BOP_LE; break;
    case TK_GT: rel = BOP_GT; break;
    case TK_GE: rel = BOP_GE; break;
    default:    break;
    }
    if (rel >= 0) {
        item->op = WI_REL;
        item->ival = rel;
        advance(p);
        item->a = parse_expr(p);
        return item;
    }
    item->a = parse_expr(p);
    if (is_kw(p, KW_TO)) {
        advance(p);
        item->op = WI_RANGE;
        item->b = parse_expr(p);
    } else {
        item->op = WI_VALUE;
    }
    return item;
}

/* CaseStmt = 'CASE' Expression NEWLINE
              { 'WHEN' WhenItem { ',' WhenItem } NEWLINE StatementList }
              [ 'ELSE' NEWLINE StatementList ] 'ENDCASE' . */
static struct node *
parse_case(struct parser *p)
{
    struct node *n = mknode(p, N_CASE);
    struct node **wt = &n->b;

    advance(p);                                 /* CASE */
    n->a = parse_expr(p);
    if (p->cur.kind != TK_NEWLINE)
        perr(p, "expected a newline after a CASE selector");
    advance(p);
    while (p->cur.kind == TK_NEWLINE)           /* blank lines before WHEN */
        advance(p);

    while (is_kw(p, KW_WHEN)) {
        struct node *w = mknode(p, N_WHEN);
        struct node **it = &w->a;
        advance(p);                             /* WHEN */
        for (;;) {
            struct node *item = parse_when_item(p);
            *it = item;
            it = &item->next;
            if (p->cur.kind == TK_COMMA) {
                advance(p);
                continue;
            }
            break;
        }
        if (p->cur.kind != TK_NEWLINE)
            perr(p, "expected a newline after the WHEN items");
        advance(p);
        w->b = parse_stmt_list(p);
        *wt = w;
        wt = &w->next;
    }
    if (is_kw(p, KW_ELSE)) {
        advance(p);
        if (p->cur.kind == TK_NEWLINE)
            advance(p);
        n->c = parse_stmt_list(p);
    }
    expect_kw(p, KW_ENDCASE, "expected ENDCASE");
    return n;
}

/* ---- procedures and functions ---- */

/* ParamList = Param { ',' Param } , Param = [ 'REF' ] IDENT 'AS' Type .
   A scalar or STRING is by value; REF (record parameters only) is by reference.
   The REF flag is kept in the N_PARAM ival; a record type name in sval. */
static struct node *
parse_params(struct parser *p)
{
    struct node *head = NULL;
    struct node **tail = &head;

    expect(p, TK_LPAREN, "expected '(' after the routine name");
    if (p->cur.kind != TK_RPAREN) {
        for (;;) {
            struct node *pm = mknode(p, N_PARAM);
            char *rn;
            int t;
            if (is_kw(p, KW_REF)) {
                pm->ival = 1;
                advance(p);
            }
            if (p->cur.kind != TK_IDENT)
                perr(p, "expected a parameter name");
            pm->name = ident_text(p);
            advance(p);
            expect_kw(p, KW_AS, "expected AS in a parameter");
            t = decl_type_spec(p, &rn);
            if (t < 0)
                perr(p, "a parameter needs a type (INT, FLOAT, BYTE, STRING, "
                        "or a record type)");
            pm->op = t;
            pm->sval = rn;
            *tail = pm;
            tail = &pm->next;
            if (p->cur.kind == TK_COMMA) {
                advance(p);
                continue;
            }
            break;
        }
    }
    expect(p, TK_RPAREN, "expected ')' after the parameter list");
    return head;
}

/* ProcDecl = 'PROC' IDENT '(' [ ParamList ] ')' StatementList 'ENDPROC' . */
static struct node *
parse_proc(struct parser *p)
{
    struct node *n = mknode(p, N_PROC);

    advance(p);                                 /* PROC */
    if (p->cur.kind != TK_IDENT)
        perr(p, "expected a PROC name");
    n->name = ident_text(p);
    advance(p);
    n->a = parse_params(p);
    if (p->cur.kind != TK_NEWLINE)
        perr(p, "expected a newline after the PROC header");
    advance(p);
    n->b = parse_stmt_list(p);
    expect_kw(p, KW_ENDPROC, "expected ENDPROC");
    return n;
}

/* FnDecl = 'FN' IDENT '(' [ ParamList ] ')' 'AS' Type StatementList 'ENDFN' .
   A single scalar return type so far; multiple returns are a later phase. */
static struct node *
parse_fn(struct parser *p)
{
    struct node *n = mknode(p, N_FN);
    int t;

    advance(p);                                 /* FN */
    if (p->cur.kind != TK_IDENT)
        perr(p, "expected an FN name");
    n->name = ident_text(p);
    advance(p);
    n->a = parse_params(p);
    expect_kw(p, KW_AS, "expected AS before the FN return type");
    t = decl_type_spec(p, &n->sval);
    if (t < 0)
        perr(p, "an FN must return INT, FLOAT, BYTE, STRING, or an opaque TYPE");
    n->op = t;
    if (p->cur.kind == TK_COMMA)
        perr(p, "multiple FN return values are not yet supported");
    if (p->cur.kind != TK_NEWLINE)
        perr(p, "expected a newline after the FN header");
    advance(p);
    n->b = parse_stmt_list(p);
    expect_kw(p, KW_ENDFN, "expected ENDFN");
    return n;
}

/* ForwardDecl = 'DECLARE' ( 'PROC' ... | 'FN' ... 'AS' Type ) .  No body. */
static struct node *
parse_declare(struct parser *p)
{
    struct node *n;
    int t;

    advance(p);                                 /* DECLARE */
    if (is_kw(p, KW_PROC)) {
        n = mknode(p, N_PROC);
        advance(p);
        if (p->cur.kind != TK_IDENT)
            perr(p, "expected a PROC name");
        n->name = ident_text(p);
        advance(p);
        n->a = parse_params(p);
    } else if (is_kw(p, KW_FN)) {
        n = mknode(p, N_FN);
        advance(p);
        if (p->cur.kind != TK_IDENT)
            perr(p, "expected an FN name");
        n->name = ident_text(p);
        advance(p);
        n->a = parse_params(p);
        expect_kw(p, KW_AS, "expected AS before the FN return type");
        t = decl_type_spec(p, &n->sval);
        if (t < 0)
            perr(p, "an FN must return INT, FLOAT, BYTE, STRING, or an opaque TYPE");
        n->op = t;
        if (p->cur.kind == TK_COMMA)
            perr(p, "multiple FN return values are not yet supported");
    } else {
        perr(p, "DECLARE must be followed by PROC or FN");
        return NULL;                            /* not reached */
    }
    n->ival = 1;                                /* forward declaration, no body */
    return n;
}

/* ReturnStmt = 'RETURN' [ Expression ] . */
static struct node *
parse_return(struct parser *p)
{
    struct node *n = mknode(p, N_RETURN);

    advance(p);                                 /* RETURN */
    if (!at_terminator(p)) {
        n->a = parse_expr(p);
        if (p->cur.kind == TK_COMMA)
            perr(p, "multiple RETURN values are not yet supported");
    }
    return n;
}

/* InputStmt = 'INPUT' [ STRING_LITERAL ';' ] Designator { ',' Designator } .
   Designators are scalar variables so far (arrays and fields are a later
   phase). */
static struct node *
parse_input(struct parser *p)
{
    struct node *n = mknode(p, N_INPUT);
    struct node **tail = &n->b;

    advance(p);                                 /* INPUT */
    if (p->cur.kind == TK_STRING_LIT) {
        n->a = parse_string(p);
        expect(p, TK_SEMI, "expected ';' after an INPUT prompt");
    }
    for (;;) {
        struct node *v;
        if (p->cur.kind != TK_IDENT)
            perr(p, "expected a variable after INPUT");
        v = mknode(p, N_VAR);
        v->name = ident_text(p);
        advance(p);
        *tail = v;
        tail = &v->next;
        if (p->cur.kind == TK_COMMA) {
            advance(p);
            continue;
        }
        break;
    }
    return n;
}

/* TypeDecl = 'TYPE' IDENT NEWLINE { Field } 'ENDTYPE' .
   Field = IDENT 'AS' Type ( ':' | NEWLINE ) .  Scalar and STRING fields so far;
   array, nested-record, and vector fields are a later phase. */
static struct node *
parse_type(struct parser *p)
{
    struct node *n = mknode(p, N_TYPE);
    struct node **tail = &n->a;

    advance(p);                                 /* TYPE */
    if (p->cur.kind != TK_IDENT)
        perr(p, "expected a TYPE name");
    n->name = ident_text(p);
    advance(p);
    if (p->cur.kind != TK_NEWLINE)
        perr(p, "expected a newline after the TYPE name");
    advance(p);

    while (!is_kw(p, KW_ENDTYPE)) {
        struct node *fld;
        char *rn;
        int t;
        if (p->cur.kind == TK_NEWLINE) {        /* blank line */
            advance(p);
            continue;
        }
        if (p->cur.kind == TK_EOF)
            perr(p, "unterminated TYPE (expected ENDTYPE)");
        fld = mknode(p, N_PARAM);
        if (p->cur.kind != TK_IDENT)
            perr(p, "expected a field name");
        fld->name = ident_text(p);
        advance(p);
        expect_kw(p, KW_AS, "expected AS in a field");
        t = decl_type_spec(p, &rn);
        if (t < 0)
            perr(p, "a field needs a type");
        fld->op = t;
        fld->sval = rn;
        *tail = fld;
        tail = &fld->next;
        if (p->cur.kind == TK_COLON || p->cur.kind == TK_NEWLINE)
            advance(p);
        else
            perr(p, "expected a newline or ':' after a field");
    }
    advance(p);                                 /* ENDTYPE */
    return n;
}

/* A single statement, appended through *tail (so DIM can emit several). */
static void
parse_statement(struct parser *p, struct node ***tail)
{
    struct node *n;

    if (p->cur.kind == TK_KEYWORD) {
        switch (p->cur.keyword) {
        case KW_PRINT:
            n = parse_print(p);
            **tail = n; *tail = &n->next;
            return;
        case KW_END:
            n = mknode(p, N_END);
            advance(p);
            **tail = n; *tail = &n->next;
            return;
        case KW_DIM:
            n = parse_dim(p, tail);
            **tail = n; *tail = &n->next;
            while (**tail)          /* DIM may have chained several items */
                *tail = &(**tail)->next;
            return;
        case KW_CONST:
            parse_const(p, 0, NULL);    /* compile-time only, no runtime node */
            return;
        case KW_ENUM:
            parse_enum(p);          /* compile-time only, no runtime node */
            return;
        case KW_IF:     n = parse_if(p);     **tail = n; *tail = &n->next; return;
        case KW_WHILE:  n = parse_while(p);  **tail = n; *tail = &n->next; return;
        case KW_REPEAT: n = parse_repeat(p); **tail = n; *tail = &n->next; return;
        case KW_FOR:    n = parse_for(p);    **tail = n; *tail = &n->next; return;
        case KW_CASE:   n = parse_case(p);   **tail = n; *tail = &n->next; return;
        case KW_EXIT:   n = parse_exit(p);   **tail = n; *tail = &n->next; return;
        case KW_RETURN: n = parse_return(p); **tail = n; *tail = &n->next; return;
        case KW_INPUT:  n = parse_input(p);  **tail = n; *tail = &n->next; return;
        case KW_WAIT:
            n = mknode(p, N_WAIT);
            advance(p);                 /* WAIT */
            n->a = parse_expr(p);       /* milliseconds */
            **tail = n; *tail = &n->next;
            return;
        default:
            perr(p, "statement not supported in this phase");
        }
    }

    if (p->cur.kind == TK_IDENT) {
        /* AssignOrCall. A trailing '(' is either an indexed assignment target
           (an array element) or a PROC call, told apart by a following '='. */
        char *name = ident_text(p);
        struct mod_ent *m;
        advance(p);
        /* A qualified call statement: M.Proc(args), where M was IMPORTed. */
        if (p->cur.kind == TK_DOT && (m = mod_lookup(p, name)) != NULL) {
            n = parse_qualified_ref(p, m);
            if (n->kind != N_CALL)
                perr(p, "a module constant is not a statement");
            **tail = n; *tail = &n->next;
            return;
        }
        if (p->cur.kind == TK_LPAREN) {
            struct node *desig = mknode(p, N_CALL);
            desig->name = name;
            desig->a = parse_args(p);
            if (p->cur.kind == TK_EQ) {
                advance(p);
                n = mknode(p, N_ASSIGN);
                n->b = desig;                   /* indexed target */
                n->a = parse_expr(p);
            } else {
                n = desig;                      /* a PROC call */
            }
            **tail = n; *tail = &n->next;
            return;
        }
        if (p->cur.kind == TK_DOT) {
            /* record field assignment: var.field = expr */
            struct node *base = mknode(p, N_VAR);
            base->name = name;
            while (p->cur.kind == TK_DOT) {
                struct node *f = mknode(p, N_FIELD);
                advance(p);
                if (p->cur.kind != TK_IDENT)
                    perr(p, "expected a field name after '.'");
                f->a = base;
                f->name = ident_text(p);
                advance(p);
                base = f;
            }
            n = mknode(p, N_ASSIGN);
            n->b = base;                        /* field target */
            expect(p, TK_EQ, "expected '=' in an assignment");
            n->a = parse_expr(p);
            **tail = n; *tail = &n->next;
            return;
        }
        n = mknode(p, N_ASSIGN);
        n->name = name;
        expect(p, TK_EQ, "expected '=' in an assignment");
        n->a = parse_expr(p);
        **tail = n; *tail = &n->next;
        return;
    }

    perr(p, "expected a statement");
}

/* Resolve a module name to its archive path by searching the import directories
   for `<dir>/<name>.a` (and the current directory as a final fallback). Returns
   an arena path, or dies naming the module that could not be found. */
static char *
resolve_module(struct parser *p, const char *mod)
{
    char buf[1024];
    int i;

    for (i = 0; i <= g_nimport_dirs; i++) {
        const char *dir = i < g_nimport_dirs ? g_import_dirs[i] : ".";
        FILE *f;

        if (snprintf(buf, sizeof buf, "%s/%s.a", dir, mod) >= (int)sizeof buf)
            continue;
        f = fopen(buf, "rb");
        if (f) {
            fclose(f);
            return arena_strdup(p->arena, buf);
        }
    }
    die("%s:%d: cannot find module '%s' (looked for '%s.a' on the import path)",
        p->filename, p->cur.line, mod, mod);
    return NULL;   /* unreachable */
}

/* Find an exported symbol by its BASIC name in a module interface. */
static struct bmi_sym *
bmi_find(struct bmi *m, const char *name)
{
    struct bmi_sym *s;

    for (s = m->syms; s; s = s->next)
        if (strcmp(s->name, name) == 0)
            return s;
    return NULL;
}

/* Load a module's interface from its archive on the import path, verifying its
   declared name and that it is built for this target. */
static struct bmi *
load_module(struct parser *p, const char *mod)
{
    char *path, *text;
    struct bmi *m;
    size_t tlen;

    path = resolve_module(p, mod);
    text = bmi_from_archive(p->arena, path, &tlen);
    m = bmi_read(p->arena, text, tlen, path);
    if (m->module && strcmp(m->module, mod) != 0)
        die("%s:%d: module '%s' declares its name as '%s'",
            p->filename, p->cur.line, mod, m->module);
    /* A .bmi is per target: its pointer width and endianness are authoritative,
       so a module built for another target would miscompile every call here. */
    if (m->lp64 != SKJ_LP64 || m->big_endian != SKJ_BIG_ENDIAN)
        die("%s:%d: module '%s' is built for %s %s-endian, but this target is "
            "%s %s-endian", p->filename, p->cur.line, mod,
            m->lp64 ? "lp64" : "ilp32", m->big_endian ? "big" : "little",
            SKJ_LP64 ? "lp64" : "ilp32", SKJ_BIG_ENDIAN ? "big" : "little");
    return m;
}

/* Build the dotted key `mod.name`, used as the func-table key and const name of
   a qualified import so it never collides with an unqualified name. */
static char *
qual_name(struct parser *p, const char *mod, const char *name)
{
    size_t ml = strlen(mod), nl = strlen(name);
    char *q = arena_alloc(p->arena, ml + nl + 2);

    memcpy(q, mod, ml);
    q[ml] = '.';
    memcpy(q + ml + 1, name, nl);
    q[ml + nl + 1] = '\0';
    return q;
}

static struct mod_ent *
mod_lookup(struct parser *p, const char *name)
{
    struct mod_ent *e;

    for (e = p->modules; e; e = e->next)
        if (strcmp(e->name, name) == 0)
            return e;
    return NULL;
}

/* Append an N_IMPORT node registering `s` as an external routine under `key`
   (the unqualified name for a FROM import, the dotted key for a qualified one).
   `qual` is the module name for a qualified import (so the lowerer can qualify
   the plain opaque-type names in the routine signature to their dotted keys), or
   NULL for a FROM import whose names stay unqualified. */
static void
emit_import(struct parser *p, struct node ***tail, const char *key,
            struct bmi_sym *s, const char *qual)
{
    struct node *n = mknode(p, N_IMPORT);

    n->name = (char *)key;
    n->sval = (char *)qual;
    n->imp = s;
    **tail = n;
    *tail = &n->next;
}

/* `FROM M IMPORT a, b, c` : load module M's interface and bring the listed
   names into scope unqualified. A CONST import folds to its value at parse time
   (like a local CONST); a TYPE, FN, or PROC import becomes an N_IMPORT node the
   lowerer registers (an opaque TYPE as a handle type, a routine as an external).
   Both this form and the qualified `IMPORT M` form support opaque handles. */
static void
parse_import(struct parser *p, struct node ***tail)
{
    char *mod;
    struct bmi *m;

    advance(p);   /* consume FROM */
    if (p->cur.kind != TK_IDENT)
        perr(p, "expected a module name after FROM");
    mod = ident_text(p);
    advance(p);
    expect_kw(p, KW_IMPORT, "expected IMPORT after the module name");

    m = load_module(p, mod);

    for (;;) {
        char *name;
        struct bmi_sym *s;

        if (p->cur.kind != TK_IDENT)
            perr(p, "expected an imported name");
        name = ident_text(p);
        s = bmi_find(m, name);
        if (!s)
            die("%s:%d: module '%s' does not export '%s'",
                p->filename, p->cur.line, mod, name);
        /* CONST folds now; a TYPE, FN, or PROC becomes an N_IMPORT the lowerer
           registers (an opaque TYPE as a handle type, a routine as an external).
           Handle signatures are supported here and in the qualified form. */
        if (s->kind == BMI_CONST)
            const_add(p, name, s->cval);
        else
            emit_import(p, tail, name, s, NULL);
        advance(p);
        if (p->cur.kind != TK_COMMA)
            break;
        advance(p);
    }
    if (!at_terminator(p))
        perr(p, "expected end of line after the import list");
}

/* `IMPORT M` (qualified) : load module M and bring all its exports into scope
   under dotted names (`M.name`). Every CONST, opaque TYPE, and supported FN/PROC
   is registered eagerly (opaque handles included); an unsupported one (a REF
   parameter) is left out and reported only if it is actually referenced (see
   parse_qualified_ref), so a module with an unsupported routine can still be
   imported for its other exports. An opaque type is used as `DIM x AS M.Type`. */
static void
parse_qualified_import(struct parser *p, struct node ***tail)
{
    char *mod;
    struct bmi *m;
    struct mod_ent *e;
    struct bmi_sym *s;

    advance(p);   /* consume IMPORT */
    if (p->cur.kind != TK_IDENT)
        perr(p, "expected a module name after IMPORT");
    mod = ident_text(p);
    advance(p);

    if (!mod_lookup(p, mod)) {
        m = load_module(p, mod);
        e = arena_zalloc(p->arena, sizeof(*e));
        e->name = mod;
        e->bmi = m;
        e->next = p->modules;
        p->modules = e;
        for (s = m->syms; s; s = s->next) {
            if (bmi_unsupported_reason(s))
                continue;   /* deferred; reported at a reference, not here */
            if (s->kind == BMI_CONST)
                const_add(p, qual_name(p, mod, s->name), s->cval);
            else
                emit_import(p, tail, qual_name(p, mod, s->name), s, mod);
        }
    }
    if (!at_terminator(p))
        perr(p, "expected end of line after IMPORT");
}

/* Resolve `M.member` after the module name has been consumed and the current
   token is '.'. Returns a folded N_INT for a CONST, or an N_CALL (keyed on the
   dotted name) for an FN/PROC. An unsupported or unknown member dies here. */
static struct node *
parse_qualified_ref(struct parser *p, struct mod_ent *m)
{
    char *member;
    struct bmi_sym *s;
    const char *why;
    struct node *n;

    advance(p);   /* consume '.' */
    if (p->cur.kind != TK_IDENT)
        perr(p, "expected a member name after 'Module.'");
    member = ident_text(p);
    advance(p);

    s = bmi_find(m->bmi, member);
    if (!s)
        die("%s:%d: module '%s' does not export '%s'",
            p->filename, p->cur.line, m->name, member);
    if (s->kind == BMI_TYPE)
        die("%s:%d: '%s.%s' is an opaque type, not a value",
            p->filename, p->cur.line, m->name, member);
    why = bmi_unsupported_reason(s);
    if (why)
        die("%s:%d: '%s.%s' %s",
            p->filename, p->cur.line, m->name, member, why);
    if (s->kind == BMI_CONST) {
        n = mknode(p, N_INT);
        n->ival = s->cval;
        return n;
    }
    if (p->cur.kind != TK_LPAREN)
        perr(p, "a call to a module routine needs '('");
    n = mknode(p, N_CALL);
    n->name = qual_name(p, m->name, member);
    n->a = parse_args(p);
    return n;
}

struct node *
parse_program(struct arena *a, const char *src, size_t len, const char *filename)
{
    struct parser p;
    struct node *prog;
    struct node **tail;

    p.arena = a;
    p.filename = filename;
    p.consts = NULL;
    p.modules = NULL;
    lexer_init(&p.lx, src, len);
    advance(&p);

    prog = arena_zalloc(a, sizeof(*prog));
    prog->kind = N_PROGRAM;
    tail = &prog->a;

    /* An optional leading MODULE statement makes the file a library; its name is
       carried on the program node (NULL for a program). It must come first. */
    while (p.cur.kind == TK_NEWLINE || p.cur.kind == TK_COLON)
        advance(&p);
    if (is_kw(&p, KW_MODULE)) {
        advance(&p);
        if (p.cur.kind != TK_IDENT)
            perr(&p, "expected a module name after MODULE");
        prog->name = ident_text(&p);
        advance(&p);
        if (!at_terminator(&p))
            perr(&p, "expected end of line after the module name");
    }

    while (p.cur.kind != TK_EOF) {
        int exported = 0;

        if (p.cur.kind == TK_NEWLINE || p.cur.kind == TK_COLON) {
            advance(&p);
            continue;
        }
        if (is_kw(&p, KW_MODULE))
            perr(&p, "MODULE must be the first statement");
        if (is_kw(&p, KW_FROM)) {
            parse_import(&p, &tail);
            continue;
        }
        if (is_kw(&p, KW_IMPORT)) {
            parse_qualified_import(&p, &tail);
            continue;
        }
        if (is_kw(&p, KW_EXPORT)) {
            advance(&p);
            exported = 1;
        }
        /* PROC / FN / DECLARE / TYPE are top-level definitions. */
        if (p.cur.kind == TK_KEYWORD &&
            (p.cur.keyword == KW_PROC || p.cur.keyword == KW_FN ||
             p.cur.keyword == KW_DECLARE || p.cur.keyword == KW_TYPE)) {
            struct node *d = p.cur.keyword == KW_PROC     ? parse_proc(&p)
                           : p.cur.keyword == KW_FN       ? parse_fn(&p)
                           : p.cur.keyword == KW_TYPE      ? parse_type(&p)
                           :                                 parse_declare(&p);
            d->is_export = exported;
            *tail = d;
            tail = &d->next;
            if (!at_terminator(&p))
                perr(&p, "expected end of definition");
            continue;
        }
        if (exported) {
            if (is_kw(&p, KW_CONST)) {
                parse_const(&p, 1, &tail);
                if (!at_terminator(&p))
                    perr(&p, "expected end of definition");
                continue;
            }
            perr(&p, "EXPORT must precede CONST, TYPE, PROC, FN, or DECLARE");
        }
        parse_statement(&p, &tail);
        if (!at_terminator(&p))
            perr(&p, "expected end of statement");
    }
    return prog;
}
