/* lex.c : hand-written lexer for Excelsior.
 *
 * Single buffer, two-token lookahead. Emits T_NL as a statement
 * terminator, suppressed when a bracket/paren is open, after a token
 * that cannot end a statement (a binary operator, a comma, etc.),
 * or across an explicit trailing "\". Consecutive newlines fold.
 * Keywords are matched case-insensitively (ASCII fold).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "excelsior.h"

static struct arena *lex_arena;
static const char *src_file;
static const char *p;
static int line;
static int trace_on;
static int peeked;
static struct token peek2_tok;
static int peeked2;
static struct token peek_tok;
static int last_kind;         /* kind of the last token produced */
static int paren_depth;       /* open ( and [ */
static int cond_depth;        /* open condition regions (then-in-if.md D3) */
static int holes_open;        /* open ${ holes (index into hole_kind) */
static char hole_kind[16];    /* 's': resumes a string; 'd': a data hole */

struct kw { const char *s; int k; };
static const struct kw keywords[] = {
    { "use",       T_USE },
    { "const",     T_CONST },
    { "type",      T_TYPE },
    { "class",     T_CLASS },
    { "endclass",  T_ENDCLASS },
    { "enum",      T_ENUM },
    { "record",    T_RECORD },
    { "endrecord", T_ENDRECORD },
    { "shape",     T_SHAPE },
    { "endshape",  T_ENDSHAPE },
    { "interface", T_INTERFACE },
    { "supports",  T_SUPPORTS },
    { "endinterface", T_ENDINTERFACE },
    { "public",    T_PUBLIC },
    { "private",   T_PRIVATE },
    { "tunable",   T_TUNABLE },
    { "discloses", T_DISCLOSES },
    { "verb",      T_VERB },
    { "endverb",   T_ENDVERB },
    { "func",      T_FUNC },
    { "endfunc",   T_ENDFUNC },
    { "if",        T_IF },
    { "elseif",    T_ELSEIF },
    { "else",      T_ELSE },
    { "otherwise", T_OTHERWISE },
    { "endif",     T_ENDIF },
    { "for",       T_FOR },
    { "in",        T_IN },
    { "endfor",    T_ENDFOR },
    { "while",     T_WHILE },
    { "endwhile",  T_ENDWHILE },
    { "match",     T_MATCH },
    { "when",      T_WHEN },
    { "then",      T_THEN },
    { "do",        T_DO },
    { "endmatch",  T_ENDMATCH },
    { "to",        T_TO },
    { "with",      T_WITH },
    { "returns",   T_RETURNS },
    { "select",    T_SELECT },
    { "from",      T_FROM },
    { "var",       T_VAR },
    { "return",    T_RETURN },
    { "break",     T_BREAK },
    { "continue",  T_CONTINUE },
    { "trace",     T_TRACE },
    { "quote",     T_QUOTE },
    { "quasi",     T_QUASI },
    { "macro",     T_MACRO },
    { "endmacro",  T_ENDMACRO },
    { "tags",      T_TAGS },
    { "overlaps",  T_OVERLAPS },
    { "shared",    T_SHARED },
    { "yield",     T_YIELD },
    { "defer",     T_DEFER },
    { "self",      T_SELF },
    { "is",        T_IS },
    { "as",        T_AS },
    { "and",       T_AND },
    { "or",        T_OR },
    { "xor",       T_XOR },
    { "not",       T_NOT },
    { "true",      T_TRUE },
    { "false",     T_FALSE },
    { "nil",       T_NIL },
    { "nothing",   T_NOTHING },
    { "maybe",     T_TMAYBE },
    { "can",       T_CAN },
    { "fail",      T_FAIL },
    { "int",       T_TINT },
    { "float",     T_TFLOAT },
    { "decimal",   T_TDEC },
    { "vec",       T_TVEC },
    { "mat",       T_TMAT },
    { "str",       T_TSTR },
    { "obj",       T_TOBJ },
    { "bool",      T_TBOOL },
    { "err",       T_TERR },
    { "list",      T_TLIST },
    { "prop",      T_TPROP },
    { NULL, 0 }
};

