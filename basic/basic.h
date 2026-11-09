#ifndef BASIC_H
#define BASIC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ir.h"   /* struct ir_program, struct arena, slurp, die, util_* */

/* skj-bc: the BASIC front end (doc/languages/basic.md). The scanner produces
   Token values; the one-pass-friendly parser consumes them and builds an AST
   that lower.c turns into the shared IR. The lexer is ported from BASIDE's
   compiler/lexer.c, trimmed to the core reserved-word set (no display, game, or
   module words). */

/* ---- lexer ---- */

typedef enum {
    TK_EOF = 0,
    TK_NEWLINE,

    /* Literals and names. */
    TK_INT_LIT,
    TK_FLOAT_LIT,
    TK_STRING_LIT,
    TK_IDENT,

    /* Punctuation and operators. */
    TK_LPAREN, TK_RPAREN,
    TK_COMMA, TK_SEMI, TK_COLON,
    TK_DOT,
    TK_EQ,          /* '=' : assignment or equality by context */
    TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_CARET,

    TK_KEYWORD,

    TK_ERROR,       /* malformed token; message in the lexer */
} TokenKind;

/* The core reserved words (doc/languages/basic.md, Reserved words). REM is a
   comment to the scanner, so it is not a keyword id. Keep this enum in step with
   kw_names[] in lex.c. */
typedef enum {
    KW_ABS, KW_AND, KW_APPEND, KW_AS, KW_ASC, KW_AT, KW_ATN, KW_ATN2,
    KW_BAND, KW_BIN, KW_BNOT, KW_BOR, KW_BXOR, KW_BYTE,
    KW_CASE, KW_CEIL, KW_CHR, KW_CLAMP, KW_CLEAR, KW_CONST, KW_COS, KW_COSD, KW_COUNT,
    KW_DECLARE, KW_DIM, KW_DIV, KW_EACH, KW_ELSE, KW_ELSEIF, KW_END, KW_ENDCASE,
    KW_ENDENUM, KW_ENDFN, KW_ENDIF, KW_ENDPROC, KW_ENDTYPE, KW_ENDWHILE, KW_ENUM, KW_EXIT,
    KW_FIX, KW_FLOAT, KW_FLOOR, KW_FN, KW_FOR, KW_HEX, KW_IF, KW_IN, KW_INPUT,
    KW_INSTR, KW_INT, KW_LCASE, KW_LEFT, KW_LEN, KW_LERP, KW_LJUST,
    KW_MAX, KW_MID, KW_MIN, KW_MOD, KW_NEXT, KW_NOT, KW_OF, KW_OR, KW_PRINT, KW_PROC,
    KW_REF, KW_REMOVE, KW_REPEAT, KW_RETURN, KW_RIGHT, KW_RJUST, KW_RND, KW_ROUND,
    KW_SEED, KW_SGN, KW_SHL, KW_SHR, KW_SIN, KW_SIND, KW_SPACE, KW_SQR, KW_STEP, KW_STR,
    KW_STRING, KW_STRING_FN, KW_TAB, KW_TAN, KW_TAND, KW_THEN, KW_TIME, KW_TO, KW_TYPE,
    KW_UCASE, KW_UNTIL, KW_VAL, KW_VALINT, KW_VECTOR, KW_WAIT, KW_WHEN, KW_WHILE, KW_ZERO,

    /* module system (doc/languages/basic-modules.md); appended so the existing
       keyword ids keep their values. */
    KW_MODULE, KW_EXPORT, KW_IMPORT, KW_FROM, KW_EXTERNAL,

    KW_COUNT_
} Keyword;

typedef struct {
    TokenKind   kind;
    int         line;
    const char *lexeme;   /* points into the source buffer */
    size_t      length;
    int64_t     int_val;  /* TK_INT_LIT */
    double      float_val;/* TK_FLOAT_LIT */
    int         keyword;  /* TK_KEYWORD: a Keyword */
} Token;

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    int         line;
    const char *error;    /* set alongside a TK_ERROR token */
} Lexer;

void  lexer_init(Lexer *lx, const char *src, size_t len);
Token lexer_next(Lexer *lx);

/* Look up a reserved word, case-insensitively. Returns a Keyword or -1. */
int keyword_lookup(const char *s, size_t len);

/* The canonical spelling of a keyword, for diagnostics. */
const char *keyword_name(int kw);

/* ---- AST ---- */

/* The node kinds the Phase-1 bring-up accepts. The grammar in
   doc/languages/basic.md is wider; later phases add node kinds for the rest
   (control flow, PROC/FN, aggregates, FLOAT/STRING variables). */
enum node_kind {
    N_PROGRAM,    /* a = statement list (via ->next); name = module name when the
                     file is a MODULE, NULL for a program */
    N_CONST,      /* an EXPORTed CONST: name; ival = folded value. A non-exported
                     CONST leaves no node (it folds at use). */
    N_DIM,        /* name = variable; op = TY_DECL_*, a scalar declaration */
    N_ASSIGN,     /* name = target variable; a = value expression */
    N_PRINT,      /* a = item list; ival = 1 to suppress the trailing newline */
    N_PRINTITEM,  /* a = expression or N_STR; op = separator after it
                     (TK_SEMI, TK_COMMA, or 0 for the last item) */
    N_END,        /* halt the program */

