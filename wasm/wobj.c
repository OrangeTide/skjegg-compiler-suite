/* wobj.c : read a relocatable WASM object into a struct wobj (see wld.h). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "wld.h"

/* a read cursor over a byte range */
struct cur {
    const unsigned char *p;
    size_t               n;
    size_t               off;
};

static unsigned char
rd_byte(struct cur *c)
{
    if (c->off >= c->n)
        die("truncated object");
    return c->p[c->off++];
}

static uint64_t
rd_uleb(struct cur *c)
{
    return wasm_read_uleb(c->p, c->n, &c->off);
}

static int64_t
rd_sleb(struct cur *c)
{
    return wasm_read_sleb(c->p, c->n, &c->off);
}

/* read a length-prefixed name as a fresh NUL-terminated string */
static char *
rd_name(struct cur *c)
{
    uint64_t len = rd_uleb(c);
    char *s;
    if (c->off + len > c->n)
        die("name past end of object");
    s = xmalloc((size_t)len + 1);
    memcpy(s, c->p + c->off, (size_t)len);
    s[len] = '\0';
    c->off += (size_t)len;
    return s;
}

/* read a limits structure (flag + min [+ max]); the values are discarded */
static void
skip_limits(struct cur *c)
{
    unsigned char flag = rd_byte(c);
    rd_uleb(c);                         /* min */
    if (flag & 0x01)
        rd_uleb(c);                     /* max */
}

static void
parse_types(struct wobj *o, struct cur *c)
{
    int n = (int)rd_uleb(c), i, j;
    o->types = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(*o->types));
    o->ntypes = n;
    for (i = 0; i < n; i++) {
        struct wtype *t = &o->types[i];
        if (rd_byte(c) != 0x60)
            die("%s: bad function type", o->path);
        t->np = (int)rd_uleb(c);
        if (t->np > WLD_MAXP)
            die("%s: too many parameters", o->path);
        for (j = 0; j < t->np; j++)
            t->p[j] = rd_byte(c);
        t->nr = (int)rd_uleb(c);
        t->r = 0;
        for (j = 0; j < t->nr; j++)
            t->r = rd_byte(c);
    }
}

static void
parse_imports(struct wobj *o, struct cur *c)
{
    int n = (int)rd_uleb(c), i;
    o->funcimport_name = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(char *));
    o->funcimport_module = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(char *));
    o->funcimport_type = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    o->globimport_name = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(char *));
    o->nfuncimports = 0;
    o->nglobimports = 0;
    for (i = 0; i < n; i++) {
        char *mod = rd_name(c);
        char *field = rd_name(c);
        unsigned char kind = rd_byte(c);
        switch (kind) {
        case 0x00:                      /* function */
            o->funcimport_type[o->nfuncimports] = (int)rd_uleb(c);  /* type index */
            o->funcimport_module[o->nfuncimports] = mod;
            o->funcimport_name[o->nfuncimports++] = field;
            break;
        case 0x01:                      /* table */
            /* the only table an object imports is env.__indirect_function_table,
               present exactly when the object has a call_indirect.  The linker
               must emit table 0 for that call_indirect even if no function
               address is taken (no elem slots), so record the import. */
            o->imports_table = 1;
            rd_byte(c);                 /* elem type */
            skip_limits(c);
            free(field);
            free(mod);
            break;
        case 0x02:                      /* memory */
            skip_limits(c);
            free(field);
            free(mod);
            break;
        case 0x03:                      /* global */
            rd_byte(c);                 /* valtype */
            rd_byte(c);                 /* mutability */
            o->globimport_name[o->nglobimports++] = field;
            free(mod);
            break;
        default:
            die("%s: unknown import kind %d", o->path, kind);
        }
    }
}

static void
parse_functions(struct wobj *o, struct cur *c)
{
    int n = (int)rd_uleb(c), i;
    o->func_type = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    o->ndeffns = n;
    for (i = 0; i < n; i++)
        o->func_type[i] = (int)rd_uleb(c);
}

static void
parse_code(struct wobj *o, struct cur *c, size_t paylen)
{
    size_t start = c->off;
    o->code = xmalloc(paylen);
    memcpy(o->code, c->p + start, paylen);
    o->codelen = paylen;
    {   /* the function count is a uleb at the start; the entries follow it */
        struct cur cc = { o->code, paylen, 0 };
        o->code_count = (int)rd_uleb(&cc);
        o->code_entries_off = cc.off;
    }
    c->off = start + paylen;
}

static void
parse_data(struct wobj *o, struct cur *c, size_t paylen)
{
    size_t contents = c->off;           /* reloc.DATA offsets are relative to here */
    size_t end = c->off + paylen;
    int count = (int)rd_uleb(c);
    unsigned char flags;
    uint64_t size;
    if (count == 0) {
        c->off = end;
        return;
    }
    if (count > 1)
        die("%s: multi-segment data is not yet supported", o->path);
    flags = rd_byte(c);
    if (flags == 0x02)
        rd_uleb(c);                     /* explicit memory index */
    if (flags == 0x00 || flags == 0x02) {
        /* an active segment carries an i32.const offset expression */
        if (rd_byte(c) != 0x41)
            die("%s: unexpected data offset expression", o->path);
        rd_sleb(c);                     /* the object-local offset, reassigned here */
        if (rd_byte(c) != 0x0B)
            die("%s: unterminated data offset expression", o->path);
    }
    size = rd_uleb(c);
    o->data_bytes_off = c->off - contents;
    if (c->off + size > end)
        die("%s: data segment past section end", o->path);
    o->data = xmalloc((size_t)(size > 0 ? size : 1));
    memcpy(o->data, c->p + c->off, (size_t)size);
    o->datalen = (size_t)size;
    o->has_data = 1;
    c->off = end;
}

