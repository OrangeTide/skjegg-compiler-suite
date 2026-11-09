/* bmi.c : the BASIC module interface (.bmi), doc/languages/basic-modules.md.

   A .bmi member is the machine-generated interface a BASIC module presents: its
   exported constants, opaque types, and routines, with primitive-class
   signatures and the object symbol each binds to. bmi_write emits one from a
   module's AST; bmi_read parses one back. The format is line-oriented text,
   interface only (no IR), and per target (a width and endianness tag the reader
   checks). */

#include "basic.h"
#include "util.h"
#include "target.h"

#include <stdlib.h>
#include <string.h>

/* ---- value classes ---- */

static const char *const class_name[] = {
    "void", "i8", "i16", "i32", "i64", "f32", "f64", "str", "handle"
};

static const char *
class_to_str(int cls)
{
    if (cls < 0 || cls >= (int)(sizeof(class_name) / sizeof(class_name[0])))
        return "void";
    return class_name[cls];
}

static int
class_from_str(const char *s, size_t n)
{
    int i;
    for (i = 0; i < (int)(sizeof(class_name) / sizeof(class_name[0])); i++)
        if (strlen(class_name[i]) == n && memcmp(class_name[i], s, n) == 0)
            return i;
    return -1;
}

/* A BASIC declared type (TY_DECL_*) as a .bmi primitive class. INT is i64, BYTE
   is i8, FLOAT is f64, STRING is a BASIC string handle, and a record (always a
   REF parameter at an exported boundary) is an opaque handle. */
static int
decl_to_class(int ty)
{
    switch (ty) {
    case TY_DECL_INT:    return BMI_I64;
    case TY_DECL_FLOAT:  return BMI_F64;
    case TY_DECL_BYTE:   return BMI_I8;
    case TY_DECL_STRING: return BMI_STR;
    case TY_DECL_RECORD: return BMI_HANDLE;
    default:             return BMI_VOID;
    }
}

/* ---- writer ---- */

/* A parameter's signature token. A record parameter (always REF) is the opaque
   handle itself. A REF primitive is a pointer to that primitive, written
   "ref <class>". A by-value primitive is just its class. */
static void
write_param(FILE *out, struct node *pm)
{
    if (pm->op == TY_DECL_RECORD)
        fprintf(out, "handle %s ", pm->sval);   /* a named opaque handle */
    else if (pm->ival)
        fprintf(out, "ref %s ", class_to_str(decl_to_class(pm->op)));
    else
        fprintf(out, "%s ", class_to_str(decl_to_class(pm->op)));
}

static void
write_sig(FILE *out, struct node *params)
{
    struct node *pm;

    fputs("( ", out);
    for (pm = params; pm; pm = pm->next)
        write_param(out, pm);
    fputs(")", out);
}

void
bmi_write(FILE *out, struct node *prog)
{
    struct node *s;

    if (!prog || prog->kind != N_PROGRAM || !prog->name)
        die("bmi_write: not a module");

    fputs(".bmi 1\n", out);
    fprintf(out, ".target %s %s\n",
            SKJ_LP64 ? "lp64" : "ilp32",
            SKJ_BIG_ENDIAN ? "big" : "little");
    fprintf(out, ".module %s\n", prog->name);

    for (s = prog->a; s; s = s->next) {
        if (!s->is_export)
            continue;
        switch (s->kind) {
        case N_CONST:
            fprintf(out, ".const %s i64 = %ld\n", s->name, s->ival);
            break;
        case N_TYPE:
            fprintf(out, ".type %s handle\n", s->name);
            break;
        case N_FN:
            if (s->ival)                /* a forward DECLARE; the definition emits */
                break;
            fprintf(out, ".func %s link %s ", s->name, s->name);
            write_sig(out, s->a);
            if (s->op == TY_DECL_RECORD)        /* a named opaque handle return */
                fprintf(out, " -> handle %s\n", s->sval);
            else
                fprintf(out, " -> %s\n", class_to_str(decl_to_class(s->op)));
            break;
        case N_PROC:
            if (s->ival)
                break;
            fprintf(out, ".proc %s link %s ", s->name, s->name);
            write_sig(out, s->a);
            fputc('\n', out);
            break;
        default:
            break;
        }
    }
}

/* ---- reader ---- */

struct bmi_lex {
    const char *p;
    const char *end;
    const char *name;   /* source file name, for diagnostics */
    int         line;
};

/* Advance to the next token, skipping blanks and comments. A token is returned
   as *tok / *len; a newline is reported as the one-byte token "\n"; end of input
   as a zero length. */
