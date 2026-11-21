/* wlink.c : resolve, renumber, relocate, and write the output module. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "wld.h"

/* ---- output type table (deduplicated across objects) ---- */

static struct wtype *g_otypes;
static int           g_notypes;

static int
type_eq(const struct wtype *a, const struct wtype *b)
{
    int i;
    if (a->np != b->np || a->nr != b->nr)
        return 0;
    if (a->nr && a->r != b->r)
        return 0;
    for (i = 0; i < a->np; i++)
        if (a->p[i] != b->p[i])
            return 0;
    return 1;
}

static int
otype_add(const struct wtype *t)
{
    int k;
    for (k = 0; k < g_notypes; k++)
        if (type_eq(&g_otypes[k], t))
            return k;
    g_otypes[g_notypes] = *t;
    return g_notypes++;
}

/* intern an output type for a synthesized signature; every synthesized
   parameter and result is i32, so one valtype describes them all */
static int
otype_sig(int np, int nr)
{
    struct wtype t;
    int j;
    t.np = np;
    for (j = 0; j < np; j++)
        t.p[j] = 0x7F;                  /* i32 */
    t.nr = nr;
    t.r = 0x7F;                         /* i32 */
    return otype_add(&t);
}

/* ---- the global map of non-local defined function symbols ---- */

struct gfunc {
    const char *name;
    int         outidx;
};
static struct gfunc *g_gfuncs;
static int           g_ngfuncs;

static int
gfunc_find(const char *name)
{
    int k;
    for (k = 0; k < g_ngfuncs; k++)
        if (strcmp(g_gfuncs[k].name, name) == 0)
            return g_gfuncs[k].outidx;
    return -1;
}

#define WASM_SYM_BINDING_LOCAL 0x02

/* ---- the global map of non-local defined data symbols ---- */

struct gdata {
    const char *name;
    int         addr;               /* final linear-memory address */
};
static struct gdata *g_gdata;
static int           g_ngdata;

static int
gdata_find(const char *name)
{
    int k;
    for (k = 0; k < g_ngdata; k++)
        if (strcmp(g_gdata[k].name, name) == 0)
            return g_gdata[k].addr;
    return -1;
}

/* resolve a data symbol reference to a final address plus its addend */
static int
data_addr(struct wobj *ob, const int *dbase, int o, int symidx, long addend)
{
    struct wsymbol *s = &ob->syms[symidx];
    int a;
    if (s->flags & WASM_SYM_UNDEFINED) {
        a = gdata_find(s->name);
        if (a < 0)
            die("undefined data symbol: %s", s->name);
    } else {
        a = dbase[o] + s->off;
    }
    return a + (int)addend;
}

/* overwrite a 5-byte fixed-width unsigned LEB placeholder at p */
static void
patch5(unsigned char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 5; i++) {
        unsigned char b = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (i < 4)
            b |= 0x80;
        p[i] = b;
    }
}