static void
parse_segment_info(struct wobj *o, struct cur *c)
{
    int n = (int)rd_uleb(c), i;
    for (i = 0; i < n; i++) {
        char *name = rd_name(c);
        int p2align = (int)rd_uleb(c);
        rd_uleb(c);                     /* flags */
        if (i == 0)
            o->data_p2align = p2align;
        free(name);
    }
}

static void
parse_symtab(struct wobj *o, struct cur *c)
{
    int n = (int)rd_uleb(c), i;
    o->syms = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(*o->syms));
    o->nsyms = n;
    for (i = 0; i < n; i++) {
        struct wsymbol *s = &o->syms[i];
        s->kind = rd_byte(c);
        s->flags = (int)rd_uleb(c);
        s->name = NULL;
        s->seg = s->off = s->size = 0;
        if (s->kind == WASM_SYMTAB_DATA) {
            s->name = rd_name(c);
            if (!(s->flags & WASM_SYM_UNDEFINED)) {
                s->seg = (int)rd_uleb(c);
                s->off = (int)rd_uleb(c);
                s->size = (int)rd_uleb(c);
            }
            s->index = 0;
        } else {                        /* function or global */
            s->index = (int)rd_uleb(c);
            if (!(s->flags & WASM_SYM_UNDEFINED) || (s->flags & WASM_SYM_EXPLICIT_NAME))
                s->name = rd_name(c);
            else if (s->kind == WASM_SYMTAB_FUNCTION)
                s->name = xstrdup(o->funcimport_name[s->index]);
            else                        /* undefined global: the import field */
                s->name = xstrdup(o->globimport_name[s->index]);
        }
    }
}

static void
parse_linking(struct wobj *o, struct cur *c, size_t paylen)
{
    size_t end = c->off + paylen;
    rd_uleb(c);                         /* metadata version */
    while (c->off < end) {
        unsigned char subid = rd_byte(c);
        size_t sublen = (size_t)rd_uleb(c);
        size_t subend = c->off + sublen;
        if (subid == 8)                 /* WASM_SYMBOL_TABLE */
            parse_symtab(o, c);
        else if (subid == 5)            /* WASM_SEGMENT_INFO */
            parse_segment_info(o, c);
        c->off = subend;                /* skip anything not consumed */
    }
    c->off = end;
}

static void
parse_relocs(struct cur *c, size_t end, struct wreloc **out, int *nout)
{
    int n, i;
    rd_uleb(c);                         /* target section index */
    n = (int)rd_uleb(c);
    *out = xmalloc((size_t)(n > 0 ? n : 1) * sizeof(**out));
    *nout = n;
    for (i = 0; i < n; i++) {
        struct wreloc *r = &(*out)[i];
        r->type = rd_byte(c);
        r->off = (size_t)rd_uleb(c);
        r->index = (int)rd_uleb(c);
        r->addend = 0;
        if (r->type == R_WASM_MEMORY_ADDR_LEB || r->type == R_WASM_MEMORY_ADDR_SLEB ||
            r->type == R_WASM_MEMORY_ADDR_I32)
            r->addend = (long)rd_sleb(c);
    }
    c->off = end;
}

static void
parse_custom(struct wobj *o, struct cur *c, size_t paylen)
{
    size_t end = c->off + paylen;
    char *name = rd_name(c);
    if (strcmp(name, "linking") == 0)
        parse_linking(o, c, end - c->off);
    else if (strcmp(name, "reloc.CODE") == 0)
        parse_relocs(c, end, &o->crelocs, &o->ncrelocs);
    else if (strcmp(name, "reloc.DATA") == 0)
        parse_relocs(c, end, &o->drelocs, &o->ndrelocs);
    c->off = end;
    free(name);
}

struct wobj *
wobj_read(const char *path)
{
    struct wobj *o = xcalloc(1, sizeof(*o));
    FILE *f;
    long n;
    struct cur c;

    o->path = path;
    f = fopen(path, "rb");
    if (!f)
        die("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0)
        die("cannot read %s", path);
    rewind(f);
    o->raw = xmalloc((size_t)n);
    if (fread(o->raw, 1, (size_t)n, f) != (size_t)n)
        die("read failed on %s", path);
    fclose(f);
    o->rawlen = (size_t)n;

    c.p = o->raw;
    c.n = o->rawlen;
    c.off = 0;
    if (o->rawlen < 8 || memcmp(o->raw, "\0asm", 4) != 0)
        die("%s is not a WASM object", path);
    c.off = 8;                          /* skip magic + version */

    while (c.off < c.n) {
        unsigned char id = rd_byte(&c);
        size_t paylen = (size_t)rd_uleb(&c);
        size_t payend = c.off + paylen;
        switch (id) {
        case 1:  parse_types(o, &c);     break;
        case 2:  parse_imports(o, &c);   break;
        case 3:  parse_functions(o, &c); break;
        case 10: parse_code(o, &c, paylen); break;
        case 11: parse_data(o, &c, paylen); break;
        case 0:  parse_custom(o, &c, paylen); break;
        default: break;                 /* table/memory/global/export/elem: later phases */
        }
        c.off = payend;
    }
    return o;
}