void
lex_init(struct arena *a, const char *src, const char *filename, int trace)
{
    lex_arena = a;
    src_file = filename ? filename : "<input>";
    p = src;
    line = 1;
    trace_on = trace;
    peeked = 0;
    peeked2 = 0;
    last_kind = T_NL;         /* suppress a leading newline */
    paren_depth = 0;
    cond_depth = 0;
    holes_open = 0;
}

/* Open a condition region: newlines inside it are layout, not statement
 * ends. A newline already sitting in the peek buffer belongs to the region
 * too (it was read before the head keyword's operand), so drop it. */
void
lex_cond_begin(void)
{
    cond_depth++;
    if (peeked && peek_tok.kind == T_NL) {
        if (peeked2) {
            peek_tok = peek2_tok;
            peeked2 = 0;
        } else {
            peeked = 0;
        }
    }
}

void
lex_cond_end(void)
{
    if (cond_depth > 0)
        cond_depth--;
}

int
lex_body_begin(void)
{
    int saved = paren_depth;
    paren_depth = 0;                    /* the body's newlines flow again */
    if (peeked && peek_tok.kind == T_NL) {
        /* a newline already read under the old depth belongs to the body */
        if (peeked2) {
            peek_tok = peek2_tok;
            peeked2 = 0;
        } else {
            peeked = 0;
        }
    }
    return saved;
}

void
lex_body_end(int saved)
{
    paren_depth = saved;
}

void
lex_save(struct lex_state *s)
{
    s->p = p;
    s->line = line;
    s->peeked = peeked;
    s->peeked2 = peeked2;
    s->last_kind = last_kind;
    s->paren_depth = paren_depth;
    s->cond_depth = cond_depth;
    s->holes_open = holes_open;
    s->peek_tok = peek_tok;
    s->peek2_tok = peek2_tok;
    memcpy(s->hole_kind, hole_kind, sizeof hole_kind);
}

void
lex_restore(const struct lex_state *s)
{
    p = s->p;
    line = s->line;
    peeked = s->peeked;
    peeked2 = s->peeked2;
    last_kind = s->last_kind;
    paren_depth = s->paren_depth;
    cond_depth = s->cond_depth;
    holes_open = s->holes_open;
    peek_tok = s->peek_tok;
    peek2_tok = s->peek2_tok;
    memcpy(hole_kind, s->hole_kind, sizeof hole_kind);
}

/* Tokens after which a newline continues the statement. This is the
 * CONT_TOKEN set published in grammar.ebnf (Line endings); keep the two
 * in sync. `else` and `then` are deliberately absent: their clauses
 * require the newline. */
static int
is_cont(int k)
{
    switch (k) {
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT:
    case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE:
    case T_ASSIGN:
    case T_DOT: case T_COMMA:
    case T_AND: case T_OR: case T_XOR: case T_NOT:
    case T_IS: case T_IN: case T_AS: case T_FROM: case T_TO:
    case T_RETURNS: case T_TMAYBE: case T_CAN:
    case T_LPAREN: case T_LBRACK:
        return 1;
    default:
        return 0;
    }
}

static int
hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* scan an unsigned integer at p (0x hex, 0b binary, 0o octal, or
 * decimal), advancing p */
static long
scan_uint(void)
{
    long n = 0;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        int d;
        p += 2;
        if (hex_digit((unsigned char)*p) < 0)
            die("%s:%d: bad hex literal", src_file, line);
        while ((d = hex_digit((unsigned char)*p)) >= 0) {
            n = (n << 4) | d;
            p++;
        }
        return n;
    }
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        p += 2;
        if (*p != '0' && *p != '1')
            die("%s:%d: bad binary literal", src_file, line);
        while (*p == '0' || *p == '1')
            n = (n << 1) | (*p++ - '0');
        return n;
    }
    if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
        p += 2;
        if (*p < '0' || *p > '7')
            die("%s:%d: bad octal literal", src_file, line);
        while (*p >= '0' && *p <= '7')
            n = (n << 3) | (*p++ - '0');
        return n;
    }
    while (isdigit((unsigned char)*p))
        n = n * 10 + (*p++ - '0');
    return n;
}