/* overwrite a 4-byte little-endian i32 at p */
static void
patch4(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* ---- the synthesized WASI runtime ----

   When the link has an entry the module imports three wasi_snapshot_preview1
   functions at indices 0, 1, 2 and the linker defines a small libc seam over
   them plus a _start that calls main.  A seam function is defined only when
   something references it undefined, so a libc that provides its own write or
   exit wins; _start is always defined. */

#define NSEAM 5
static const char *const SEAM_NAMES[NSEAM] = {
    "write", "read", "_exit", "exit", "__libc_sbrk",
};
#define SEAM_SBRK 4                     /* the one seam with a non-trivial body */

/* the break global sits just past __stack_pointer (global 0) */
#define BRK_GLOBAL 1

static int g_synth;                 /* synthesizing the runtime this link */
static int g_prog_end;              /* first output index past the program funcs */
static int g_seam_outidx[NSEAM];    /* output index of each seam, or -1 */
static int g_seam_order[NSEAM];     /* seams in the order they were assigned */
static int g_nseam;                 /* how many seams are in use */

/* Extra WASI imports the objects declare: an undefined function symbol whose
   import module is not "env" is a WASI preview1 import from the libc's shim
   (runtime/wasi_file.c calls __wasi_path_open etc.).  Deduped by (module,
   field); each takes an output funcidx just past the fixed seam imports, and
   the Import section emits a real import for it.  This is how the C-level file,
   directory, and stat seams reach the host without a hand-encoded linker body. */
struct owasi {
    const char *module;
    const char *field;
    int         src_obj;            /* an object that declares it (for the type) */
    int         src_ltype;          /* that object's local type index */
    int         tidx;               /* output type index (filled after the typemap) */
    int         outidx;             /* output function index */
};
static struct owasi *g_owasi;
static int g_nowasi;

static int
owasi_find(const char *module, const char *field)
{
    int k;
    for (k = 0; k < g_nowasi; k++)
        if (strcmp(g_owasi[k].module, module) == 0 &&
            strcmp(g_owasi[k].field, field) == 0)
            return k;
    return -1;
}

static int
seam_name_index(const char *name)
{
    int k;
    for (k = 0; k < NSEAM; k++)
        if (strcmp(SEAM_NAMES[k], name) == 0)
            return k;
    return -1;
}

/* the output index of seam k, assigning one past the program functions on
   first use */
static int
seam_outidx(int k)
{
    if (g_seam_outidx[k] < 0) {
        g_seam_outidx[k] = g_prog_end + g_nseam;
        g_seam_order[g_nseam++] = k;
    }
    return g_seam_outidx[k];
}

/* resolve a function symbol reference to an output function index */
static int
func_outidx(struct wobj *ob, const int *base, int o, int symidx)
{
    struct wsymbol *s = &ob->syms[symidx];
    int v;
    if (s->flags & WASM_SYM_UNDEFINED) {
        if (s->index < ob->nfuncimports &&
            strcmp(ob->funcimport_module[s->index], "env") != 0) {
            int w = owasi_find(ob->funcimport_module[s->index],
                               ob->funcimport_name[s->index]);
            if (w < 0)
                die("internal: unregistered WASI import %s", s->name);
            return g_owasi[w].outidx;
        }
        v = gfunc_find(s->name);
        if (v < 0) {
            int k = g_synth ? seam_name_index(s->name) : -1;
            if (k < 0)
                die("undefined symbol: %s", s->name);
            v = seam_outidx(k);
        }
    } else {
        v = base[o] + (s->index - ob->nfuncimports);
    }
    return v;
}

/* ---- the indirect function table: output funcidx held at each slot ---- */

static int *g_tslots;           /* g_tslots[i] is the function at table slot i+1 */
static int  g_ntslots;

/* return the 1-based table slot for an output function, assigning on first use
   (slot 0 stays reserved as the null entry, matching wasm-ld) */
static int
table_slot(int outidx)
{
    int k;
    for (k = 0; k < g_ntslots; k++)
        if (g_tslots[k] == outidx)
            return k + 1;
    g_tslots[g_ntslots++] = outidx;
    return g_ntslots;
}

/* instruction builders over a code buffer (the seam bodies are fixed) */
static void ib_const(struct wbuf *b, int32_t v) { wbuf_byte(b, 0x41); wbuf_sleb(b, v); }
static void ib_localget(struct wbuf *b, int i)  { wbuf_byte(b, 0x20); wbuf_uleb(b, (uint64_t)i); }
static void ib_localset(struct wbuf *b, int i)  { wbuf_byte(b, 0x21); wbuf_uleb(b, (uint64_t)i); }
static void ib_globalget(struct wbuf *b, int i) { wbuf_byte(b, 0x23); wbuf_uleb(b, (uint64_t)i); }
static void ib_globalset(struct wbuf *b, int i) { wbuf_byte(b, 0x24); wbuf_uleb(b, (uint64_t)i); }
static void ib_store(struct wbuf *b) { wbuf_byte(b, 0x36); wbuf_uleb(b, 2); wbuf_uleb(b, 0); }
static void ib_load(struct wbuf *b)  { wbuf_byte(b, 0x28); wbuf_uleb(b, 2); wbuf_uleb(b, 0); }
static void ib_call(struct wbuf *b, int f) { wbuf_byte(b, 0x10); wbuf_uleb(b, (uint64_t)f); }
static void ib_add(struct wbuf *b)  { wbuf_byte(b, 0x6A); }
static void ib_sub(struct wbuf *b)  { wbuf_byte(b, 0x6B); }
static void ib_shl(struct wbuf *b)  { wbuf_byte(b, 0x74); }
static void ib_shru(struct wbuf *b) { wbuf_byte(b, 0x76); }
static void ib_ltu(struct wbuf *b)  { wbuf_byte(b, 0x49); }
static void ib_gtu(struct wbuf *b)  { wbuf_byte(b, 0x4B); }
static void ib_eq(struct wbuf *b)   { wbuf_byte(b, 0x46); }
static void ib_memsize(struct wbuf *b) { wbuf_byte(b, 0x3F); wbuf_byte(b, 0x00); }
static void ib_memgrow(struct wbuf *b) { wbuf_byte(b, 0x40); wbuf_byte(b, 0x00); }
static void ib_if(struct wbuf *b)   { wbuf_byte(b, 0x04); wbuf_byte(b, 0x40); } /* void blocktype */
static void ib_end(struct wbuf *b)  { wbuf_byte(b, 0x0B); }
static void ib_return(struct wbuf *b) { wbuf_byte(b, 0x0F); }

/* wrap a body (raw instructions, no end) as a code-section entry, declaring
   ni32 i32 locals ahead of it */
static void
emit_body(struct wbuf *code, const struct wbuf *body, int ni32)
{
    struct wbuf b;
    wbuf_init(&b);
    if (ni32 > 0) {
        wbuf_uleb(&b, 1);          /* one local declaration group */
        wbuf_uleb(&b, (uint64_t)ni32);
        wbuf_byte(&b, 0x7F);       /* i32 */
    } else {
        wbuf_uleb(&b, 0);          /* no locals */
    }
    wbuf_bytes(&b, body->data, body->len);
    wbuf_byte(&b, 0x0B);           /* end */
    wbuf_uleb(code, (uint64_t)b.len);
    wbuf_bytes(code, b.data, b.len);
    wbuf_free(&b);
}

/* write(fd,buf,n) / read(fd,buf,n): a one-iovec fd_write / fd_read through the
   fixed low scratch (iovec at 8/12, result at 16), matching the backend seam */
static void
seam_rw_body(struct wbuf *code, int is_read)
{
    struct wbuf b;
    wbuf_init(&b);
    ib_const(&b, 8);  ib_localget(&b, 1); ib_store(&b);
    ib_const(&b, 12); ib_localget(&b, 2); ib_store(&b);
    ib_localget(&b, 0);                 /* fd */
    ib_const(&b, 8);                    /* iovs */
    ib_const(&b, 1);                    /* iovs_len */
    ib_const(&b, 16);                   /* result pointer */
    ib_call(&b, is_read ? 1 : 0);       /* fd_read / fd_write */
    wbuf_byte(&b, 0x1A);                /* drop */
    if (is_read) {
        ib_const(&b, 16);
        ib_load(&b);                    /* bytes read */
    } else {
        ib_localget(&b, 2);             /* n */
    }
    emit_body(code, &b, 0);
    wbuf_free(&b);
}

/* _exit(code) / exit(code): proc_exit then unreachable (it does not return) */
static void
seam_exit_body(struct wbuf *code)
{
    struct wbuf b;
    wbuf_init(&b);
    ib_localget(&b, 0);
    ib_call(&b, 2);                     /* proc_exit */
    wbuf_byte(&b, 0x00);               /* unreachable */
    emit_body(code, &b, 0);
    wbuf_free(&b);
}

/* __libc_sbrk(incr): the malloc memory seam.  A break pointer lives in global
   BRK_GLOBAL, starting at heap_base (just past the shadow stack).  It bumps the
   break by incr bytes, growing linear memory with memory.grow when the new
   break passes the current size, and returns the old break (or -1 on failure).
   locals: 0=incr (param), 1=old, 2=new. */
static void
seam_sbrk_body(struct wbuf *code, int heap_base)
{
    struct wbuf b;
    wbuf_init(&b);

    ib_globalget(&b, BRK_GLOBAL); ib_localset(&b, 1);           /* old = brk */
    ib_localget(&b, 1); ib_localget(&b, 0); ib_add(&b); ib_localset(&b, 2); /* new = old+incr */

    /* refuse a break below the heap base (covers an over-large shrink) */
    ib_localget(&b, 2); ib_const(&b, heap_base); ib_ltu(&b);
    ib_if(&b);
        ib_const(&b, -1); ib_return(&b);
    ib_end(&b);

    /* grow when the new break passes the current memory size */
    ib_localget(&b, 2);
    ib_memsize(&b); ib_const(&b, 16); ib_shl(&b);              /* size in bytes */
    ib_gtu(&b);
    ib_if(&b);
        /* pages = ((new - size_bytes) + 65535) >> 16 */
        ib_localget(&b, 2);
        ib_memsize(&b); ib_const(&b, 16); ib_shl(&b);
        ib_sub(&b);
        ib_const(&b, 65535); ib_add(&b);
        ib_const(&b, 16); ib_shru(&b);
        ib_memgrow(&b);
        ib_const(&b, -1); ib_eq(&b);                          /* grow failed? */
        ib_if(&b);
            ib_const(&b, -1); ib_return(&b);
        ib_end(&b);
    ib_end(&b);

    ib_localget(&b, 2); ib_globalset(&b, BRK_GLOBAL);          /* brk = new */
    ib_localget(&b, 1);                                        /* return old */

    emit_body(code, &b, 2);
    wbuf_free(&b);
}

/* _start: call main (argc=argv=0 when it takes them), run the exit finalizer
   when the libc provides one, and proc_exit main's result (0 when main returns
   void).  The exit code sits on the operand stack across the fini call, which
   is void(void) and leaves the stack beneath it untouched. */
static void
seam_start_body(struct wbuf *code, int main_idx, int main_np, int main_nr,
                int fini_idx)
{
    struct wbuf b;
    wbuf_init(&b);
    if (main_np >= 2) {
        ib_const(&b, 0);
        ib_const(&b, 0);
    }
    ib_call(&b, main_idx);
    if (!main_nr)
        ib_const(&b, 0);
    if (fini_idx >= 0)
        ib_call(&b, fini_idx);          /* __libc_fini: atexit + stream flush */
    ib_call(&b, 2);                     /* proc_exit */
    emit_body(code, &b, 0);
    wbuf_free(&b);
}

void
wlink(struct wobj **objs, int nobj, const char **exports, int nexports,
      int entry, const char *outpath)
{
    int *base;              /* [nobj] output index of each object's first function */
    int **typemap;          /* [nobj][ntypes] object-local -> output type index */
    int *dbase;             /* [nobj] linear-memory base of each object's data */
    int totalfuncs = 0, totalsyms = 0, i, o, k;
    int stack_top, mem_pages, data_end, data_addr_next;
    int maxtypes = 0, nfixed, nimports, start_idx = -1, main_idx = -1;
    int fini_idx = -1;
    int maxfi = 0;
    int main_np = 0, main_nr = 0, out_nfuncs, heap_base, nglobals;
    int rw_tidx = 0, pe_tidx = 0, fw_tidx = 0, vv_tidx = 0, si_tidx = 0;
    struct wbuf mod, sec, code;
    FILE *out;

    /* a link with an entry synthesizes the WASI runtime: three preview1 imports
       occupy the low function indices, and main must be present */
    g_synth = entry;
    nfixed = g_synth ? 3 : 0;            /* fd_write / fd_read / proc_exit */
    for (k = 0; k < NSEAM; k++)
        g_seam_outidx[k] = -1;
    g_nseam = 0;

    /* pre-pass: collect the extra WASI imports the objects declare, deduped by
       (module, field), so their count is known before the program functions are
       numbered.  They occupy the output indices right after the fixed seam
       imports; the type index is filled once the typemap is built. */
    for (o = 0; o < nobj; o++)
        maxfi += objs[o]->nfuncimports;
    g_owasi = xmalloc((size_t)(maxfi > 0 ? maxfi : 1) * sizeof(*g_owasi));
    g_nowasi = 0;
    for (o = 0; o < nobj; o++) {
        struct wobj *ob = objs[o];
        for (k = 0; k < ob->nsyms; k++) {
            struct wsymbol *s = &ob->syms[k];
            int idx = s->index;
            if (s->kind != WASM_SYMTAB_FUNCTION || !(s->flags & WASM_SYM_UNDEFINED))
                continue;
            if (idx >= ob->nfuncimports ||
                strcmp(ob->funcimport_module[idx], "env") == 0)
                continue;
            if (owasi_find(ob->funcimport_module[idx], ob->funcimport_name[idx]) >= 0)
                continue;
            g_owasi[g_nowasi].module = ob->funcimport_module[idx];
            g_owasi[g_nowasi].field = ob->funcimport_name[idx];
            g_owasi[g_nowasi].src_obj = o;
            g_owasi[g_nowasi].src_ltype = ob->funcimport_type[idx];
            g_owasi[g_nowasi].outidx = nfixed + g_nowasi;
            g_nowasi++;
        }
    }
    nimports = nfixed + g_nowasi;

    /* assign output function indices (after the imports) and tally the types */
    base = xmalloc((size_t)nobj * sizeof(int));
    for (o = 0; o < nobj; o++) {
        base[o] = nimports + totalfuncs;
        totalfuncs += objs[o]->ndeffns;
        totalsyms += objs[o]->nsyms;
        maxtypes += objs[o]->ntypes;
    }
    g_prog_end = nimports + totalfuncs;
    g_otypes = xmalloc((size_t)(maxtypes + 8) * sizeof(*g_otypes));
    g_notypes = 0;

    typemap = xmalloc((size_t)nobj * sizeof(int *));
    for (o = 0; o < nobj; o++) {
        struct wobj *ob = objs[o];
        typemap[o] = xmalloc((size_t)(ob->ntypes > 0 ? ob->ntypes : 1) * sizeof(int));
        for (k = 0; k < ob->ntypes; k++)
            typemap[o][k] = otype_add(&ob->types[k]);
    }

    /* now that the global type table exists, resolve each extra WASI import's
       output type index through its declaring object's typemap */
    for (k = 0; k < g_nowasi; k++)
        g_owasi[k].tidx = typemap[g_owasi[k].src_obj][g_owasi[k].src_ltype];

    /* the global map: every non-local defined function symbol */
    g_gfuncs = xmalloc((size_t)(totalfuncs > 0 ? totalfuncs : 1) * sizeof(*g_gfuncs));
    g_ngfuncs = 0;
    for (o = 0; o < nobj; o++) {
        struct wobj *ob = objs[o];
        for (k = 0; k < ob->nsyms; k++) {
            struct wsymbol *s = &ob->syms[k];
            if (s->kind != WASM_SYMTAB_FUNCTION || (s->flags & WASM_SYM_UNDEFINED))
                continue;
            if (s->flags & WASM_SYM_BINDING_LOCAL)
                continue;
            if (gfunc_find(s->name) >= 0)
                die("duplicate symbol: %s", s->name);
            g_gfuncs[g_ngfuncs].name = s->name;
            g_gfuncs[g_ngfuncs].outidx = base[o] + (s->index - ob->nfuncimports);
            g_ngfuncs++;
        }
    }

    /* locate main and its signature for the synthesized _start */
    if (g_synth) {
        int found = 0;
        main_idx = gfunc_find("main");
        if (main_idx < 0)
            die("no main: an entry link needs a defined main (or pass --no-entry)");
        for (o = 0; o < nobj && !found; o++) {
            struct wobj *ob = objs[o];
            for (k = 0; k < ob->nsyms; k++) {
                struct wsymbol *s = &ob->syms[k];
                struct wtype *t;
                if (s->kind != WASM_SYMTAB_FUNCTION || (s->flags & WASM_SYM_UNDEFINED))
                    continue;
                if (strcmp(s->name, "main") != 0)
                    continue;
                t = &ob->types[ob->func_type[s->index - ob->nfuncimports]];
                main_np = t->np;
                main_nr = t->nr;
                found = 1;
                break;
            }
        }
        /* a hosted libc defines __libc_fini (atexit handlers + stream flush);
           _start runs it before proc_exit.  A freestanding link omits fini.c,
           gfunc_find returns -1, and _start exits straight away. */
        fini_idx = gfunc_find("__libc_fini");
    }

    /* lay out each object's data segment above the data base, honoring its
       alignment, and record the running top */
    dbase = xmalloc((size_t)nobj * sizeof(int));
    data_addr_next = WLD_DATA_BASE;
    for (o = 0; o < nobj; o++) {
        struct wobj *ob = objs[o];
        if (ob->has_data) {
            int align = 1 << ob->data_p2align;
            data_addr_next = (data_addr_next + align - 1) & ~(align - 1);
            dbase[o] = data_addr_next;
            data_addr_next += (int)ob->datalen;
        } else {
            dbase[o] = data_addr_next;
        }
    }

    /* the global map: every non-local defined data symbol.  Sized by the total
       symbol count, since a data-heavy object (static aggregates, const arrays)
       defines far more data symbols than the program has functions. */
    g_gdata = xmalloc((size_t)(totalsyms + 1) * sizeof(*g_gdata));
    g_ngdata = 0;
    for (o = 0; o < nobj; o++) {
        struct wobj *ob = objs[o];
        for (k = 0; k < ob->nsyms; k++) {
            struct wsymbol *s = &ob->syms[k];
            if (s->kind != WASM_SYMTAB_DATA || (s->flags & WASM_SYM_UNDEFINED))
                continue;
            if (s->flags & WASM_SYM_BINDING_LOCAL)
                continue;
            if (gdata_find(s->name) >= 0)
                die("duplicate symbol: %s", s->name);
            g_gdata[g_ngdata].name = s->name;
            g_gdata[g_ngdata].addr = dbase[o] + s->off;
            g_ngdata++;
        }
    }

    /* the indirect function table, filled on demand by the relocations below */
    g_tslots = xmalloc((size_t)(totalfuncs > 0 ? totalfuncs : 1) * sizeof(int));
    g_ntslots = 0;

    /* apply the data relocations: patch each object's data image in place */
    for (o = 0; o < nobj; o++) {
        struct wobj *ob = objs[o];
        for (i = 0; i < ob->ndrelocs; i++) {
            struct wreloc *r = &ob->drelocs[i];
            size_t idx = r->off - ob->data_bytes_off;
            if (idx + 4 > ob->datalen)
                die("%s: data relocation past segment end", ob->path);
            switch (r->type) {
            case R_WASM_MEMORY_ADDR_I32:
                patch4(ob->data + idx,
                       (uint32_t)data_addr(ob, dbase, o, r->index, r->addend));
                break;
            case R_WASM_TABLE_INDEX_I32:
                patch4(ob->data + idx,
                       (uint32_t)table_slot(func_outidx(ob, base, o, r->index)));
                break;
            default:
                die("data relocation type %d not yet supported", r->type);
            }
        }
    }

    /* apply the code relocations in each object */
    for (o = 0; o < nobj; o++) {
        struct wobj *ob = objs[o];
        for (i = 0; i < ob->ncrelocs; i++) {
            struct wreloc *r = &ob->crelocs[i];
            unsigned char *at = ob->code + r->off;
            switch (r->type) {
            case R_WASM_FUNCTION_INDEX_LEB:
                patch5(at, (uint64_t)func_outidx(ob, base, o, r->index));
                break;
            case R_WASM_GLOBAL_INDEX_LEB:
                patch5(at, 0);          /* the one global: __stack_pointer */
                break;
            case R_WASM_TYPE_INDEX_LEB:
                patch5(at, (uint64_t)typemap[o][r->index]);
                break;
            case R_WASM_MEMORY_ADDR_SLEB:
                patch5(at, (uint64_t)data_addr(ob, dbase, o, r->index, r->addend));
                break;
            case R_WASM_TABLE_INDEX_SLEB:
                patch5(at, (uint64_t)table_slot(func_outidx(ob, base, o, r->index)));
                break;
            default:
                die("relocation type %d not yet supported", r->type);
            }
        }
    }

    /* memory layout: the merged data, then the shadow stack, then the heap grows
       up from just past the stack (sbrk raises the break and grows memory) */
    data_end = (data_addr_next + 15) & ~15;
    stack_top = data_end + WLD_STACK_BYTES;
    heap_base = stack_top;
    mem_pages = (stack_top + 65535) / 65536;

    /* the runtime functions sit past the seams; intern their signatures (and
       the WASI imports') so the type table is complete before it is written */
    out_nfuncs = totalfuncs + g_nseam;
    if (g_synth) {
        start_idx = g_prog_end + g_nseam;
        out_nfuncs += 1;                /* _start */
        fw_tidx = otype_sig(4, 1);      /* fd_write / fd_read */
        pe_tidx = otype_sig(1, 0);      /* proc_exit, _exit, exit */
        vv_tidx = otype_sig(0, 0);      /* _start */
        if (g_seam_outidx[0] >= 0 || g_seam_outidx[1] >= 0)
            rw_tidx = otype_sig(3, 1);  /* write / read */
        if (g_seam_outidx[SEAM_SBRK] >= 0)
            si_tidx = otype_sig(1, 1);  /* __libc_sbrk */
    }
    nglobals = 1 + (g_seam_outidx[SEAM_SBRK] >= 0 ? 1 : 0);

    /* ---- write the module ---- */
    wbuf_init(&mod);
    wbuf_byte(&mod, 0x00); wbuf_byte(&mod, 0x61);
    wbuf_byte(&mod, 0x73); wbuf_byte(&mod, 0x6D);
    wbuf_u32(&mod, 1);

    /* Type (1) */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)g_notypes);
    for (k = 0; k < g_notypes; k++) {
        struct wtype *t = &g_otypes[k];
        int j;
        wbuf_byte(&sec, 0x60);
        wbuf_uleb(&sec, (uint64_t)t->np);
        for (j = 0; j < t->np; j++)
            wbuf_byte(&sec, t->p[j]);
        wbuf_uleb(&sec, (uint64_t)t->nr);
        for (j = 0; j < t->nr; j++)
            wbuf_byte(&sec, t->r);
    }
    wbuf_section(&mod, 1, &sec);
    wbuf_free(&sec);

    /* Import (2): the fixed WASI preview1 seam imports (when a runtime is built),
       then the extra WASI imports the objects declared */
    if (g_synth || g_nowasi > 0) {
        wbuf_init(&sec);
        wbuf_uleb(&sec, (uint64_t)nimports);
        if (g_synth) {
            static const struct { const char *field; int tidx_is_fw; } wi[3] = {
                { "fd_write", 1 }, { "fd_read", 1 }, { "proc_exit", 0 },
            };
            for (k = 0; k < 3; k++) {
                wbuf_name(&sec, "wasi_snapshot_preview1");
                wbuf_name(&sec, wi[k].field);
                wbuf_byte(&sec, 0x00);      /* function import */
                wbuf_uleb(&sec, (uint64_t)(wi[k].tidx_is_fw ? fw_tidx : pe_tidx));
            }
        }
        for (k = 0; k < g_nowasi; k++) {
            wbuf_name(&sec, g_owasi[k].module);
            wbuf_name(&sec, g_owasi[k].field);
            wbuf_byte(&sec, 0x00);          /* function import */
            wbuf_uleb(&sec, (uint64_t)g_owasi[k].tidx);
        }
        wbuf_section(&mod, 2, &sec);
        wbuf_free(&sec);
    }

    /* Function (3): the type index of every defined function, in index order:
       the program functions, then the synthesized seams, then _start */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)out_nfuncs);
    for (o = 0; o < nobj; o++) {
        struct wobj *ob = objs[o];
        for (k = 0; k < ob->ndeffns; k++)
            wbuf_uleb(&sec, (uint64_t)typemap[o][ob->func_type[k]]);
    }
    for (k = 0; k < g_nseam; k++) {
        int sk = g_seam_order[k];
        wbuf_uleb(&sec, (uint64_t)(sk < 2 ? rw_tidx :
                                   sk == SEAM_SBRK ? si_tidx : pe_tidx));
    }
    if (g_synth)
        wbuf_uleb(&sec, (uint64_t)vv_tidx);
    wbuf_section(&mod, 3, &sec);
    wbuf_free(&sec);

    /* Table (4): the indirect function table, sized to hold every slot */
    if (g_ntslots > 0) {
        wbuf_init(&sec);
        wbuf_uleb(&sec, 1);             /* one table */
        wbuf_byte(&sec, 0x70);          /* funcref */
        wbuf_byte(&sec, 0x00);          /* limits: min only */
        wbuf_uleb(&sec, (uint64_t)(g_ntslots + 1));
        wbuf_section(&mod, 4, &sec);
        wbuf_free(&sec);
    }

    /* Memory (5) */
    wbuf_init(&sec);
    wbuf_uleb(&sec, 1);
    wbuf_byte(&sec, 0x00);
    wbuf_uleb(&sec, (uint64_t)mem_pages);
    wbuf_section(&mod, 5, &sec);
    wbuf_free(&sec);

    /* Global (6): __stack_pointer, then the sbrk break (when synthesized) */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)nglobals);
    wbuf_byte(&sec, 0x7F); wbuf_byte(&sec, 0x01);
    wbuf_byte(&sec, 0x41); wbuf_sleb(&sec, stack_top); wbuf_byte(&sec, 0x0B);
    if (nglobals > 1) {
        wbuf_byte(&sec, 0x7F); wbuf_byte(&sec, 0x01);
        wbuf_byte(&sec, 0x41); wbuf_sleb(&sec, heap_base); wbuf_byte(&sec, 0x0B);
    }
    wbuf_section(&mod, 6, &sec);
    wbuf_free(&sec);

    /* Export (7): memory, _start (when built), and the requested functions */
    wbuf_init(&sec);
    wbuf_uleb(&sec, (uint64_t)(1 + (g_synth ? 1 : 0) + nexports));
    wbuf_name(&sec, "memory");
    wbuf_byte(&sec, 0x02); wbuf_uleb(&sec, 0);
    if (g_synth) {
        wbuf_name(&sec, "_start");
        wbuf_byte(&sec, 0x00); wbuf_uleb(&sec, (uint64_t)start_idx);
    }
    for (i = 0; i < nexports; i++) {
        int v = gfunc_find(exports[i]);
        if (v < 0)
            die("cannot export undefined symbol: %s", exports[i]);
        wbuf_name(&sec, exports[i]);
        wbuf_byte(&sec, 0x00); wbuf_uleb(&sec, (uint64_t)v);
    }
    wbuf_section(&mod, 7, &sec);
    wbuf_free(&sec);

    /* Elem (9): one active segment placing each table function at its slot,
       starting at slot 1 (slot 0 is the reserved null entry) */
    if (g_ntslots > 0) {
        wbuf_init(&sec);
        wbuf_uleb(&sec, 1);             /* one element segment */
        wbuf_byte(&sec, 0x00);          /* active, table 0, funcref */
        wbuf_byte(&sec, 0x41); wbuf_sleb(&sec, 1); wbuf_byte(&sec, 0x0B);
        wbuf_uleb(&sec, (uint64_t)g_ntslots);
        for (k = 0; k < g_ntslots; k++)
            wbuf_uleb(&sec, (uint64_t)g_tslots[k]);
        wbuf_section(&mod, 9, &sec);
        wbuf_free(&sec);
    }

    /* Code (10): the program bodies in object order, then the seam bodies in
       index order, then _start */
    wbuf_init(&code);
    wbuf_uleb(&code, (uint64_t)out_nfuncs);
    for (o = 0; o < nobj; o++) {
        struct wobj *ob = objs[o];
        wbuf_bytes(&code, ob->code + ob->code_entries_off,
                   ob->codelen - ob->code_entries_off);
    }
    for (k = 0; k < g_nseam; k++) {
        int sk = g_seam_order[k];
        if (sk < 2)
            seam_rw_body(&code, sk == 1);   /* write (0) / read (1) */
        else if (sk == SEAM_SBRK)
            seam_sbrk_body(&code, heap_base);
        else
            seam_exit_body(&code);          /* _exit (2) / exit (3) */
    }
    if (g_synth)
        seam_start_body(&code, main_idx, main_np, main_nr, fini_idx);
    wbuf_section(&mod, 10, &code);
    wbuf_free(&code);

    /* Data (11): one active segment holding every object's merged data */
    if (data_addr_next > WLD_DATA_BASE) {
        int span = data_addr_next - WLD_DATA_BASE;
        unsigned char *img = xcalloc(1, (size_t)span);
        for (o = 0; o < nobj; o++) {
            struct wobj *ob = objs[o];
            if (ob->has_data)
                memcpy(img + (dbase[o] - WLD_DATA_BASE), ob->data, ob->datalen);
        }
        wbuf_init(&sec);
        wbuf_uleb(&sec, 1);
        wbuf_byte(&sec, 0x00);          /* active, memory 0 */
        wbuf_byte(&sec, 0x41); wbuf_sleb(&sec, WLD_DATA_BASE); wbuf_byte(&sec, 0x0B);
        wbuf_uleb(&sec, (uint64_t)span);
        wbuf_bytes(&sec, img, (size_t)span);
        wbuf_section(&mod, 11, &sec);
        wbuf_free(&sec);
        free(img);
    }

    out = fopen(outpath, "wb");
    if (!out)
        die("cannot write %s", outpath);
    if (fwrite(mod.data, 1, mod.len, out) != mod.len)
        die("write failed on %s", outpath);
    fclose(out);
    wbuf_free(&mod);
}