static void
bmi_tok(struct bmi_lex *lx, const char **tok, size_t *len)
{
    for (;;) {
        while (lx->p < lx->end && (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r'))
            lx->p++;
        if (lx->p >= lx->end) { *tok = lx->p; *len = 0; return; }
        if (*lx->p == '\n') { *tok = lx->p++; *len = 1; lx->line++; return; }
        if (*lx->p == ';') {                 /* comment to end of line */
            while (lx->p < lx->end && *lx->p != '\n')
                lx->p++;
            continue;
        }
        break;
    }
    *tok = lx->p;
    while (lx->p < lx->end && *lx->p != ' ' && *lx->p != '\t' &&
           *lx->p != '\r' && *lx->p != '\n')
        lx->p++;
    *len = (size_t)(lx->p - *tok);
}

static int
tok_is(const char *tok, size_t len, const char *word)
{
    return strlen(word) == len && memcmp(tok, word, len) == 0;
}

static char *
tok_dup(struct arena *a, const char *tok, size_t len)
{
    char *s = arena_alloc(a, len + 1);
    memcpy(s, tok, len);
    s[len] = '\0';
    return s;
}

static long
tok_long(struct bmi_lex *lx, const char *tok, size_t len)
{
    long v = 0;
    int neg = 0, i = 0;
    if (len > 0 && (tok[0] == '-' || tok[0] == '+')) { neg = tok[0] == '-'; i = 1; }
    if (i >= (int)len)
        die("%s:%d: bad integer in .bmi", lx->name, lx->line);
    for (; i < (int)len; i++) {
        if (tok[i] < '0' || tok[i] > '9')
            die("%s:%d: bad integer in .bmi", lx->name, lx->line);
        v = v * 10 + (tok[i] - '0');
    }
    return neg ? -v : v;
}

static char *read_ident(struct bmi_lex *lx, struct arena *a, const char *what);

/* Parse a signature "( c0 c1 ... )" optionally followed by "-> ret" into sym. */
static void
read_sig(struct bmi_lex *lx, struct arena *a, struct bmi_sym *sym, int want_ret)
{
    const char *t;
    size_t n;

    bmi_tok(lx, &t, &n);
    if (!tok_is(t, n, "("))
        die("%s:%d: expected '(' in a .bmi signature", lx->name, lx->line);
    sym->nparam = 0;
    for (;;) {
        int ref = 0, cls;
        bmi_tok(lx, &t, &n);
        if (tok_is(t, n, ")"))
            break;
        if (tok_is(t, n, "ref")) {          /* a by-reference primitive */
            ref = 1;
            bmi_tok(lx, &t, &n);
        }
        cls = (n == 0 || (n == 1 && t[0] == '\n')) ? -1 : class_from_str(t, n);
        if (cls < 0)
            die("%s:%d: bad parameter class in .bmi", lx->name, lx->line);
        if (sym->nparam >= BMI_MAXPARAM)
            die("%s:%d: too many parameters in .bmi", lx->name, lx->line);
        sym->param[sym->nparam] = cls;
        sym->param_ref[sym->nparam] = ref;
        /* an opaque handle names its type: "handle <Name>" */
        if (cls == BMI_HANDLE)
            sym->param_type[sym->nparam] =
                read_ident(lx, a, "an opaque type name after 'handle'");
        sym->nparam++;
    }
    sym->ret = BMI_VOID;
    if (want_ret) {
        bmi_tok(lx, &t, &n);
        if (!tok_is(t, n, "->"))
            die("%s:%d: expected '->' after a .func signature", lx->name, lx->line);
        bmi_tok(lx, &t, &n);
        sym->ret = class_from_str(t, n);
        if (sym->ret < 0)
            die("%s:%d: bad return class in .bmi", lx->name, lx->line);
        if (sym->ret == BMI_HANDLE)
            sym->ret_type =
                read_ident(lx, a, "an opaque type name after 'handle'");
    }
}

static void
expect_eol(struct bmi_lex *lx)
{
    const char *t;
    size_t n;
    bmi_tok(lx, &t, &n);
    if (n != 1 || t[0] != '\n') {
        if (n == 0)
            return;
        die("%s:%d: trailing tokens in .bmi", lx->name, lx->line);
    }
}

static char *
read_ident(struct bmi_lex *lx, struct arena *a, const char *what)
{
    const char *t;
    size_t n;
    bmi_tok(lx, &t, &n);
    if (n == 0 || t[0] == '\n')
        die("%s:%d: expected %s in .bmi", lx->name, lx->line, what);
    return tok_dup(a, t, n);
}

struct bmi *
bmi_read(struct arena *a, const char *text, size_t len, const char *name)
{
    struct bmi *m = arena_zalloc(a, sizeof(*m));
    struct bmi_sym **tail = &m->syms;
    struct bmi_lex lx;
    const char *t;
    size_t n;

    lx.p = text;
    lx.end = text + len;
    lx.name = name ? name : "<bmi>";
    lx.line = 1;

    bmi_tok(&lx, &t, &n);
    if (!tok_is(t, n, ".bmi"))
        die("%s: not a .bmi member (missing .bmi header)", lx.name);
    bmi_tok(&lx, &t, &n);
    if (!tok_is(t, n, "1"))
        die("%s: unsupported .bmi version", lx.name);
    expect_eol(&lx);

    for (;;) {
        bmi_tok(&lx, &t, &n);
        if (n == 0)
            break;
        if (n == 1 && t[0] == '\n')
            continue;
        if (tok_is(t, n, ".target")) {
            bmi_tok(&lx, &t, &n);
            m->lp64 = tok_is(t, n, "lp64");
            bmi_tok(&lx, &t, &n);
            m->big_endian = tok_is(t, n, "big");
            expect_eol(&lx);
        } else if (tok_is(t, n, ".module")) {
            m->module = read_ident(&lx, a, "a module name");
            expect_eol(&lx);
        } else if (tok_is(t, n, ".const")) {
            struct bmi_sym *s = arena_zalloc(a, sizeof(*s));
            s->kind = BMI_CONST;
            s->name = read_ident(&lx, a, "a constant name");
            bmi_tok(&lx, &t, &n);        /* class, kept as i64 */
            (void)t; (void)n;
            bmi_tok(&lx, &t, &n);        /* '=' */
            if (!tok_is(t, n, "="))
                die("%s:%d: expected '=' in a .const", lx.name, lx.line);
            bmi_tok(&lx, &t, &n);
            s->cval = tok_long(&lx, t, n);
            *tail = s; tail = &s->next;
            expect_eol(&lx);
        } else if (tok_is(t, n, ".type")) {
            struct bmi_sym *s = arena_zalloc(a, sizeof(*s));
            s->kind = BMI_TYPE;
            s->name = read_ident(&lx, a, "a type name");
            bmi_tok(&lx, &t, &n);        /* "handle" */
            if (!tok_is(t, n, "handle"))
                die("%s:%d: an exported .type must be a handle", lx.name, lx.line);
            *tail = s; tail = &s->next;
            expect_eol(&lx);
        } else if (tok_is(t, n, ".func") || tok_is(t, n, ".proc")) {
            int is_fn = tok_is(t, n, ".func");
            struct bmi_sym *s = arena_zalloc(a, sizeof(*s));
            s->kind = is_fn ? BMI_FUNC : BMI_PROC;
            s->name = read_ident(&lx, a, "a routine name");
            bmi_tok(&lx, &t, &n);        /* "link" */
            if (!tok_is(t, n, "link"))
                die("%s:%d: expected 'link' in a .bmi routine", lx.name, lx.line);
            s->link = read_ident(&lx, a, "a link symbol");
            read_sig(&lx, a, s, is_fn);
            *tail = s; tail = &s->next;
            expect_eol(&lx);
        } else {
            die("%s:%d: unknown .bmi directive '%.*s'",
                lx.name, lx.line, (int)n, t);
        }
    }
    *tail = NULL;
    return m;
}

/* ---- archive access ----

   A compiled module is a GNU `ar` archive carrying the reserved `.bmi` member
   beside its `.o` objects. The archiver does not index a dot-named member, so it
   is found by an exact-name header walk (the symbol index '/' and long-name
   table '//' both begin with '/', so their name fields come out empty and never
   match '.bmi'). */

char *
bmi_from_archive(struct arena *a, const char *path, size_t *outlen)
{
    FILE *f;
    long n;
    char *buf, *out;
    size_t len, off, wl = strlen(".bmi");

    f = fopen(path, "rb");
    if (!f)
        die("cannot open module archive '%s'", path);
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        fclose(f);
        die("cannot read module archive '%s'", path);
    }
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        die("out of memory");
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        die("read failed on '%s'", path);
    }
    fclose(f);
    len = (size_t)n;

    if (len < 8 || memcmp(buf, "!<arch>\n", 8) != 0) {
        free(buf);
        die("'%s' is not an ar archive", path);
    }
    off = 8;
    while (off + 60 <= len) {
        const char *h = buf + off;
        char nm[17];
        int k;
        size_t j, sz = 0;

        for (k = 0; k < 16 && h[k] != '/' && h[k] != ' '; k++)
            nm[k] = h[k];
        nm[k] = '\0';
        for (j = 48; j < 58 && h[j] >= '0' && h[j] <= '9'; j++)
            sz = sz * 10 + (size_t)(h[j] - '0');
        if ((size_t)k == wl && memcmp(nm, ".bmi", wl) == 0) {
            if (off + 60 + sz > len)
                sz = len - off - 60;
            out = arena_alloc(a, sz + 1);
            memcpy(out, buf + off + 60, sz);
            out[sz] = '\0';
            free(buf);
            if (outlen)
                *outlen = sz;
            return out;
        }
        off += 60 + sz + (sz & 1);
    }
    free(buf);
    die("'%s' has no .bmi member (not a BASIC module archive)", path);
    return NULL;   /* unreachable */
}

const char *
bmi_unsupported_reason(struct bmi_sym *s)
{
    int i;

    if (s->kind != BMI_FUNC && s->kind != BMI_PROC)
        return NULL;
    for (i = 0; i < s->nparam; i++)
        if (s->param_ref[i])
            return "has a REF parameter, not yet supported";
    return NULL;
}
