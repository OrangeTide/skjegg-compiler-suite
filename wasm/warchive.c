/* warchive.c : ar(1) archive (.a) support for skj-ld-wasm.
 *
 * A static archive is a symbol index plus a bag of relocatable objects.  The
 * linker pulls in only the members that define a symbol some already-loaded
 * object references but nothing defines, iterating until the set is stable (a
 * pulled member can reference further members).  It relies on the GNU/SysV
 * symbol index (member "/"), which skj-ar writes for WASM objects the same way
 * it does for ELF.
 *
 * Member selection is driven entirely by "is this symbol still undefined?"
 * (sym_needed): once a member is pulled its symbols become defined, so no later
 * index entry re-pulls it.  A seen-set keyed by member offset guards against a
 * malformed index that keeps listing a symbol a member does not actually
 * define, which would otherwise re-pull without end.
 *
 * It reads the archive with fopen/fread, not mmap, so the file stays in the
 * subset skj-cc-wasm compiles when this links into the self-hosted skj-ld.wasm.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "wld.h"

/****************************************************************
 * Object vector
 ****************************************************************/

void
wobjvec_push(struct wobjvec *v, struct wobj *o)
{
    if (v->n >= v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->objs = realloc(v->objs, (size_t)v->cap * sizeof(*v->objs));
        if (!v->objs)
            die("out of memory");
    }
    v->objs[v->n++] = o;
}

/****************************************************************
 * ar field helpers
 ****************************************************************/

