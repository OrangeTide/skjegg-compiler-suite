/* lex.c : the BASIC scanner (doc/languages/basic.md, Lexical elements).

   Ported from BASIDE's compiler/lexer.c and trimmed to the core reserved-word
   set: the display, console, controls, sound, card, asset, timer, matrix, and
   game-extension words are not reserved in skjegg's core BASIC. The scanning
   logic (numbers with radix prefixes and '_' separators, '""'-escaped strings,
   REM and "'" comments, the '$'-suffixed builtin names) is unchanged. */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "basic.h"

/* Canonical spellings, indexed by Keyword. Keep in step with the enum in
   basic.h and the reserved-word block in doc/languages/basic.md. The
   '$'-suffixed builtins carry the '$' in their spelling; scan_ident includes a
   trailing '$' in the lexeme, so the match is exact. */
static const char *const kw_names[KW_COUNT_] = {
    "ABS", "AND", "APPEND", "AS", "ASC", "AT", "ATN", "ATN2",
    "BAND", "BIN$", "BNOT", "BOR", "BXOR", "BYTE",
    "CASE", "CEIL", "CHR$", "CLAMP", "CLEAR", "CONST", "COS", "COSD", "COUNT",
    "DECLARE", "DIM", "DIV", "EACH", "ELSE", "ELSEIF", "END", "ENDCASE",
    "ENDENUM", "ENDFN", "ENDIF", "ENDPROC", "ENDTYPE", "ENDWHILE", "ENUM", "EXIT",
    "FIX$", "FLOAT", "FLOOR", "FN", "FOR", "HEX$", "IF", "IN", "INPUT",
    "INSTR", "INT", "LCASE$", "LEFT$", "LEN", "LERP", "LJUST$",
    "MAX", "MID$", "MIN", "MOD", "NEXT", "NOT", "OF", "OR", "PRINT", "PROC",
    "REF", "REMOVE", "REPEAT", "RETURN", "RIGHT$", "RJUST$", "RND", "ROUND",
    "SEED", "SGN", "SHL", "SHR", "SIN", "SIND", "SPACE$", "SQR", "STEP", "STR$",
    "STRING", "STRING$", "TAB", "TAN", "TAND", "THEN", "TIME", "TO", "TYPE",
    "UCASE$", "UNTIL", "VAL", "VALINT", "VECTOR", "WAIT", "WHEN", "WHILE", "ZERO$",
};

const char *
keyword_name(int kw)
{
    if (kw < 0 || kw >= KW_COUNT_ || !kw_names[kw])
        return "?";
    return kw_names[kw];
}

/* Case-insensitive compare of a length-delimited lexeme against a NUL string. */
static int
ci_equal(const char *s, size_t len, const char *word)
{
    for (size_t i = 0; i < len; i++) {
        if (!word[i] || toupper((unsigned char)s[i]) != (unsigned char)word[i])
            return 0;
    }
    return word[len] == '\0';
}

int
keyword_lookup(const char *s, size_t len)
{
    /* REM is handled as a comment by the scanner, so it is not in the table. */
    for (int i = 0; i < KW_COUNT_; i++)
        if (kw_names[i] && ci_equal(s, len, kw_names[i]))
            return i;
    return -1;
}

void
lexer_init(Lexer *lx, const char *src, size_t len)
{
    lx->src = src;
    lx->len = len;
    lx->pos = 0;
    lx->line = 1;
    lx->error = NULL;
}

static int
peek_ch(Lexer *lx)
{
    return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1;
}

static int
peek2(Lexer *lx)
{
    return lx->pos + 1 < lx->len ? (unsigned char)lx->src[lx->pos + 1] : -1;
}

static void
skip_to_eol(Lexer *lx)
{
    while (lx->pos < lx->len && lx->src[lx->pos] != '\n')
        lx->pos++;
}

/* A decimal digit, or an underscore separator. Underscores may sit between
   digits of a numeric literal and are ignored in the value. */
static int
digit_or_us(int c)
{
    return isdigit(c) || c == '_';
}

static Token
make(TokenKind k, int line)
{
    Token t = { 0 };
    t.kind = k;
    t.line = line;
    return t;
}

static Token
tok_error(Lexer *lx, const char *msg)
{
    lx->error = msg;
    return make(TK_ERROR, lx->line);
}

/* Scan a number: a decimal integer or float, or a C-style 0x hex or 0b binary
   integer. */
