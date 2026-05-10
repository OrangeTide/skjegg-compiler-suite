/* scheme.h : TinScheme front-end header */

#ifndef SCHEME_H
#define SCHEME_H

#include "gc.h"
#include "ir.h"

/****************************************************************
 * Diagnostics (from ir/util.c)
 ****************************************************************/

/* die(), warn(), xmalloc(), xstrdup() are in ir/util.c */

/****************************************************************
 * Tokens
 ****************************************************************/

enum {
    TOK_EOF,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_DOT,
    TOK_QUOTE,
    TOK_NUM,
    TOK_STR,
    TOK_SYM,
    TOK_TRUE,
    TOK_FALSE,
};

struct token {
    int kind;
    long nval;
    char *sval;
    int slen;
    int line;
};

/****************************************************************
 * Lexer
 ****************************************************************/

void lex_init(struct arena *a, const char *src, const char *filename);
struct token lex_next(void);
struct token lex_peek(void);

/****************************************************************
 * Parser
 ****************************************************************/

val_t scm_read(struct gc_heap *h);
val_t scm_read_all(struct gc_heap *h);

/****************************************************************
 * Printer (for debugging)
 ****************************************************************/

void scm_print(val_t v);
void scm_println(val_t v);

/****************************************************************
 * Lowering
 ****************************************************************/

struct ir_program *scm_lower(struct arena *a, struct gc_heap *h, val_t program);

#endif /* SCHEME_H */