    N_INT,        /* ival = integer literal value */
    N_FLT,        /* fval = float literal value */
    N_VAR,        /* name = variable reference */
    N_STR,        /* sval/slen = unescaped string literal bytes */
    N_BINOP,      /* op = a bop operator id; a, b. Covers arithmetic,
                     comparisons, bitwise, logical AND/OR, and '^' (power). */
    N_NEG,        /* a = operand of a unary minus */
    N_NOT,        /* a = operand of logical NOT (nonzero -> 0, zero -> 1) */
    N_BNOT,       /* a = operand of bitwise BNOT (x XOR -1) */

    /* ---- control flow ---- */
    N_IF,         /* a = condition; b = then body; c = else body (may be NULL,
                     or a single N_IF chaining an ELSEIF) */
    N_WHILE,      /* a = condition (tested before); b = body */
    N_REPEAT,     /* a = condition (tested after); b = body */
    N_FOR,        /* name = control var; a = start; b = limit; c = step (NULL =
                     1); d = body */
    N_FOREACH,    /* name = element var declared by the loop; a = N_VAR naming
                     the array walked; d = body */
    N_EXIT,       /* op = EXIT_* : leave the nearest enclosing loop of that kind */
    N_CASE,       /* a = selector; b = N_WHEN list; c = else body */
    N_WHEN,       /* a = N_WHENITEM list; b = body; chained by ->next */
    N_WHENITEM,   /* op = WI_* ; a = value / lo / relop operand; b = hi (range) */

    /* ---- procedures and functions ---- */
    N_PROC,       /* name; a = N_PARAM list; b = body; ival = 1 if forward only */
    N_FN,         /* name; a = N_PARAM list; b = body; op = return TY_DECL_*;
                     ival = 1 if forward only */
    N_PARAM,      /* name; op = TY_DECL_* */
    N_RETURN,     /* a = return expression, or NULL in a PROC */
    N_CALL,       /* name; a = argument expression list (chained by ->next) */
    N_IMPORT,     /* an imported FN/PROC from another module; imp = its bmi_sym.
                     Registered as an external routine; CONST imports fold at
                     parse time and leave no node. */

    /* ---- strings and I/O ---- */
    N_BUILTIN,    /* op = BF_* ; a = argument expression list (chained) */
    N_INPUT,      /* a = optional N_STR prompt; b = target N_VAR list (chained) */

    /* ---- records ---- */
    N_TYPE,       /* name; a = field list (N_PARAM nodes) */
    N_FIELD,      /* name = field; a = base designator (N_VAR record variable) */

    /* ---- timing ---- */
    N_WAIT        /* a = milliseconds expression */
};

/* Built-in function ids for an N_BUILTIN node. */
enum {
    BF_LEN, BF_ASC, BF_INSTR,          /* -> INT */
    BF_CHR, BF_STR,                    /* -> STRING */
    BF_LEFT, BF_RIGHT, BF_MID,
    BF_UCASE, BF_LCASE,
    /* core numeric built-ins */
    BF_ABS, BF_SGN,                    /* ABS keeps its argument's type; SGN -> INT */
    BF_MIN, BF_MAX, BF_CLAMP,          /* numeric; FLOAT if any argument is FLOAT */
    BF_INT, BF_FLOAT,                  /* conversions: INT -> INT, FLOAT -> FLOAT */
    BF_FLOOR, BF_CEIL, BF_ROUND,       /* FLOAT -> INT (truncated, 32-bit range) */
    BF_LERP,                           /* interpolation -> FLOAT */
    BF_SQR,                            /* square root -> FLOAT */
    BF_SIN, BF_COS, BF_TAN, BF_ATN,    /* trig (radians) -> FLOAT */
    BF_ATN2,                           /* two-argument arctangent -> FLOAT */
    BF_SIND, BF_COSD, BF_TAND,         /* trig in degrees -> FLOAT */
    BF_VAL, BF_VALINT,                 /* string -> FLOAT / INT */
    BF_SPACE, BF_STRING, BF_HEX, BF_BIN, /* -> STRING */
    BF_FIX, BF_ZERO, BF_RJUST, BF_LJUST, /* formatting -> STRING */
    BF_RND, BF_SEED, BF_TIME             /* RND/TIME -> FLOAT; SEED(n) -> INT */
};

/* N_EXIT op: which enclosing loop EXIT leaves. */
enum { EXIT_WHILE, EXIT_REPEAT, EXIT_FOR };

/* N_WHENITEM op: how the item matches the CASE selector. For WI_REL, the
   relation is carried in ival as a bop comparison id. */
enum { WI_VALUE, WI_RANGE, WI_REL };

/* Declared type for an N_DIM / N_PARAM / FN-return op field. TY_DECL_RECORD
   names a record type whose name is carried in the node's sval. */