static int
read_escape(void)
{
    int c, h1, h2;

    c = *p++;
    switch (c) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '\\': return '\\';
    case '"':  return '"';
    case '$':  return '$';          /* a literal ${ without opening a hole */
    case 'x':
        h1 = hex_digit((unsigned char)*p);
        if (h1 < 0)
            die("%s:%d: bad \\x escape", src_file, line);
        p++;
        h2 = hex_digit((unsigned char)*p);
        if (h2 < 0)
            return h1;
        p++;
        return (h1 << 4) | h2;
    default:
        die("%s:%d: unknown escape \\%c", src_file, line, c);
    }
    return 0;
}

static struct token
make(int k)
{
    struct token t;
    t.kind = k;
    t.nval = 0;
    t.fval = 0;
    t.sval = NULL;
    t.slen = 0;
    t.line = line;
    return t;
}

/* record bracket depth and last-token bookkeeping, then return */
static struct token
emit(int k)
{
    last_kind = k;
    if (k == T_LPAREN || k == T_LBRACK)
        paren_depth++;
    else if (k == T_RPAREN || k == T_RBRACK) {
        if (paren_depth > 0)
            paren_depth--;
    }
    return make(k);
}

/* Lex a string body from p, which sits just past an opening `"` or past a
 * hole-closing `}`. Reads until the closing `"` (returns T_STRING) or a `${`
 * hole (returns T_ISTR_PIECE for the literal so far and bumps holes_open; the
 * caller resumes here after the hole's `}`). `${` and `}` are unambiguous
 * because the language uses no braces otherwise. */
static struct token
lex_string_body(void)
{
    struct token t;
    char *buf;
    size_t cap, len;
    int piece = 0;

    cap = 16;
    len = 0;
    buf = xmalloc(cap);
    while (*p && *p != '"') {
        int ch;
        if (*p == '$' && p[1] == '{') {         /* start of a hole */
            p += 2;
            if (holes_open >= (int)sizeof hole_kind)
                die("%s:%d: ${ holes nested too deeply", src_file, line);
            hole_kind[holes_open++] = 's';
            piece = 1;
            break;
        }
        if (*p == '\\') {
            p++;
            ch = read_escape();
        } else if (*p == '\n') {
            die("%s:%d: newline in string", src_file, line);
        } else {
            ch = (unsigned char)*p++;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf)
                die("out of memory");
        }
        buf[len++] = (char)ch;
    }
    if (!piece) {
        if (*p != '"')
            die("%s:%d: unterminated string", src_file, line);
        p++;
    }
    buf[len] = '\0';
    t = make(piece ? T_ISTR_PIECE : T_STRING);
    t.sval = arena_strndup(lex_arena, buf, len);
    t.slen = (int)len;
    free(buf);
    last_kind = t.kind;
    return t;
}

static struct token
lex_string(void)
{
    return lex_string_body();           /* p is just past the opening `"` */
}

