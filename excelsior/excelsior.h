/* excelsior.h : shared declarations for the Excelsior reader
 *
 * Excelsior is a statically-typed, class-based in-world scripting
 * language. This header covers the reader (lexer + parser + AST); see
 * grammar.ebnf for the surface and core.md for the design.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#ifndef EXCELSIOR_H
#define EXCELSIOR_H

#include "arena.h"
#include "util.h"

/***** Lexer *****/

enum tok {
    T_EOF = 0,
    T_NL,                 /* statement-terminating newline */
    /***** section / definition keywords *****/
    T_USE, T_CONST, T_TYPE,
    T_CLASS, T_ENDCLASS, T_ENUM, T_RECORD, T_ENDRECORD,
    T_SHAPE, T_ENDSHAPE,
    T_INTERFACE, T_ENDINTERFACE,  /* a named object slice (interface-decl.md) */
    T_SUPPORTS,               /* class-body directive (interface-support.md) */
    T_PUBLIC, T_PRIVATE, T_TUNABLE, T_DISCLOSES,
    T_VERB, T_ENDVERB, T_FUNC, T_ENDFUNC,
    T_MACRO, T_ENDMACRO,      /* meta-layer macro definition (meta.md) */
    /***** statement keywords *****/
    T_IF, T_ELSEIF, T_ELSE, T_ENDIF,
    T_OTHERWISE,              /* the value fallback operator (fallback-words.md) */
    T_FOR, T_IN, T_ENDFOR, T_WHILE, T_ENDWHILE,
    T_MATCH, T_ENDMATCH,
    T_WHEN,                   /* opens a match arm (match-when.md D1) */
    T_VAR, T_RETURN, T_BREAK, T_CONTINUE, T_TRACE,
    /***** expression keywords *****/
    T_QUOTE, T_QUASI, T_SELF, T_IS, T_AS,
    T_AND, T_OR, T_XOR, T_NOT,
    T_TRUE, T_FALSE, T_NIL, T_NOTHING,
    T_SELECT, T_FROM,         /* `select idx from a, b, c otherwise d` */
    /***** type keywords *****/
    T_TINT, T_TFLOAT, T_TDEC, T_TVEC, T_TMAT,
    T_TSTR, T_TOBJ, T_TBOOL, T_TERR, T_TLIST, T_TPROP, T_TMAYBE,
    T_TSET, T_OF,             /* `set of E` flag bitmask (set-of.md) */
    T_EMPTY, T_FULL, T_OVERLAPS,
    T_SHARED,                 /* by-reference parameter mode (shared-params.md) */
    /***** literals *****/
    T_IDENT, T_NUMBER, T_FLOAT_LIT, T_STRING,
    T_ISTR_PIECE,             /* string literal chunk before a ${ hole */
    /***** operators *****/
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_ASSIGN,
    T_TO, T_THEN, T_DO, T_RETURNS, T_CAN, T_FAIL, T_WITH,
    T_YIELD,                  /* source body: produce and suspend (sources.md) */
    T_DEFER,                  /* block-scoped teardown (defer.md) */
    T_TAGS,                   /* record field annotations (record-annotations.md) */
    /***** punctuation *****/
    T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK,
    T_DOT, T_COLON, T_COMMA,
    T_HOLE, T_HOLE_END,       /* ${ ... } in a data literal */
    /***** special *****/
    T_TRACE_COMMENT,
};

struct token {
    int kind;
    long nval;                /* integer literal */
    double fval;              /* decimal literal (T_FLOAT_LIT) */
    char *sval;
    int slen;
    int line;
};

void lex_init(struct arena *a, const char *src, const char *filename,
              int trace_on);
struct token lex_next(void);
struct token lex_peek(void);
struct token lex_peek2(void);
const char *tok_str(int kind);
const char *lex_filename(void);
int lex_trace_on(void);

/* The condition region (then-in-if.md D3): between a head keyword and its
 * `then` / `do` a newline never ends the statement, exactly as inside `( )`,
 * so a compound condition wraps with no continuation trick. */
void lex_cond_begin(void);
void lex_cond_end(void);

/* A func literal's body has newline-separated statements, but a lambda is
 * often written inside a call's `( )`, where newlines are suppressed. These
 * reset the bracket depth to zero for the body (so its newlines flow) and
 * restore it after `endfunc` (function-values.md). lex_body_begin returns the
 * saved depth for lex_body_end; the pair nests for a lambda in a lambda. */
