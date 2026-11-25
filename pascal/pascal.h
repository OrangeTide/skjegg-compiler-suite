/* pascal.h : Pascal front-end declarations */

#ifndef PASCAL_H
#define PASCAL_H

#include "ir.h"

/****************************************************************
 * Lexer
 ****************************************************************/

enum tok {
    T_EOF = 0,
    T_PROGRAM, T_BEGIN, T_END,
    T_VAR, T_CONST, T_TYPE,
    T_PROCEDURE, T_FUNCTION, T_FORWARD,
    T_IF, T_THEN, T_ELSE,
    T_WHILE, T_DO,
    T_FOR, T_TO, T_DOWNTO,
    T_REPEAT, T_UNTIL,
    T_CASE, T_OF,
    T_WITH,
    T_BREAK, T_CONTINUE,
    T_AND, T_OR, T_NOT, T_IN,
    T_DIV, T_MOD, T_SET, T_SIZEOF,
    T_SHL, T_SHR,
    T_TRUE, T_FALSE,
    T_INTEGER, T_BOOLEAN, T_CHAR, T_BYTE, T_WORD,
    T_IDENT, T_NUMBER, T_REAL, T_STRING,
    T_LPAREN, T_RPAREN,
    T_LBRACK, T_RBRACK,
    T_COMMA, T_SEMI, T_COLON, T_DOT,
    T_DOTDOT,
    T_ASSIGN,
    T_PLUS, T_MINUS, T_STAR, T_SLASH,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_SYMDIFF,                  /* >< set symmetric difference (FPC) */
    T_CARET,
    T_AT,
    T_AND_THEN, T_OR_ELSE,
};

struct token {
    int kind;
    long nval;
    double fval;
    char *sval;
    char *raw;      /* identifier's original-case spelling (for external names) */
    int slen;
    int line;
    int off;        /* byte offset of the token's first character in the source */
};

void lex_init(struct arena *a, const char *src, const char *filename);
void lex_define(const char *name);
struct token lex_next(void);
struct token lex_peek(void);

/* The source buffer passed to lex_init, so the parser can capture a verbatim
   span (a unit's interface section) by byte offset. */
const char *lex_source(void);

/* Save and restore the whole lexer position so a nested parse (a used unit's
   interface text) can run over a separate buffer and then resume the main
   source exactly where it left off. */
struct lex_state {
    const char *src_buf;
    const char *src_file;
    const char *p;
    int line;
    int peeked;
    struct token peek_tok;
};
struct lex_state lex_save(void);
void lex_restore(struct lex_state s);
void lex_begin(const char *src, const char *filename);

extern int lex_align;
extern int lex_rangechecks;
extern int lex_overflowchecks;
extern int lex_iochecks;

/****************************************************************
 * AST
 ****************************************************************/

enum node_kind {
    N_PROGRAM,
    N_VARDECL, N_CONSTDECL,
    N_PROCDECL, N_FUNCDECL,
    N_PARAM,
    N_BLOCK, N_COMPOUND,
    N_IF, N_WHILE, N_FOR, N_REPEAT,
    N_BREAK, N_CONTINUE, N_CASE, N_CASEARM, N_CASELABEL, N_WITH,
    N_ASSIGN, N_CALL, N_EXPR_STMT,
    N_BINOP, N_UNOP,
    N_NAME, N_NUM, N_REAL, N_STR, N_BOOL,
    N_TYPEDEF, N_FIELDDECL,
    N_DOT, N_INDEX,
    N_DEREF,            /* p^ : pointer dereference */
    N_ADDR,             /* @x : address-of, yields a pointer */
    N_FMTARG,           /* write arg with a :width[:decimals] suffix */
    N_INITLIST,
    N_SET, N_SETRANGE,
    N_SIZEOF,
    N_VARIANTPART,
};

struct node {
    int kind;
    int op;
    int line;

    struct node *a, *b, *c, *d;
    struct node *next;

    char *name;
    char *raw_name;     /* original-case spelling of name (for external symbols) */
    long ival;
    double fval;
    char *sval;
    int slen;

    int is_forward;
    int is_external;
    int downto;
    int iocheck;        /* N_CALL: {$I±} state at the call site (1 = trap) */
    int is_unit;        /* N_PROGRAM: this compilation unit is a `unit`, not a
                           `program` (no main block, routines are exported) */
};

struct node *parse_program(struct arena *a);

/****************************************************************
 * Units (uses / unit)
 ****************************************************************/

/* Load unit `lname`'s interface-section source from `<dir>/<name>.a`, searching
   srcdir, then each -U dir, then ".".  In each directory the original-case name
   is tried before the lowercased one.  Returns the interface declaration text
   (arena-allocated, header line stripped), or dies if the unit is not found or
   its ABI tag does not match this target.  `lname` is the lowercased unit name,
   `rawname` its original-case spelling (either may be NULL to skip).  The
   header's interface-dependency unit names are written into deps[0..maxdeps),
   their count into *ndeps (original-case, as recorded by the producer). */
char *unit_load_interface(struct arena *a, const char *lname,
                          const char *rawname, const char *srcdir,
                          const char **updirs, int nupdirs,
                          char **deps, int maxdeps, int *ndeps);

/* Resolve prog's `uses` clause: for each used unit, load its interface source,
   re-parse it, and inject the declarations into prog's block so the program can
   reference the unit's consts, types, and routines.  No-op when prog has no
   `uses` clause.  Must run after parse_program and before lower_program. */
void parse_resolve_uses(struct arena *a, struct node *prog,
                        const char *srcdir,
                        const char **updirs, int nupdirs);

/****************************************************************
 * Lowering
 ****************************************************************/

struct ir_program *lower_program(struct arena *a, struct node *ast);

/* Debug info (-g): when pascal_debug is set, lowering emits an IR_LOC per
   statement and records pascal_src_file on each function for the line table.
   Set by the driver before lower_program. */
extern int pascal_debug;
extern const char *pascal_src_file;

#endif /* PASCAL_H */