enum { TY_DECL_INT, TY_DECL_FLOAT, TY_DECL_BYTE, TY_DECL_STRING, TY_DECL_RECORD };

/* AST operator ids for N_BINOP's op field. These have their own numbering,
   independent of TokenKind and Keyword, so the AST never conflates two lexer
   enum values that happen to share a number (e.g. TK_EQ and KW_BXOR). */
enum bop {
    BOP_ADD, BOP_SUB, BOP_MUL,
    BOP_DIV, BOP_MOD,
    BOP_BAND, BOP_BOR, BOP_BXOR, BOP_SHL, BOP_SHR,
    BOP_SLASH,   /* '/' : FLOAT division (promotes integer operands) */
    BOP_EQ, BOP_NE, BOP_LT, BOP_LE, BOP_GT, BOP_GE,
    BOP_AND, BOP_OR,
    BOP_POW
};

struct node {
    int           kind;
    int           line;
    int           is_export; /* N_PROC/N_FN/N_TYPE/N_CONST: part of a module's interface */
    int           op;      /* operator / separator, by kind */
    long          ival;    /* N_INT value; N_PRINT newline-suppress flag */
    double        fval;    /* N_FLT value */
    char         *name;    /* N_DIM/N_ASSIGN/N_VAR identifier */
    char         *sval;    /* N_STR bytes (unescaped) */
    int           slen;    /* N_STR length */
    struct bmi_sym *imp;   /* N_IMPORT: the imported routine's interface */
    char         *link;    /* N_PROC/N_FN: the C symbol of an EXTERNAL
                              declaration (bodyless, binds to a foreign routine),
                              or NULL for an ordinary routine */
    struct node  *a;
    struct node  *b;
    struct node  *c;       /* IF else body; FOR step; CASE else body */
    struct node  *d;       /* FOR body */
    struct node  *next;    /* sibling in a list */
};

/* ---- module interface (.bmi), doc/languages/basic-modules.md ----

   The reader turns a .bmi member back into this structure; the writer emits one
   from a module's AST. Both are in bmi.c. The value classes are primitives only:
   a BASIC INT is i64, BYTE is i8, FLOAT is f64, STRING is a BASIC string handle
   (str), and an opaque exported TYPE is a handle. */
enum bmi_class {
    BMI_VOID, BMI_I8, BMI_I16, BMI_I32, BMI_I64,
    BMI_F32, BMI_F64, BMI_STR, BMI_HANDLE
};
enum bmi_kind { BMI_CONST, BMI_TYPE, BMI_FUNC, BMI_PROC };

#define BMI_MAXPARAM 16

struct bmi_sym {
    int             kind;                 /* a bmi_kind */
    char           *name;                 /* the BASIC name */
    char           *link;                 /* object symbol (func/proc) */
    long            cval;                  /* BMI_CONST value */
    int             ret;                  /* BMI_FUNC return class */
    char           *ret_type;             /* opaque type name when ret==BMI_HANDLE */
    int             nparam;               /* BMI_FUNC/BMI_PROC */
    int             param[BMI_MAXPARAM];  /* parameter classes */
    char           *param_type[BMI_MAXPARAM]; /* opaque type name when the param
                                                 class is BMI_HANDLE, else NULL */
    int             param_ref[BMI_MAXPARAM]; /* 1 = passed by reference (a pointer) */
    struct bmi_sym *next;
};

struct bmi {
    char           *module;               /* the module's name */
    int             lp64;                 /* target pointer width */
    int             big_endian;
    struct bmi_sym *syms;                 /* in declaration order */
};

/* Write a module's interface, derived from its AST, as a .bmi member. */
void        bmi_write(FILE *out, struct node *prog);

/* Parse .bmi text into a struct bmi (arena-allocated), or die on a malformed
   member. name is for diagnostics. */
struct bmi *bmi_read(struct arena *a, const char *text, size_t len,
                     const char *name);

/* Read the reserved `.bmi` member from a GNU `ar` archive at `path` into an
   arena-copied, NUL-terminated buffer (*outlen excludes the NUL), or die if the
   file is not an archive or has no `.bmi` member. */
char *bmi_from_archive(struct arena *a, const char *path, size_t *outlen);

/* NULL if an imported symbol has a signature the current phase can lower (a
   CONST, or an FN/PROC over by-value primitives); otherwise a human-readable
   reason ("has a REF parameter, not yet supported", ...). An opaque handle
   (type, handle parameter, or handle return) and a by-reference parameter are
   the deferred cases. The returned string begins with a verb, to read after the
   routine's name. */
const char *bmi_unsupported_reason(struct bmi_sym *s);

/* ---- pipeline ---- */

/* Set the module import search directories (-I), consulted by parse_program to
   resolve a `FROM M IMPORT` against `<dir>/M.a`. The array is borrowed, not
   copied, so it must outlive the parse. */
void                basic_set_import_dirs(const char **dirs, int ndirs);

struct node        *parse_program(struct arena *a, const char *src, size_t len,
                                  const char *filename);
struct ir_program  *lower_program(struct arena *a, struct node *ast);

extern int basic_debug;

#endif /* BASIC_H */