int lex_body_begin(void);
void lex_body_end(int saved);

/* A saved lexer position, so the parser can scan ahead and rewind. Used
 * once, to tell an if-expression condition from a closing `then`
 * (then-in-if.md D5). The fields are the lexer's whole state; nothing
 * outside lex.c should read them. */
struct lex_state {
    const char *p;
    int line, peeked, peeked2, last_kind;
    int paren_depth, cond_depth, holes_open;
    struct token peek_tok, peek2_tok;
    char hole_kind[16];
};
void lex_save(struct lex_state *s);
void lex_restore(const struct lex_state *s);

/***** Types *****/

struct node;
struct sym;
struct scope;

/* internal ex_type kinds used by the type checker, above the token range:
 * ET_ANY suppresses cascading errors on unknown / external values,
 * ET_NIL is the type of the `nil` literal, ET_MAYBE wraps its inner
 * type as "a T or nothing" (fallible.md; the null-word representation
 * lives in lowering), and ET_DEC is an untyped decimal constant (a
 * literal that has not yet resolved to decimal or float; numbers.md). */
enum { ET_ANY = 900, ET_NIL, ET_MAYBE, ET_DEC, ET_SIGNAL, ET_TYPEHOLE,
       ET_SLICE, ET_SOURCE, ET_FUNC };
/* ET_FUNC is a function value's type (function-values.md D1/D3): a code
 * pointer at rest. `inner` is the return type (NULL for a valueless func),
 * and `verbs` reuses its node-list slot to point at the declaring node's
 * N_PARAM list, so the parameter types are read from there. */
/* ET_SOURCE is `source of T` (sources.md D4): a failable-continuation
 * source, a suspended body each pump resumes. Second-class (D5): a local
 * or func parameter, never a field, never sent. Its ->inner is T. */
/* ET_SLICE is an object slice, `obj with (open, close)` (object-slices.md): a
 * structural verb subset. `verbs` is the N_NAME list of permitted verbs; the
 * value at rest is a plain object handle, so it lowers exactly like `obj`. */
/* ET_TYPEHOLE is a `${T}` in a type position inside a macro body: the hole
 * expression rides in `hole`, and form_build replaces the whole type with the
 * meta type value it evaluates to (typed-macros.md D4). It never survives
 * expansion, so nothing downstream sees one. */
/* ET_SIGNAL is the type of a `can fail` call: a valueless success/failure
 * signal, not a value (can-fail.md, R14). It is consumed by `if`/`while`
 * and a bare statement; using it as a value is a teaching error. */

struct ex_type {
    int kind;                 /* a T_Txxx type keyword, or T_IDENT for named */
    struct ex_type *inner;    /* list<inner> */
    char *name;               /* named type (class / enum) */
    struct sym *sym;          /* resolved declaration, or NULL (external) */
    struct node *hole;        /* ET_TYPEHOLE: the `${T}` expression */
    struct node *verbs;       /* ET_SLICE: the permitted verb names */
};

/***** AST *****/