static struct token
lex_one(void)
{
    struct token t;
    const char *start;
    int c, saw_nl;

    /* layout: whitespace, comments, and newline handling */
    saw_nl = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (*p == '\\') {
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t' || *q == '\r')
                q++;
            if (*q == '\n') {
                p = q + 1;
                line++;
                continue;
            }
        }
        if (*p == '\n') {
            line++;
            p++;
            if (!(paren_depth > 0 || cond_depth > 0 || is_cont(last_kind)))
                saw_nl = 1;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            if (p[2] == '/' && trace_on)
                break;                    /* trace comment token */
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n')
                    line++;
                p++;
            }
            if (*p)
                p += 2;
            continue;
        }
        break;
    }

    if (saw_nl && last_kind != T_NL && last_kind != T_EOF) {
        last_kind = T_NL;
        return make(T_NL);
    }

    if (*p == '\0') {
        last_kind = T_EOF;
        return make(T_EOF);
    }

    /* `}` closes an open ${ hole: resume the enclosing string body, or
     * emit the closing token of a data-literal hole */
    if (holes_open > 0 && *p == '}') {
        p++;
        holes_open--;
        if (hole_kind[holes_open] == 's')
            return lex_string_body();
        return emit(T_HOLE_END);
    }

    c = (unsigned char)*p;

    /* /// trace comment (reached only when trace_on) */
    if (c == '/' && p[1] == '/' && p[2] == '/') {
        p += 3;
        while (*p == ' ' || *p == '\t')
            p++;
        start = p;
        while (*p && *p != '\n')
            p++;
        t = make(T_TRACE_COMMENT);
        t.sval = arena_strndup(lex_arena, start, (size_t)(p - start));
        t.slen = (int)(p - start);
        last_kind = T_TRACE_COMMENT;
        return t;
    }

    /* identifiers and keywords (case-insensitive keyword match) */
    if (isalpha(c) || c == '_') {
        const struct kw *kw;
        char low[32];
        size_t sz, i;
        start = p;
        while (isalnum((unsigned char)*p) || *p == '_')
            p++;
        sz = (size_t)(p - start);
        if (sz < sizeof(low)) {
            for (i = 0; i < sz; i++)
                low[i] = (char)tolower((unsigned char)start[i]);
            low[sz] = '\0';
            /* a leading underscore is always a user identifier */
            if (start[0] != '_') {
                for (kw = keywords; kw->s; kw++) {
                    if (strcmp(kw->s, low) == 0)
                        return emit(kw->k);
                }
                if (strcmp(low, "fixed") == 0)
                    die("%s:%d: the type is called `decimal` now "
                        "(base-10, four fraction digits; numbers.md); "
                        "`fixed` and `fixed(num, den)` were retired",
                        src_file, line);
            }
        }
        t = make(T_IDENT);
        t.sval = arena_strndup(lex_arena, start, sz);
        t.slen = (int)sz;
        last_kind = T_IDENT;
        return t;
    }

    /* number literals: decimal integer or decimal/float, and the
     * prefixed 0x/0b/0o integers */
    if (isdigit(c)) {
        const char *num_start = p;
        long n;

        if (p[0] == '0' && (p[1] == 'f' || p[1] == 'F') &&
            isdigit((unsigned char)p[2]))
            die("%s:%d: decimal literals need no `0f` prefix now; "
                "write the value plainly (numbers.md)", src_file, line);
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X' ||
                            p[1] == 'b' || p[1] == 'B' ||
                            p[1] == 'o' || p[1] == 'O')) {
            n = scan_uint();            /* prefixed forms have no float */
        } else {
            int is_float = 0;
            while (isdigit((unsigned char)*p))
                p++;
            if (*p == '.' && isdigit((unsigned char)p[1])) {
                is_float = 1;
                p++;
                while (isdigit((unsigned char)*p))
                    p++;
            }
            if (*p == 'e' || *p == 'E') {
                is_float = 1;
                p++;
                if (*p == '+' || *p == '-')
                    p++;
                if (!isdigit((unsigned char)*p))
                    die("%s:%d: bad float exponent", src_file, line);
                while (isdigit((unsigned char)*p))
                    p++;
            }
            if (is_float) {
                t = make(T_FLOAT_LIT);
                t.fval = strtod(num_start, NULL);
                last_kind = T_FLOAT_LIT;
                return t;
            }
            n = 0;
            for (const char *q = num_start; q < p; q++)
                n = n * 10 + (*q - '0');
        }

        t = make(T_NUMBER);
        t.nval = n;
        last_kind = T_NUMBER;
        return t;
    }

    if (c == '"') {
        p++;
        return lex_string();
    }

    /* operators and punctuation */
    switch (c) {
    case '(': p++; return emit(T_LPAREN);
    case ')': p++; return emit(T_RPAREN);
    case '[': p++; return emit(T_LBRACK);
    case ']': p++; return emit(T_RBRACK);
    case ',': p++; return emit(T_COMMA);
    case ':': p++; return emit(T_COLON);
    case '?':
        die("%s:%d: there is no `?:`; write `cond then A else B`",
            src_file, line);
        break;
    case '$':
        p++;
        if (*p == '{') {                /* a ${ hole in a data literal */
            p++;
            if (holes_open >= (int)sizeof hole_kind)
                die("%s:%d: ${ holes nested too deeply", src_file, line);
            hole_kind[holes_open++] = 'd';
            return emit(T_HOLE);
        }
        die("%s:%d: stray `$` (a hole is written ${expr})",
            src_file, line);
        break;
    case '*': p++; return emit(T_STAR);
    case '/': p++; return emit(T_SLASH);
    case '%': p++; return emit(T_PERCENT);
    case '^': p++; return emit(T_CARET);        /* set toggle (set-of.md) */
    case '.':
        p++;
        if (*p == '.')
            die("%s:%d: there is no `..`; a range is written `lo to hi`",
                src_file, line);
        return emit(T_DOT);
    case '+':
        p++;
        if (*p == '=')
            die("%s:%d: there is no `+=`; write `x = x + 1`",
                src_file, line);
        return emit(T_PLUS);
    case '-':
        p++;
        if (*p == '>')
            die("%s:%d: there is no `->`; a match arm uses `then` and a "
                "return type uses `returns`", src_file, line);
        if (*p == '=')
            die("%s:%d: there is no `-=`; write `x = x - 1`",
                src_file, line);
        return emit(T_MINUS);
    case '=':
        p++;
        if (*p == '=')
            die("%s:%d: there is no `==`; equality is `=` (equality.md)",
                src_file, line);
        return emit(T_ASSIGN);
    case '!':
        p++;
        if (*p == '=')
            die("%s:%d: there is no `!=`; inequality is `<>` (equality.md)",
                src_file, line);
        die("%s:%d: unexpected '!' (use `not`)", src_file, line);
        break;
    case '<':
        p++;
        if (*p == '=') { p++; return emit(T_LE); }
        if (*p == '>') { p++; return emit(T_NE); }    /* `<>` not-equal */
        return emit(T_LT);
    case '>':
        p++;
        if (*p == '=') { p++; return emit(T_GE); }
        return emit(T_GT);
    }

    die("%s:%d: unexpected character 0x%02x '%c'", src_file, line,
        c, isprint(c) ? c : '?');
    return t;
}

