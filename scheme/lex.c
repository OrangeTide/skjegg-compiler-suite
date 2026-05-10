/* lex.c : S-expression lexer for TinScheme */

#include "scheme.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static struct arena *lex_arena;
static const char *src_buf;
static const char *src_file;
static const char *p;
static int line;
static int peeked;
static struct token peek_tok;

void
lex_init(struct arena *a, const char *src, const char *filename)
{
    lex_arena = a;
    src_buf = src;
    src_file = filename ? filename : "<input>";
    p = src;
    line = 1;
    peeked = 0;
}

static void
skip_ws(void)
{
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (*p == '\n') {
            line++;
            p++;
            continue;
        }
        if (*p == ';') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        break;
    }
}

static int
is_sym_char(int c)
{
    if (isalnum(c))
        return 1;
    switch (c) {
    case '!': case '$': case '%': case '&': case '*':
    case '+': case '-': case '.': case '/': case ':':
    case '<': case '=': case '>': case '?': case '_':
    case '~': case '^':
        return 1;
    }
    return 0;
}

static struct token
make(int k)
{
    struct token t;

    t.kind = k;
    t.nval = 0;
    t.sval = NULL;
    t.slen = 0;
    t.line = line;
    return t;
}

static struct token
lex_one(void)
{
    struct token t;
    const char *start;
    int c;
    long n, sign;
    char *buf;
    size_t cap, len;

    skip_ws();
    t = make(TOK_EOF);
    if (*p == '\0')
        return t;

    c = (unsigned char)*p;

    switch (c) {
    case '(':
        p++;
        return make(TOK_LPAREN);
    case ')':
        p++;
        return make(TOK_RPAREN);
    case '\'':
        p++;
        return make(TOK_QUOTE);
    case '#':
        p++;
        if (*p == 't') {
            p++;
            if (is_sym_char((unsigned char)*p))
                die("%s:%d: bad # literal", src_file, line);
            return make(TOK_TRUE);
        }
        if (*p == 'f') {
            p++;
            if (is_sym_char((unsigned char)*p))
                die("%s:%d: bad # literal", src_file, line);
            return make(TOK_FALSE);
        }
        die("%s:%d: unknown # literal", src_file, line);
        break;
    }

    if (c == '"') {
        p++;
        cap = 16;
        len = 0;
        buf = xmalloc(cap);
        while (*p && *p != '"') {
            int ch;
            if (*p == '\\') {
                p++;
                switch (*p) {
                case 'n':  ch = '\n'; break;
                case 't':  ch = '\t'; break;
                case 'r':  ch = '\r'; break;
                case '\\': ch = '\\'; break;
                case '"':  ch = '"';  break;
                case '0':  ch = '\0'; break;
                default:
                    die("%s:%d: unknown escape \\%c",
                        src_file, line, *p);
                    ch = 0;
                }
                p++;
            } else if (*p == '\n') {
                die("%s:%d: newline in string",
                    src_file, line);
                ch = 0;
            } else {
                ch = (unsigned char)*p++;
            }
            if (len + 1 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
                if (!buf)
                    die("oom");
            }
            buf[len++] = (char)ch;
        }
        if (*p != '"')
            die("%s:%d: unterminated string", src_file, line);
        p++;
        buf[len] = '\0';
        t = make(TOK_STR);
        t.sval = arena_strndup(lex_arena, buf, len);
        t.slen = (int)len;
        free(buf);
        return t;
    }

    /* numbers: optional sign followed by digits */
    if (isdigit(c) ||
        ((c == '-' || c == '+') && isdigit((unsigned char)p[1]))) {
        sign = 1;
        if (c == '-') {
            sign = -1;
            p++;
        } else if (c == '+') {
            p++;
        }
        n = 0;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
            if (!isxdigit((unsigned char)*p))
                die("%s:%d: bad hex literal",
                    src_file, line);
            while (isxdigit((unsigned char)*p)) {
                int d;
                c = (unsigned char)*p;
                if (c >= '0' && c <= '9')
                    d = c - '0';
                else if (c >= 'a' && c <= 'f')
                    d = 10 + c - 'a';
                else
                    d = 10 + c - 'A';
                n = (n << 4) | d;
                p++;
            }
        } else {
            while (isdigit((unsigned char)*p)) {
                n = n * 10 + (*p - '0');
                p++;
            }
        }
        t = make(TOK_NUM);
        t.nval = sign * n;
        return t;
    }

    /* dot: only as a bare delimiter, not part of a symbol */
    if (c == '.' && !is_sym_char((unsigned char)p[1])) {
        p++;
        return make(TOK_DOT);
    }

    /* symbol */
    if (is_sym_char(c)) {
        start = p;
        while (is_sym_char((unsigned char)*p))
            p++;
        t = make(TOK_SYM);
        t.sval = arena_strndup(lex_arena, start, (size_t)(p - start));
        return t;
    }

    die("%s:%d: unexpected character 0x%02x", src_file, line, c);
    return t;
}

struct token
lex_next(void)
{
    if (peeked) {
        peeked = 0;
        return peek_tok;
    }
    return lex_one();
}

struct token
lex_peek(void)
{
    if (!peeked) {
        peek_tok = lex_one();
        peeked = 1;
    }
    return peek_tok;
}