enum node_kind {
    N_FILE,
    /* top-level */
    N_USE, N_CONST, N_CLASS, N_ENUM, N_RECORD, N_MACRO,
    N_SHAPE,                  /* a data-literal schema (typed-data.md) */
    N_INTERFACE,              /* a named interface: `Name is obj with (...)` */
    N_SHAPEKIND,              /* one node-kind or group line in a shape */
    N_SHAPESLOT,              /* one slot in a node-kind, or one group alt */
    /* class members */
    N_FIELD, N_VERB, N_FUNC, N_PARAM, N_DISCLOSE,
    N_SUPPORTS,               /* `supports Name`: an interface the class claims */
    /* statements */
    N_VAR, N_ASSIGN, N_IF, N_FOR, N_WHILE, N_MATCH, N_MATCHARM,
    N_RETURN, N_BREAK, N_CONTINUE, N_TRACE, N_TRACE_CMT, N_EXPR_STMT,
    N_ONFAIL,                 /* `stmt on fail handler` (fallible-consumers.md) */
    /* expressions */
    N_BINOP, N_UNOP, N_FIELD_ACC, N_SEND, N_CALL,
    N_CMPCHAIN,               /* a < b < c: a, then N_CMPLINK list */
    N_CMPLINK,                /* one link: op + right operand in ->a */
    N_INDEX, N_SLICE, N_CAST, N_ISTEST, N_RANGE,
    N_WITH,                   /* `p with (x, y)`: a record field slice */
    N_TAG,                    /* one `symbol atom...` field annotation entry */
    N_NAME, N_NUM, N_FLOAT, N_STR, N_BOOL, N_NIL, N_SELF,
    N_TYPELIT,                /* a builtin type named in a macro body */
    N_NOTHING,                /* the maybe absence literal */
    N_EMPTY, N_FULL,          /* set identity literals (set-of.md) */
    N_FAIL,                   /* fail statement */
    N_YIELD,                  /* yield statement in a source body (sources.md) */
    N_DEFER,                  /* `defer STMT` block-scoped teardown (defer.md) */
    N_TOSTR,                  /* stringify an interpolation hole */
    N_THENELSE,               /* `cond then A else B` if-expression */
    N_SELECT,                 /* `select idx from a, b, c otherwise d` */
    N_MATCHEXPR,               /* `match E` in expression position */
    N_OTHERWISE,              /* `A otherwise B` value fallback operator */
    N_DATALIT, N_DATAITEM, N_QUOTE,
    N_COMP,                   /* list comprehension `[e for x in xs if c]`
                               * (function-values.md D5): a=head, b=source,
                               * c=cond (or NULL); name/sym the binder */
    N_FUNCLIT,                /* anonymous func value `func(p) returns T ...
                               * endfunc` (function-values.md D1): a=params,
                               * b=body, type=return; name is a generated
                               * label assigned at lower time */
    N_QUASI,                  /* quasi FORM: a template with ${} splices (meta.md) */
    N_HOLE,                   /* ${expr} element of a data literal / quasi splice */
};

/* node flags */
enum {
    NF_PUBLIC  = 1 << 0,      /* member is under a `public` section */
    NF_TUNABLE = 1 << 1,      /* field is `tunable` */
    NF_CANFAIL = 1 << 2,      /* func declared `can fail` (valueless) */
    NF_FALLIBLE = 1 << 3,     /* expression node whose result may fail */
    NF_SHAPE_GROUP = 1 << 4,  /* a shape line is a group (alternation) */
    NF_SHARED = 1 << 5,       /* a by-reference `shared` parameter (shared-params.md) */
    NF_NOSIG = 1 << 6,        /* a verb written with no signature (interface-support.md D4) */
};

struct node {
    int kind;
    int op;                   /* operator token (binop / unop / assign) */
    int line;
    int flags;
    struct node *a, *b, *c;   /* children, meaning per kind */
    struct node *next;        /* sibling in a list */
    char *name;               /* identifier / verb selector / dotted path */
    char *alias;              /* use-as alias; class parent name */
    long ival;                /* integer / bool literal; a decimal
                                 literal's scaled value once resolved */
    double fval;              /* decimal / float literal */
    char *sval;               /* string literal */
    int slen;
    struct ex_type *type;     /* declared type / return type / cast target */
    struct sym *sym;          /* resolved declaration, or NULL (external) */
};

struct node *parse_program(struct arena *a);

/***** Symbol table / name resolution *****/

enum sym_kind {
    SYM_CLASS, SYM_ENUM, SYM_RECORD, SYM_SHAPE, SYM_INTERFACE, SYM_ENUM_MEMBER,
    SYM_CONST, SYM_FIELD, SYM_VERB, SYM_FUNC, SYM_MACRO,
    SYM_PARAM, SYM_LOCAL, SYM_MODULE, SYM_IMPORT,
};

struct sym {
    int kind;
    char *name;               /* first-seen display spelling */
    char *key;                /* case-folded lookup key */
    struct node *decl;        /* declaring node (NULL if external) */
    struct ex_type *type;     /* declared type, when known */
    struct scope *members;    /* class / enum / record member scope */
    struct sym *owner;        /* for a member: the class that declares it */
    struct sym *base;         /* for a class: its parent class, or NULL */
    struct sym *next;         /* next symbol in the owning scope */
};

const char *sym_kind_name(int kind);
void resolve_program(struct arena *a, struct node *file);
void dump_symbols(struct node *file);
int ex_ci_eq(const char *a, const char *b);          /* ASCII case-fold compare */
struct sym *sym_member(struct sym *owner, const char *name);
struct sym *module_sym(const char *name);

/***** Type checking *****/

void typecheck_program(struct arena *a, struct node *file);

/***** IR lowering *****/

struct ir_program;
struct ir_program *lower_program(struct arena *a, struct node *file);

#endif /* EXCELSIOR_H */
