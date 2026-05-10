/* tinc.h : TinC front-end declarations
 * Made by a machine.  PUBLIC DOMAIN (CC0-1.0)
 */

#ifndef TINC_H
#define TINC_H

#include "ir.h"

/****************************************************************
 * Lexer
 ****************************************************************/

enum tok {
    T_EOF = 0,

    /* keywords */
    T_INT, T_BYTE, T_DEFINE, T_IF, T_ELSE, T_WHILE,
    T_BREAK, T_CONTINUE, T_RETURN, T_FOREACH,

    /* literals and identifiers */
    T_IDENT, T_NUMBER, T_STRING, T_CHARLIT, T_UNDERSCORE,

    /* delimiters */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK,
    T_COMMA, T_SEMI, T_COLON, T_ARROW, T_DOTDOT,

    /* operators */
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_BANG,
    T_ANDAND, T_OROR,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_SHL, T_SHR,

    /* assignment */
    T_ASSIGN,
    T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_PERCENT_EQ,
    T_AMP_EQ, T_PIPE_EQ, T_CARET_EQ, T_SHL_EQ, T_SHR_EQ,
};

struct token {
    int kind;
    long nval;
    char *sval;
    int slen;
    int line;
};

void lex_init(struct arena *a, const char *src, const char *filename);
struct token lex_next(void);
struct token lex_peek(void);
struct token lex_peek2(void);

/****************************************************************
 * AST
 ****************************************************************/

/* return modes for N_FUNC */
enum ret_mode {
    RET_VOID,       /* no -> clause */
    RET_ONE,        /* -> int */
    RET_FIXED,      /* -> N  (N >= 2) */
    RET_VAR,        /* -> int.. */
    RET_VAR_MIN,    /* -> N..  (at least N values) */
};

/* element widths for arrays */
enum elem_type {
    ELEM_INT  = 4,
    ELEM_BYTE = 1,
};

enum node_kind {
    N_PROGRAM, N_FUNC, N_GLOBAL,
    N_BLOCK, N_IF, N_WHILE, N_FOREACH,
    N_BREAK, N_CONTINUE,
    N_RETURN,       /* fixed return: a = expr list (or NULL for void) */
    N_RETURN_PUSH,  /* return.. expr: a = expr */
    N_EXPR_STMT,

    /* expressions */
    N_BINOP, N_UNOP, N_ASSIGN, N_COMPOUND_ASSIGN,
    N_INDEX, N_SLICE, N_CALL, N_NAME,
    N_NUM, N_STR, N_CHARLIT,
    N_INIT_LIST,

    /* destructuring LHS: (a, _, rest..) = expr */
    N_DESTRUCT,
    N_DISCARD,      /* _ */
    N_REST,         /* name.. */

    /* parameter declaration */
    N_PARAM,
};

struct node {
    int kind;
    int op;
    int line;

    struct node *a, *b, *c;

    struct node *next;

    char *name;
    long ival;
    char *sval;
    int slen;

    int arr_size;       /* declared array size (-1 = inferred) */
    int elem;           /* ELEM_INT or ELEM_BYTE */
    int ret_mode;       /* RET_VOID / RET_ONE / RET_FIXED / RET_VAR / RET_VAR_MIN */
    int ret_count;      /* for RET_FIXED / RET_VAR_MIN: the count N */
    int is_view;        /* 1 if array view (alias or slice) */

    int slot;
};

struct node *parse_program(struct arena *a);

/****************************************************************
 * Lowering
 ****************************************************************/

struct ir_program *lower_program(struct arena *a, struct node *ast);

#endif /* TINC_H */