struct token
lex_next(void)
{
    if (peeked) {
        struct token t = peek_tok;
        if (peeked2) {
            peek_tok = peek2_tok;
            peeked2 = 0;
        } else {
            peeked = 0;
        }
        return t;
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

/* the token after lex_peek()'s: two-token lookahead, used only to tell
 * a match arm's constant-label list from a statement (see parse_match) */
struct token
lex_peek2(void)
{
    lex_peek();
    if (!peeked2) {
        peek2_tok = lex_one();
        peeked2 = 1;
    }
    return peek2_tok;
}

const char *
lex_filename(void)
{
    return src_file;
}

int
lex_trace_on(void)
{
    return trace_on;
}

const char *
tok_str(int kind)
{
    switch (kind) {
    case T_EOF:       return "EOF";
    case T_NL:        return "NL";
    case T_USE:       return "use";
    case T_CONST:     return "const";
    case T_TYPE:      return "type";
    case T_CLASS:     return "class";
    case T_ENDCLASS:  return "endclass";
    case T_ENUM:      return "enum";
    case T_RECORD:    return "record";
    case T_ENDRECORD: return "endrecord";
    case T_SHAPE:     return "shape";
    case T_ENDSHAPE:  return "endshape";
    case T_INTERFACE: return "interface";
    case T_ENDINTERFACE: return "endinterface";
    case T_SUPPORTS:  return "supports";
    case T_PUBLIC:    return "public";
    case T_PRIVATE:   return "private";
    case T_TUNABLE:   return "tunable";
    case T_DISCLOSES: return "discloses";
    case T_VERB:      return "verb";
    case T_ENDVERB:   return "endverb";
    case T_FUNC:      return "func";
    case T_ENDFUNC:   return "endfunc";
    case T_IF:        return "if";
    case T_ELSEIF:    return "elseif";
    case T_ELSE:      return "else";
    case T_OTHERWISE: return "otherwise";
    case T_ENDIF:     return "endif";
    case T_FOR:       return "for";
    case T_IN:        return "in";
    case T_ENDFOR:    return "endfor";
    case T_WHILE:     return "while";
    case T_ENDWHILE:  return "endwhile";
    case T_MATCH:     return "match";
    case T_SELECT:    return "select";
    case T_FROM:      return "from";
    case T_WHEN:      return "when";
    case T_ENDMATCH:  return "endmatch";
    case T_VAR:       return "var";
    case T_RETURN:    return "return";
    case T_BREAK:     return "break";
    case T_CONTINUE:  return "continue";
    case T_TRACE:     return "trace";
    case T_QUOTE:     return "quote";
    case T_QUASI:     return "quasi";
    case T_MACRO:     return "macro";
    case T_ENDMACRO:  return "endmacro";
    case T_TAGS:      return "tags";
    case T_TSET:      return "set";
    case T_OF:        return "of";
    case T_EMPTY:     return "empty";
    case T_FULL:      return "full";
    case T_OVERLAPS:  return "overlaps";
    case T_SHARED:    return "shared";
    case T_YIELD:     return "yield";
    case T_DEFER:     return "defer";
    case T_SELF:      return "self";
    case T_IS:        return "is";
    case T_AS:        return "as";
    case T_AND:       return "and";
    case T_OR:        return "or";
    case T_XOR:       return "xor";
    case T_NOT:       return "not";
    case T_TRUE:      return "true";
    case T_FALSE:     return "false";
    case T_NIL:       return "nil";
    case T_NOTHING:   return "nothing";
    case T_TMAYBE:    return "maybe";
    case T_CAN:       return "can";
    case T_FAIL:      return "fail";
    case T_TINT:      return "int";
    case T_TFLOAT:    return "float";
    case T_TDEC:      return "decimal";
    case T_TVEC:      return "vec";
    case T_TMAT:      return "mat";
    case T_TSTR:      return "str";
    case T_TOBJ:      return "obj";
    case T_TBOOL:     return "bool";
    case T_TERR:      return "err";
    case T_TLIST:     return "list";
    case T_TPROP:     return "prop";
    case T_IDENT:     return "IDENT";
    case T_NUMBER:    return "NUMBER";
    case T_FLOAT_LIT: return "FLOAT";
    case T_STRING:    return "STRING";
    case T_ISTR_PIECE: return "STRING";
    case T_PLUS:      return "+";
    case T_MINUS:     return "-";
    case T_STAR:      return "*";
    case T_SLASH:     return "/";
    case T_PERCENT:   return "%";
    case T_CARET:     return "^";
    case T_EQ:        return "=";
    case T_NE:        return "<>";
    case T_LT:        return "<";
    case T_LE:        return "<=";
    case T_GT:        return ">";
    case T_GE:        return ">=";
    case T_ASSIGN:    return "=";
    case T_TO:        return "to";
    case T_WITH:      return "with";
    case T_THEN:      return "then";
    case T_DO:        return "do";
    case T_RETURNS:   return "returns";
    case T_LPAREN:    return "(";
    case T_RPAREN:    return ")";
    case T_LBRACK:    return "[";
    case T_RBRACK:    return "]";
    case T_DOT:       return ".";
    case T_COLON:     return ":";
    case T_COMMA:     return ",";
    case T_HOLE:      return "${";
    case T_HOLE_END:  return "}";
    case T_TRACE_COMMENT: return "///";
    default:          return "?";
    }
}
