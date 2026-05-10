/* lex.c : GAS syntax tokenizer for ColdFire assembler */

#include "as.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void
lex_init(struct lexer *l, const char *src)
{
    l->src = src;
    l->pos = src;
    l->line = 1;
    l->str_buf = NULL;
    l->str_cap = 0;
    l->tok.type = T_NEWLINE;
}

static void
str_buf_push(struct lexer *l, int *len, char c)
{
    if (*len >= l->str_cap) {
        l->str_cap = l->str_cap ? l->str_cap * 2 : 64;
        l->str_buf = realloc(l->str_buf, l->str_cap);
    }
    l->str_buf[(*len)++] = c;
}

static void
skip_line_comment(struct lexer *l)
{
    while (*l->pos && *l->pos != '\n')
        l->pos++;
}

static void
lex_string(struct lexer *l)
{
    int len = 0;

    l->pos++;
    while (*l->pos && *l->pos != '"') {
        if (*l->pos == '\\') {
            l->pos++;
            switch (*l->pos) {
            case 'n':
                str_buf_push(l, &len, '\n');
                l->pos++;
                break;
            case 't':
                str_buf_push(l, &len, '\t');
                l->pos++;
                break;
            case 'r':
                str_buf_push(l, &len, '\r');
                l->pos++;
                break;
            case '\\':
                str_buf_push(l, &len, '\\');
                l->pos++;
                break;
            case '"':
                str_buf_push(l, &len, '"');
                l->pos++;
                break;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                int val = 0;
                int k;
                for (k = 0; k < 3 && *l->pos >= '0' && *l->pos <= '7'; k++) {
                    val = val * 8 + (*l->pos - '0');
                    l->pos++;
                }
                str_buf_push(l, &len, (char)val);
                break;
            }
            default:
                str_buf_push(l, &len, *l->pos);
                l->pos++;
                break;
            }
        } else {
            str_buf_push(l, &len, *l->pos);
            l->pos++;
        }
    }
    if (*l->pos == '"')
        l->pos++;
    str_buf_push(l, &len, '\0');
    l->tok.type = T_STRING;
    l->tok.str = l->str_buf;
    l->tok.str_len = len - 1;
}

static int
is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static void
lex_ident(struct lexer *l, int dot_prefix)
{
    const char *start;
    int len;

    if (dot_prefix)
        start = l->pos - 1;
    else
        start = l->pos;

    while (is_ident_char(*l->pos) || *l->pos == '.')
        l->pos++;

    len = (int)(l->pos - start);
    l->tok.type = dot_prefix ? T_DOT_IDENT : T_IDENT;
    l->tok.str = arena_strndup(l->arena, start, len);
    l->tok.str_len = len;
}

static void
lex_number(struct lexer *l)
{
    char *end;

    l->tok.type = T_INT;
    if (l->pos[0] == '0' && (l->pos[1] == 'x' || l->pos[1] == 'X'))
        l->tok.ival = strtol(l->pos, &end, 16);
    else
        l->tok.ival = strtol(l->pos, &end, 10);
    l->pos = end;
}

void
lex_next(struct lexer *l)
{
    for (;;) {
        while (*l->pos == ' ' || *l->pos == '\t')
            l->pos++;

        l->tok.line = l->line;

        if (*l->pos == '\0') {
            l->tok.type = T_EOF;
            return;
        }

        if (*l->pos == '\n') {
            l->pos++;
            l->line++;
            l->tok.type = T_NEWLINE;
            return;
        }

        if (*l->pos == '|') {
            skip_line_comment(l);
            continue;
        }

        if (*l->pos == '"') {
            lex_string(l);
            return;
        }

        if (*l->pos == '.') {
            l->pos++;
            if (is_ident_char(*l->pos)) {
                lex_ident(l, 1);
                return;
            }
            l->tok.type = T_DOT_IDENT;
            l->tok.str = ".";
            l->tok.str_len = 1;
            return;
        }

        if (isalpha((unsigned char)*l->pos) || *l->pos == '_') {
            lex_ident(l, 0);
            return;
        }

        if (isdigit((unsigned char)*l->pos)) {
            const char *start = l->pos;
            lex_number(l);
            if ((*l->pos == 'b' || *l->pos == 'f') &&
                !is_ident_char(l->pos[1])) {
                l->pos = start;
                lex_ident(l, 0);
                return;
            }
            return;
        }

        switch (*l->pos) {
        case '#': l->pos++; l->tok.type = T_HASH; return;
        case ',': l->pos++; l->tok.type = T_COMMA; return;
        case ':': l->pos++; l->tok.type = T_COLON; return;
        case '(': l->pos++; l->tok.type = T_LPAREN; return;
        case ')': l->pos++; l->tok.type = T_RPAREN; return;
        case '-': l->pos++; l->tok.type = T_MINUS; return;
        case '+': l->pos++; l->tok.type = T_PLUS; return;
        case '%':
            l->pos++;
            lex_ident(l, 0);
            return;
        default:
            l->pos++;
            continue;
        }
    }
}