static Token
scan_number(Lexer *lx)
{
    int line = lx->line;
    size_t start = lx->pos;

    /* Radix-prefixed integers: 0x/0X hexadecimal, 0b/0B binary. */
    if (peek_ch(lx) == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X' ||
                               peek2(lx) == 'b' || peek2(lx) == 'B')) {
        int isbin = (peek2(lx) == 'b' || peek2(lx) == 'B');
        lx->pos += 2;
        uint64_t v = 0;
        int ndig = 0;
        if (isbin) {
            while (peek_ch(lx) == '0' || peek_ch(lx) == '1' ||
                   (peek_ch(lx) == '_' && ndig > 0)) {
                int c = lx->src[lx->pos++];
                if (c == '_') continue;
                v = v * 2 + (uint64_t)(c - '0');
                ndig++;
            }
        } else {
            while (isxdigit(peek_ch(lx)) || (peek_ch(lx) == '_' && ndig > 0)) {
                int c = lx->src[lx->pos++];
                if (c == '_') continue;
                int d = isdigit(c) ? c - '0' : (toupper(c) - 'A' + 10);
                v = v * 16 + (uint64_t)d;
                ndig++;
            }
        }
        if (ndig == 0)
            return tok_error(lx, isbin ? "malformed binary literal"
                                       : "malformed hex literal");
        Token t = make(TK_INT_LIT, line);
        t.int_val = (int64_t)v;
        t.lexeme = lx->src + start;
        t.length = lx->pos - start;
        return t;
    }

    while (digit_or_us(peek_ch(lx)))
        lx->pos++;

    int is_float = 0;
    if (peek_ch(lx) == '.' && isdigit(peek2(lx))) {
        is_float = 1;
        lx->pos++;
        while (digit_or_us(peek_ch(lx)))
            lx->pos++;
    } else if (peek_ch(lx) == '.') {
        /* trailing dot, e.g. "3." : a float with no fraction digits */
        is_float = 1;
        lx->pos++;
        while (digit_or_us(peek_ch(lx)))
            lx->pos++;
    }
    if (peek_ch(lx) == 'e' || peek_ch(lx) == 'E') {
        is_float = 1;
        lx->pos++;
        if (peek_ch(lx) == '+' || peek_ch(lx) == '-')
            lx->pos++;
        if (!isdigit(peek_ch(lx)))
            return tok_error(lx, "malformed float exponent");
        while (digit_or_us(peek_ch(lx)))
            lx->pos++;
    }

    Token t = make(is_float ? TK_FLOAT_LIT : TK_INT_LIT, line);
    t.lexeme = lx->src + start;
    t.length = lx->pos - start;
    /* Parse the value. Copy into a small buffer, dropping the '_' separators
       that neither strtoll nor strtod accepts. */
    char buf[64];
    size_t bn = 0;
    for (size_t i = 0; i < t.length && bn < sizeof buf - 1; i++) {
        char c = t.lexeme[i];
        if (c == '_') continue;
        buf[bn++] = c;
    }
    buf[bn] = '\0';
    if (is_float)
        t.float_val = strtod(buf, NULL);
    else
        t.int_val = (int64_t)strtoll(buf, NULL, 10);
    return t;
}

static Token
scan_ident(Lexer *lx)
{
    int line = lx->line;
    size_t start = lx->pos;
    while (isalnum(peek_ch(lx)) || peek_ch(lx) == '_')
        lx->pos++;
    if (peek_ch(lx) == '$')   /* trailing sigil for the $-suffixed builtins */
        lx->pos++;

    size_t len = lx->pos - start;
    const char *s = lx->src + start;

    /* REM starts a comment to end of line. */
    if (ci_equal(s, len, "REM")) {
        skip_to_eol(lx);
        /* Fall through to the newline on the next call. */
        return lexer_next(lx);
    }

    int kw = keyword_lookup(s, len);
    Token t = make(kw >= 0 ? TK_KEYWORD : TK_IDENT, line);
    t.lexeme = s;
    t.length = len;
    t.keyword = kw;
    return t;
}

static Token
scan_string(Lexer *lx)
{
    int line = lx->line;
    lx->pos++;                 /* opening quote */
    size_t start = lx->pos;
    for (;;) {
        int c = peek_ch(lx);
        if (c < 0 || c == '\n')
            return tok_error(lx, "unterminated string literal");
        if (c == '"') {
            if (peek2(lx) == '"') {   /* "" escapes one quote */
                lx->pos += 2;
                continue;
            }
            break;
        }
        lx->pos++;
    }
    Token t = make(TK_STRING_LIT, line);
    t.lexeme = lx->src + start;   /* raw content, "" not yet unescaped */
    t.length = lx->pos - start;
    lx->pos++;                    /* closing quote */
    return t;
}

Token
lexer_next(Lexer *lx)
{
    /* Skip spaces, tabs, carriage returns, and ' comments. */
    for (;;) {
        int c = peek_ch(lx);
        if (c == ' ' || c == '\t' || c == '\r') {
            lx->pos++;
        } else if (c == '\'') {
            skip_to_eol(lx);
        } else {
            break;
        }
    }

    int c = peek_ch(lx);
    if (c < 0)
        return make(TK_EOF, lx->line);

    if (c == '\n') {
        Token t = make(TK_NEWLINE, lx->line);
        lx->pos++;
        lx->line++;
        return t;
    }

    if (isdigit(c))
        return scan_number(lx);
    if (isalpha(c) || c == '_')
        return scan_ident(lx);
    if (c == '"')
        return scan_string(lx);

    int line = lx->line;
    lx->pos++;
    switch (c) {
    case '(': return make(TK_LPAREN, line);
    case ')': return make(TK_RPAREN, line);
    case ',': return make(TK_COMMA, line);
    case ';': return make(TK_SEMI, line);
    case ':': return make(TK_COLON, line);
    case '.': return make(TK_DOT, line);
    case '+': return make(TK_PLUS, line);
    case '-': return make(TK_MINUS, line);
    case '*': return make(TK_STAR, line);
    case '/': return make(TK_SLASH, line);
    case '^': return make(TK_CARET, line);
    case '=': return make(TK_EQ, line);
    case '<':
        if (peek_ch(lx) == '=') { lx->pos++; return make(TK_LE, line); }
        if (peek_ch(lx) == '>') { lx->pos++; return make(TK_NE, line); }
        return make(TK_LT, line);
    case '>':
        if (peek_ch(lx) == '=') { lx->pos++; return make(TK_GE, line); }
        return make(TK_GT, line);
    }
    return tok_error(lx, "unexpected character");
}