static uint32_t
be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Decimal ASCII field of width w, space padded. */
static long
ar_num(const unsigned char *p, int w)
{
    long v = 0;
    int i;
    for (i = 0; i < w; i++) {
        if (p[i] < '0' || p[i] > '9')
            break;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

/* The member's own name, resolving a GNU long name ("/N" -> offset N in the
 * "//" string table), formatted as "archive.a(member.o)" for diagnostics. */
static char *
member_name(const unsigned char *h, const char *ext, uint32_t extlen,
            const char *arpath)
{
    char mem[64];
    int k = 0;
    size_t n;
    char *out;

    if (h[0] == '/' && h[1] >= '0' && h[1] <= '9') {
        long off = ar_num(h + 1, 15);
        while (off + k < (long)extlen && ext[off + k] != '/'
               && ext[off + k] != '\n' && k < (int)sizeof(mem) - 1) {
            mem[k] = ext[off + k];
            k++;
        }
    } else {
        while (k < 16 && h[k] != '/' && h[k] != ' '
               && k < (int)sizeof(mem) - 1) {
            mem[k] = (char)h[k];
            k++;
        }
    }
    mem[k] = '\0';

    n = strlen(arpath) + strlen(mem) + 3;
    out = xmalloc(n);
    snprintf(out, n, "%s(%s)", arpath, mem);
    return out;
}

/****************************************************************
 * Symbol state over the currently loaded objects
 ****************************************************************/

/* A symbol is "needed" when some loaded object references it (an undefined
 * entry) and no loaded object defines it with external linkage, so pulling a
 * member that provides it is warranted.  A local definition does not satisfy a
 * cross-object reference, so it does not count as a definition here, matching
 * what skj-ar put in the index. */
static int
sym_needed(struct wobjvec *v, const char *name)
{
    int undef = 0, def = 0;
    int oi, si;

    for (oi = 0; oi < v->n; oi++) {
        struct wobj *o = v->objs[oi];
        for (si = 0; si < o->nsyms; si++) {
            struct wsymbol *s = &o->syms[si];
            if (!s->name || strcmp(s->name, name) != 0)
                continue;
            if (s->flags & WASM_SYM_UNDEFINED)
                undef = 1;
            else if (!(s->flags & WASM_SYM_BINDING_LOCAL))
                def = 1;
        }
    }
    return undef && !def;
}

/****************************************************************
 * Archive reader
 ****************************************************************/

/* Read the whole file into a fresh heap buffer; *lenp gets its length. */
static unsigned char *
ar_slurp(const char *path, size_t *lenp)
{
    FILE *f;
    long n;
    unsigned char *buf;

    f = fopen(path, "rb");
    if (!f)
        die("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0)
        die("cannot read %s", path);
    rewind(f);
    buf = xmalloc((size_t)(n ? n : 1));
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        die("read failed on %s", path);
    fclose(f);
    *lenp = (size_t)n;
    return buf;
}

int
warchive_is_archive(const char *path)
{
    FILE *f;
    char magic[8];
    size_t n;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    n = fread(magic, 1, 8, f);
    fclose(f);
    return n == 8 && memcmp(magic, "!<arch>\n", 8) == 0;
}

int
warchive_read(struct wobjvec *v, const char *path)
{
    unsigned char *buf;
    size_t len, pos;
    const unsigned char *symtab = NULL;
    uint32_t symtab_size = 0;
    const char *ext = NULL;
    uint32_t ext_size = 0;
    uint32_t nsym, i;
    const unsigned char *offs;
    const char *names, *names_end;
    uint32_t *seen = NULL;              /* member offsets already pulled */
    int nseen = 0, seen_cap = 0;
    int added = 0, changed;

    buf = ar_slurp(path, &len);
    if (len < 8 || memcmp(buf, "!<arch>\n", 8) != 0)
        die("%s: not an ar archive", path);

    /* first pass: find the symbol index ("/") and the long-name table ("//") */
    for (pos = 8; pos + 60 <= len; ) {
        const unsigned char *h = buf + pos;
        long size = ar_num(h + 48, 10);

        if (h[0] == '/' && h[1] == ' ') {
            symtab = buf + pos + 60;
            symtab_size = (uint32_t)size;
        } else if (h[0] == '/' && h[1] == '/') {
            ext = (const char *)(buf + pos + 60);
            ext_size = (uint32_t)size;
        }
        pos += 60 + (size_t)size;
        if (size & 1)
            pos++;                      /* members are 2-byte aligned */
    }

    if (!symtab || symtab_size < 4)
        die("%s: no symbol index (build the archive with 'skj-ar rcs')", path);

    /* A corrupt or truncated archive must be rejected, not walked off the end
       of the buffer: the index and long-name members must lie within it. */
    if ((size_t)(symtab - buf) + symtab_size > len)
        die("%s: symbol index runs past the end of the archive", path);
    if (ext && (size_t)((const unsigned char *)ext - buf) + ext_size > len)
        die("%s: long-name table runs past the end of the archive", path);

    /* GNU index: be32 count, count be32 member offsets, count NUL names */
    nsym = be32(symtab);
    if ((uint64_t)4 + (uint64_t)nsym * 4 > symtab_size)
        die("%s: symbol index count exceeds its member", path);
    offs = symtab + 4;
    names = (const char *)(symtab + 4 + (size_t)nsym * 4);
    names_end = (const char *)(symtab + symtab_size);

    changed = 1;
    while (changed) {
        const char *np = names;
        changed = 0;
        for (i = 0; i < nsym; i++) {
            const char *name = np;
            const void *nul = memchr(np, '\0', (size_t)(names_end - np));
            uint32_t moff;
            long msize;
            const unsigned char *h;
            unsigned char *copy;
            struct wobj *o;
            int j, dup;

            if (!nul)
                die("%s: unterminated name in symbol index", path);
            np = (const char *)nul + 1;
            if (!sym_needed(v, name))
                continue;

            moff = be32(offs + (size_t)i * 4);
            dup = 0;
            for (j = 0; j < nseen; j++)
                if (seen[j] == moff) {
                    dup = 1;
                    break;
                }
            if (dup)
                continue;               /* never pull one member twice */
            if ((size_t)moff + 60 > len)
                die("%s: bad member offset in symbol index", path);
            h = buf + moff;
            msize = ar_num(h + 48, 10);
            if ((size_t)moff + 60 + (size_t)msize > len)
                die("%s: member overruns the archive", path);

            /* copy the member out so it survives freeing the archive buffer */
            copy = xmalloc((size_t)(msize ? msize : 1));
            memcpy(copy, buf + moff + 60, (size_t)msize);
            o = wobj_parse(member_name(h, ext, ext_size, path), copy,
                           (size_t)msize);
            wobjvec_push(v, o);

            if (nseen >= seen_cap) {
                seen_cap = seen_cap ? seen_cap * 2 : 16;
                seen = realloc(seen, (size_t)seen_cap * sizeof(*seen));
                if (!seen)
                    die("out of memory");
            }
            seen[nseen++] = moff;
            added++;
            changed = 1;
        }
    }

    free(seen);
    free(buf);
    return added;
}
