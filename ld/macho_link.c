/* macho_link.c : lay out a PIE MH_EXECUTE from the input objects, apply the
 * arm64 relocations, bind the libSystem imports through stubs and a __got via
 * dyld chained fixups, and self-sign with an ad-hoc code signature.
 *
 * Layout (arm64, 16 KiB segment pages):
 *   __PAGEZERO  (no file)         4 GiB guard
 *   __TEXT  r-x  @ 0x100000000    mach header + load commands, __text, __stubs
 *   __DATA  rw                    __got, __const, __data, __bss(zerofill)
 *   __LINKEDIT r                  chained fixups, symbol table, string table, signature
 *
 * The imports and the absolute __DATA pointers are a dyld fixup chain
 * (LC_DYLD_CHAINED_FIXUPS, DYLD_CHAINED_PTR_64_OFFSET): each __got slot is a
 * bind word naming an import, each absolute pointer to our own symbols a rebase
 * word, all linked per 16 KiB page by a `next` field (no indirect symbol table,
 * no dyld_stub_binder, no bind/rebase opcode stream).  Each `bl _import` is
 * redirected to a three-instruction stub that loads its __got slot and
 * branches. */

#include "macho_ld.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TEXT_BASE   0x100000000ull
#define PAGEZERO_SZ 0x100000000ull
#define SEG_ALIGN   0x4000ull       /* 16 KiB arm64 segment page */
#define CS_PAGE     4096            /* code-signing hash page */

/****************************************************************
 * growable output buffer
 ****************************************************************/

struct buf {
    uint8_t *d;
    size_t len, cap;
};

static void
bput(struct buf *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        while (b->len + n > b->cap)
            b->cap = b->cap ? b->cap * 2 : 4096;
        b->d = realloc(b->d, b->cap);
    }
    if (p)
        memcpy(b->d + b->len, p, n);
    else
        memset(b->d + b->len, 0, n);
    b->len += n;
}

static void bu8(struct buf *b, uint8_t v)  { bput(b, &v, 1); }

static void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void
w32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void
w64(uint8_t *p, uint64_t v)
{
    w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32));
}

/* Code-signing blobs are big-endian. */
static void
w32be(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void
w64be(uint8_t *p, uint64_t v)
{
    w32be(p, (uint32_t)(v >> 32)); w32be(p + 4, (uint32_t)v);
}

static uint32_t
r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t
r64(const uint8_t *p)
{
    return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32);
}

static uint64_t
align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/****************************************************************
 * merged sections
 ****************************************************************/

/* Each placed kind is the concatenation of that kind across the inputs.  We
   keep, per object and kind, the offset of that object's slice in the merge. */
struct merged {
    uint8_t *data;              /* NULL for bss */
    uint64_t size;
    int align;
    uint64_t out_addr;          /* assigned vm address */
    uint64_t out_fileoff;       /* file offset (0 for bss) */
    uint64_t *obj_off;          /* per-object slice offset */
};

struct layout {
    struct merged m[MSEC_KIND_COUNT];
    uint64_t stubs_addr, stubs_fileoff, stubs_size;
    uint64_t got_addr, got_fileoff, got_size;
};

static void
merge_kind(struct macho_link *ld, struct layout *L, int kind)
{
    struct merged *mg = &L->m[kind];
    int i;
    uint64_t off = 0;
    int maxal = 1;

    mg->obj_off = arena_alloc(&ld->arena, (size_t)(ld->nobj ? ld->nobj : 1)
                              * sizeof(uint64_t));
    for (i = 0; i < ld->nobj; i++) {
        struct min_sec *s = &ld->objs[i].secs[kind];
        mg->obj_off[i] = 0;
        if (s->kind < 0)
            continue;
        if (s->align > maxal)
            maxal = s->align;
        off = align_up(off, (uint64_t)(s->align ? s->align : 1));
        mg->obj_off[i] = off;
        off += s->size;
    }
    mg->size = off;
    mg->align = maxal;
    if (off > 0 && kind != MSEC_BSS) {
        mg->data = arena_alloc(&ld->arena, (size_t)off);
        memset(mg->data, 0, (size_t)off);
        for (i = 0; i < ld->nobj; i++) {
            struct min_sec *s = &ld->objs[i].secs[kind];
            if (s->kind < 0 || !s->data)
                continue;
            memcpy(mg->data + mg->obj_off[i], s->data, (size_t)s->size);
        }
    }
}

/* Final vm address of a defined global symbol. */
static uint64_t
gsym_addr(struct macho_link *ld, struct layout *L, int g)
{
    struct min_gsym *s = &ld->gsyms[g];
    if (!s->defined)
        return 0;
    return L->m[s->seckind].out_addr
         + L->m[s->seckind].obj_off[s->obj]
         + s->value;
}

/****************************************************************
 * relocation application
 ****************************************************************/

static void
patch32(uint8_t *p, uint32_t clearmask, uint32_t val)
{
    uint32_t insn = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                  | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    insn = (insn & ~clearmask) | (val & clearmask);
    w32(p, insn);
}

/* A rebase site recorded during relocation: an absolute pointer in __DATA that
   dyld must slide. */
struct rebase_site { int kind; uint64_t off; };   /* off within its merged kind */

static void
apply_relocs(struct macho_link *ld, struct layout *L,
             struct rebase_site **rb_out, int *nrb_out)
{
    struct rebase_site *rb = NULL;
    int nrb = 0, rbcap = 0;
    int oi, kind;

    for (oi = 0; oi < ld->nobj; oi++) {
        for (kind = 0; kind < MSEC_KIND_COUNT; kind++) {
            struct min_sec *s = &ld->objs[oi].secs[kind];
            struct merged *mg = &L->m[kind];
            int r;
            if (s->kind < 0 || !mg->data)
                continue;
            for (r = 0; r < s->nrelocs; r++) {
                struct min_reloc *rl = &s->relocs[r];
                uint64_t site_off = mg->obj_off[oi] + rl->offset;
                uint64_t site_addr = mg->out_addr + site_off;
                uint8_t *p = mg->data + site_off;
                struct min_gsym *t = &ld->gsyms[rl->target_sym];
                uint64_t target;

                if (t->is_import && rl->kind == ARM64_RELOC_BRANCH26)
                    target = L->stubs_addr + (uint64_t)t->stub_index * 12;
                else
                    target = gsym_addr(ld, L, rl->target_sym) + (uint64_t)rl->addend;

                switch (rl->kind) {
                case ARM64_RELOC_BRANCH26: {
                    int64_t disp = (int64_t)target - (int64_t)site_addr;
                    patch32(p, 0x03FFFFFFu, (uint32_t)((disp >> 2) & 0x03FFFFFF));
                    break;
                }
                case ARM64_RELOC_PAGE21: {
                    int64_t d = (int64_t)(target & ~0xfffull)
                              - (int64_t)(site_addr & ~0xfffull);
                    int64_t pages = d >> 12;
                    uint32_t immlo = (uint32_t)(pages & 3) << 29;
                    uint32_t immhi = (uint32_t)((pages >> 2) & 0x7FFFF) << 5;
                    patch32(p, (3u << 29) | (0x7FFFFu << 5), immlo | immhi);
                    break;
                }
                case ARM64_RELOC_PAGEOFF12: {
                    /* the add's imm12 is 12 bits at bits 10..21 */
                    uint32_t lo = (uint32_t)(target & 0xfff);
                    patch32(p, 0xFFFu << 10, lo << 10);
                    break;
                }
                case ARM64_RELOC_UNSIGNED: {
                    /* absolute pointer: the assembler left the addend inline in
                       the bytes (Mach-O has no addend field), so fold it in and
                       store the unslid vm address.  If the target is one of our
                       symbols, record a rebase so dyld slides it. */
                    int64_t inl = (rl->length == 3)
                                ? (int64_t)r64(p) : (int64_t)(int32_t)r32(p);
                    uint64_t base = t->is_import ? 0 : gsym_addr(ld, L, rl->target_sym);
                    target = base + (uint64_t)(inl + rl->addend);
                    if (rl->length == 3)
                        w64(p, target);
                    else
                        w32(p, (uint32_t)target);
                    /* __DWARF holds absolute (unslid) addresses that dyld never
                       maps or slides, so it records no rebase; only the loadable
                       __const/__data pointers join the fixup chain. */
                    if (!t->is_import && !msec_is_debug(kind)) {
                        if (nrb == rbcap) {
                            rbcap = rbcap ? rbcap * 2 : 16;
                            rb = realloc(rb, (size_t)rbcap
                                         * sizeof(struct rebase_site));
                        }
                        rb[nrb].kind = kind;
                        rb[nrb].off = site_off;
                        nrb++;
                    }
                    break;
                }
                default:
                    die("macho: unhandled relocation kind %d", rl->kind);
                }
            }
        }
    }
    *rb_out = rb;
    *nrb_out = nrb;
}

/****************************************************************
 * the linker
 ****************************************************************/

void
macho_link_and_write(struct macho_link *ld, const char *outpath,
                     const char *entry)
{
    struct layout L;
    int i, g;
    int entry_g;

    memset(&L, 0, sizeof(L));

    /* collect imports (undefined globals) in first-seen order */
    ld->imports = arena_alloc(&ld->arena,
                              (size_t)(ld->ngsym ? ld->ngsym : 1) * sizeof(int));
    ld->nimport = 0;
    for (g = 0; g < ld->ngsym; g++) {
        if (!ld->gsyms[g].defined) {
            ld->gsyms[g].is_import = 1;
            ld->gsyms[g].stub_index = ld->nimport;
            ld->gsyms[g].got_index = ld->nimport;
            ld->imports[ld->nimport++] = g;
        }
    }

    entry_g = macho_gsym(ld, entry);
    if (!ld->gsyms[entry_g].defined)
        die("macho: entry symbol %s is undefined", entry);

    for (i = 0; i < MSEC_KIND_COUNT; i++)
        merge_kind(ld, &L, i);

    L.stubs_size = (uint64_t)ld->nimport * 12;
    L.got_size = (uint64_t)ld->nimport * 8;

    /* ---- compute the load-command size, so we know where __text starts ----
       __DATA_CONST (got + const) and __DATA (data + bss) are each emitted only
       when non-empty; presence is known here from the section sizes. */
    int text_nsects = 1 + (L.stubs_size ? 1 : 0);          /* __text [+ __stubs] */
    int dc_nsects   = (L.got_size ? 1 : 0) + (L.m[MSEC_CONST].size ? 1 : 0);
    int data_nsects = (L.m[MSEC_DATA].size ? 1 : 0) + (L.m[MSEC_BSS].size ? 1 : 0);
    int dw_nsects   = (L.m[MSEC_DEBUG_LINE].size ? 1 : 0)
                    + (L.m[MSEC_DEBUG_INFO].size ? 1 : 0)
                    + (L.m[MSEC_DEBUG_ABBREV].size ? 1 : 0);
    int have_dc   = dc_nsects   ? 1 : 0;
    int have_data = data_nsects ? 1 : 0;
    int have_dw   = dw_nsects   ? 1 : 0;
    const char *dyld_path = "/usr/lib/dyld";
    const char *dylib_path = "/usr/lib/libSystem.B.dylib";
    uint32_t dyld_lc = (uint32_t)align_up(12 + strlen(dyld_path) + 1, 8);
    uint32_t dylib_lc = (uint32_t)align_up(24 + strlen(dylib_path) + 1, 8);

    uint32_t sizeofcmds =
        72                                  /* PAGEZERO */
      + 72 + (uint32_t)text_nsects * 80     /* TEXT */
      + (have_dc   ? 72 + (uint32_t)dc_nsects   * 80 : 0)  /* DATA_CONST */
      + (have_data ? 72 + (uint32_t)data_nsects * 80 : 0)  /* DATA */
      + (have_dw   ? 72 + (uint32_t)dw_nsects   * 80 : 0)  /* DWARF (debug) */
      + 72                                  /* LINKEDIT */
      + 16                                  /* DYLD_CHAINED_FIXUPS */
      + 24                                  /* SYMTAB */
      + 80                                  /* DYSYMTAB */
      + dyld_lc                             /* LOAD_DYLINKER */
      + 24                                  /* UUID */
      + 24                                  /* BUILD_VERSION */
      + 24                                  /* MAIN */
      + dylib_lc                            /* LOAD_DYLIB */
      + 16;                                 /* CODE_SIGNATURE */
    /* __PAGEZERO, __TEXT, [__DATA_CONST], [__DATA], [__DWARF], __LINKEDIT
       + 9 others */
    uint32_t ncmds = (uint32_t)(3 + have_dc + have_data + have_dw + 9);
    uint64_t hdr_size = 32 + sizeofcmds;

    /* ---- __TEXT layout ---- */
    uint64_t t = hdr_size;
    t = align_up(t, 4);
    L.m[MSEC_TEXT].out_fileoff = t;
    L.m[MSEC_TEXT].out_addr = TEXT_BASE + t;
    t += L.m[MSEC_TEXT].size;
    if (L.stubs_size) {
        t = align_up(t, 4);
        L.stubs_fileoff = t;
        L.stubs_addr = TEXT_BASE + t;
        t += L.stubs_size;
    }
    uint64_t text_filesize = t;
    uint64_t text_vmsize = align_up(t, SEG_ALIGN);

    /* ---- __DATA_CONST layout (got + const: pointer-bearing, mapped read-only
       after dyld applies the fixup chain) ---- */
    uint64_t dc_vmaddr = TEXT_BASE + text_vmsize;
    uint64_t dc_fileoff = text_vmsize;
    uint64_t dc = 0;
    if (L.got_size) {
        dc = align_up(dc, 8);
        L.got_fileoff = dc_fileoff + dc;
        L.got_addr = dc_vmaddr + dc;
        dc += L.got_size;
    }
    if (L.m[MSEC_CONST].size) {
        dc = align_up(dc, (uint64_t)L.m[MSEC_CONST].align);
        L.m[MSEC_CONST].out_fileoff = dc_fileoff + dc;
        L.m[MSEC_CONST].out_addr = dc_vmaddr + dc;
        dc += L.m[MSEC_CONST].size;
    }
    uint64_t dc_filesize = dc;
    uint64_t dc_vmsize = align_up(dc, SEG_ALIGN);

    /* ---- __DATA layout (data + bss: mutable) ---- */
    uint64_t data_vmaddr = dc_vmaddr + dc_vmsize;
    uint64_t data_fileoff = dc_fileoff + dc_vmsize;
    uint64_t d = 0;
    if (L.m[MSEC_DATA].size) {
        d = align_up(d, (uint64_t)L.m[MSEC_DATA].align);
        L.m[MSEC_DATA].out_fileoff = data_fileoff + d;
        L.m[MSEC_DATA].out_addr = data_vmaddr + d;
        d += L.m[MSEC_DATA].size;
    }
    uint64_t data_filesize = d;                 /* bss adds no file bytes */
    if (L.m[MSEC_BSS].size) {
        d = align_up(d, (uint64_t)L.m[MSEC_BSS].align);
        L.m[MSEC_BSS].out_addr = data_vmaddr + d;
        d += L.m[MSEC_BSS].size;
    }
    uint64_t data_vmsize = align_up(d, SEG_ALIGN);

    /* ---- __DWARF layout (non-loaded debug sections, byte-aligned, carried
       through from a -g object) ---- */
    uint64_t dw_vmaddr = data_vmaddr + data_vmsize;
    uint64_t dw_fileoff = data_fileoff + data_vmsize;
    uint64_t dw = 0;
    {
        int dk;
        for (dk = MSEC_DEBUG_FIRST; dk < MSEC_KIND_COUNT; dk++) {
            if (!L.m[dk].size)
                continue;
            L.m[dk].out_fileoff = dw_fileoff + dw;
            L.m[dk].out_addr = dw_vmaddr + dw;
            dw += L.m[dk].size;
        }
    }
    uint64_t dw_filesize = dw;
    uint64_t dw_vmsize = have_dw ? align_up(dw, SEG_ALIGN) : 0;

    /* segments in load order; a data segment is omitted when empty.  The
       chained-fixups starts-in-image is indexed by this order.  __DWARF (if
       present) sits between __DATA and __LINKEDIT and carries no fixups. */
    int seg_idx = 2;                            /* after __PAGEZERO, __TEXT */
    int seg_dc  = have_dc   ? seg_idx++ : -1;
    int seg_dat = have_data ? seg_idx++ : -1;
    int seg_dw  = have_dw   ? seg_idx++ : -1;
    int seg_count = seg_idx + 1;                /* + __LINKEDIT */
    (void)seg_dw;

    /* ---- generate the stubs now that addresses are known ---- */
    uint8_t *stub_bytes = NULL;
    if (L.stubs_size) {
        stub_bytes = arena_alloc(&ld->arena, (size_t)L.stubs_size);
        for (i = 0; i < ld->nimport; i++) {
            uint64_t saddr = L.stubs_addr + (uint64_t)i * 12;
            uint64_t gaddr = L.got_addr + (uint64_t)i * 8;
            int64_t pages = (int64_t)(gaddr & ~0xfffull)
                          - (int64_t)(saddr & ~0xfffull);
            pages >>= 12;
            uint32_t adrp = 0x90000000u | 16;   /* adrp x16, page */
            adrp |= (uint32_t)(pages & 3) << 29;
            adrp |= (uint32_t)((pages >> 2) & 0x7FFFF) << 5;
            uint32_t ldr = 0xF9400000u | (16) | (16 << 5);  /* ldr x16,[x16,#off] */
            ldr |= (uint32_t)((gaddr & 0xfff) >> 3) << 10;
            uint32_t br = 0xD61F0000u | (16 << 5);          /* br x16 */
            w32(stub_bytes + i * 12 + 0, adrp);
            w32(stub_bytes + i * 12 + 4, ldr);
            w32(stub_bytes + i * 12 + 8, br);
        }
    }

    /* ---- apply relocations ---- */
    struct rebase_site *rb;
    int nrb;
    apply_relocs(ld, &L, &rb, &nrb);

    /* ---- chained fixups ----
       The __got slots (binds) and the absolute __DATA pointers (rebases) become
       a dyld pointer chain: each site holds a 64-bit fixup word whose `next`
       field (12 bits at bit 51, in 4-byte strides) links it to the following
       fixup on the same page, 0 ending a page's chain.  The __got starts as
       bind words, and apply_relocs left each rebase site's unslid vm address in
       place, which we rewrite as a rebase word.  This replaces the classic
       rebase/bind opcode streams; the format is DYLD_CHAINED_PTR_64_OFFSET
       (target is an image-relative offset dyld adds to the load address). */
    uint8_t *got_bytes = NULL;
    if (L.got_size) {
        got_bytes = arena_alloc(&ld->arena, (size_t)L.got_size);
        memset(got_bytes, 0, (size_t)L.got_size);
    }

    /* Split the fixups by segment: the __got binds and the __const rebases live
       in __DATA_CONST, the __data rebases in __DATA.  Each site's seg_off is
       relative to its own segment's vm address. */
    struct fixsite { uint64_t seg_off; uint64_t word; uint8_t *slot; };
    struct fixsite *dcfx = arena_alloc(&ld->arena,
        (size_t)((ld->nimport + nrb) ? (ld->nimport + nrb) : 1) * sizeof(struct fixsite));
    struct fixsite *dfx = arena_alloc(&ld->arena,
        (size_t)(nrb ? nrb : 1) * sizeof(struct fixsite));
    int ndc = 0, ndat = 0;
    for (i = 0; i < ld->nimport; i++) {         /* __got: dyld_chained_ptr_64_bind */
        dcfx[ndc].seg_off = (L.got_addr - dc_vmaddr) + (uint64_t)i * 8;
        dcfx[ndc].word = ((uint64_t)1 << 63)            /* bind = 1 */
                       | ((uint64_t)i & 0xFFFFFF);      /* ordinal (24 bits) */
        dcfx[ndc].slot = got_bytes + i * 8;
        ndc++;
    }
    for (i = 0; i < nrb; i++) {                 /* absolute pointer: rebase word */
        struct merged *mg = &L.m[rb[i].kind];
        uint8_t *slot = mg->data + rb[i].off;
        uint64_t off = r64(slot) - TEXT_BASE;           /* image-relative target */
        uint64_t word = (off & 0xFFFFFFFFFULL)          /* target (36 bits) */
                      | (((off >> 56) & 0xFF) << 36);   /* high8 */
        if (rb[i].kind == MSEC_CONST) {
            dcfx[ndc].seg_off = (mg->out_addr - dc_vmaddr) + rb[i].off;
            dcfx[ndc].word = word; dcfx[ndc].slot = slot; ndc++;
        } else {
            dfx[ndat].seg_off = (mg->out_addr - data_vmaddr) + rb[i].off;
            dfx[ndat].word = word; dfx[ndat].slot = slot; ndat++;
        }
    }

    /* the chained segments, in load order; each is sorted by offset and linked
       into a per-16 KiB-page chain (next in 4-byte strides, 0 ends a page). */
    struct chseg { int seg_index; uint64_t vmaddr; struct fixsite *fx; int nf;
                   uint64_t pages; uint32_t info_off; } cs[2];
    int ncs = 0;
    if (seg_dc  >= 0 && ndc)  { cs[ncs].seg_index = seg_dc;  cs[ncs].vmaddr = dc_vmaddr;
                                cs[ncs].fx = dcfx; cs[ncs].nf = ndc;  ncs++; }
    if (seg_dat >= 0 && ndat) { cs[ncs].seg_index = seg_dat; cs[ncs].vmaddr = data_vmaddr;
                                cs[ncs].fx = dfx;  cs[ncs].nf = ndat; ncs++; }
    for (i = 0; i < ncs; i++) {
        struct fixsite *v = cs[i].fx;
        int n = cs[i].nf, a, b;
        for (a = 1; a < n; a++) {               /* insertion sort by seg_off */
            struct fixsite t = v[a];
            b = a - 1;
            while (b >= 0 && v[b].seg_off > t.seg_off) { v[b + 1] = v[b]; b--; }
            v[b + 1] = t;
        }
        for (a = 0; a < n; a++) {               /* link + write each word */
            uint64_t next = 0;
            if (a + 1 < n &&
                v[a + 1].seg_off / CHAINED_PAGE_SIZE == v[a].seg_off / CHAINED_PAGE_SIZE)
                next = (v[a + 1].seg_off - v[a].seg_off) / 4;
            w64(v[a].slot, v[a].word | (next << 51));
        }
        cs[i].pages = n ? (v[n - 1].seg_off / CHAINED_PAGE_SIZE + 1) : 0;
    }

    /* the LC_DYLD_CHAINED_FIXUPS payload: header, starts-in-image (one entry per
       segment, nonzero for each chained data segment), each chained segment's
       starts-in-segment page_start table, then the import table + symbol pool. */
    struct buf chainfix = {0};
    {
        uint32_t off_starts = 32;                       /* 28-byte header, pad to 32 */
        uint32_t cur = off_starts + 4 + 4u * (uint32_t)seg_count;
        for (i = 0; i < ncs; i++) {
            cs[i].info_off = (uint32_t)align_up(cur, 8);
            cur = cs[i].info_off + 22 + 2u * (uint32_t)cs[i].pages;
        }
        uint32_t off_imports = (uint32_t)align_up(cur, 4);
        uint32_t off_syms = off_imports + 4u * (uint32_t)ld->nimport;

        bput(&chainfix, NULL, off_imports);             /* reserve header + starts */
        w32(chainfix.d + 0, 0);                         /* fixups_version */
        w32(chainfix.d + 4, off_starts);                /* starts_offset */
        w32(chainfix.d + 8, off_imports);               /* imports_offset */
        w32(chainfix.d + 12, off_syms);                 /* symbols_offset */
        w32(chainfix.d + 16, (uint32_t)ld->nimport);    /* imports_count */
        w32(chainfix.d + 20, DYLD_CHAINED_IMPORT);      /* imports_format */
        w32(chainfix.d + 24, 0);                        /* symbols_format (uncompressed) */

        /* dyld_chained_starts_in_image + a starts-in-segment per chained seg */
        w32(chainfix.d + off_starts, (uint32_t)seg_count);
        for (i = 0; i < ncs; i++) {
            uint8_t *sg = chainfix.d + cs[i].info_off;
            uint64_t p;
            w32(chainfix.d + off_starts + 4 + 4 * cs[i].seg_index,
                cs[i].info_off - off_starts);
            w32(sg + 0, 22 + 2u * (uint32_t)cs[i].pages); /* size */
            w16(sg + 4, (uint16_t)CHAINED_PAGE_SIZE);     /* page_size */
            w16(sg + 6, DYLD_CHAINED_PTR_64_OFFSET);      /* pointer_format */
            w64(sg + 8, cs[i].vmaddr - TEXT_BASE);        /* segment_offset */
            w32(sg + 16, 0);                              /* max_valid_pointer */
            w16(sg + 20, (uint16_t)cs[i].pages);          /* page_count */
            for (p = 0; p < cs[i].pages; p++) {
                uint16_t start = DYLD_CHAINED_PTR_START_NONE;
                int k;
                for (k = 0; k < cs[i].nf; k++)
                    if (cs[i].fx[k].seg_off / CHAINED_PAGE_SIZE == p) {
                        start = (uint16_t)(cs[i].fx[k].seg_off % CHAINED_PAGE_SIZE);
                        break;
                    }
                w16(sg + 22 + 2 * p, start);
            }
        }

        /* imports table + symbol-name pool */
        {
            struct buf syms = {0};
            bu8(&syms, 0);                              /* pool index 0 = empty */
            for (i = 0; i < ld->nimport; i++) {
                const char *nm = ld->gsyms[ld->imports[i]].name;
                uint32_t imp = 1u                       /* lib_ordinal = libSystem (8) */
                             | (0u << 8)                /* weak_import (1) */
                             | ((uint32_t)syms.len << 9); /* name_offset (23) */
                uint8_t w[4];
                w32(w, imp);
                bput(&chainfix, w, 4);
                bput(&syms, nm, strlen(nm) + 1);
            }
            bput(&chainfix, syms.d, syms.len);
            free(syms.d);
        }
        while (chainfix.len & 7) bu8(&chainfix, 0);
    }

    /* ---- symbol + string tables ---- */
    struct buf strtab = {0};
    struct buf symtab = {0};
    bu8(&strtab, 0);                    /* index 0 = empty */
    int nsym = 0;
    /* defined symbols first (as local/ext per their binding is not tracked;
       emit them all as external defined so nm shows them), then imports. */
    for (g = 0; g < ld->ngsym; g++) {
        if (!ld->gsyms[g].defined)
            continue;
        uint8_t ns[16];
        memset(ns, 0, 16);
        w32(ns + 0, (uint32_t)strtab.len);
        bput(&strtab, ld->gsyms[g].name, strlen(ld->gsyms[g].name) + 1);
        ns[4] = N_SECT | N_EXT;
        ns[5] = 1;                       /* section ref: nominal (__text) */
        w64(ns + 8, gsym_addr(ld, &L, g));
        bput(&symtab, ns, 16);
        nsym++;
    }
    int nlocal_def = nsym;
    for (i = 0; i < ld->nimport; i++) {
        uint8_t ns[16];
        memset(ns, 0, 16);
        w32(ns + 0, (uint32_t)strtab.len);
        bput(&strtab, ld->gsyms[ld->imports[i]].name,
             strlen(ld->gsyms[ld->imports[i]].name) + 1);
        ns[4] = N_UNDF | N_EXT;
        w16(ns + 6, (uint16_t)(1 << 8));   /* library ordinal 1 in n_desc hi */
        bput(&symtab, ns, 16);
        nsym++;
    }
    while (strtab.len & 7) bu8(&strtab, 0);

    /* ---- LINKEDIT layout (after __DWARF, if any) ---- */
    uint64_t le_vmaddr = dw_vmaddr + dw_vmsize;
    uint64_t le_fileoff = dw_fileoff + dw_vmsize;
    uint64_t cur = le_fileoff;
    uint64_t chainfix_off = cur; cur += chainfix.len;
    uint64_t symoff = cur; cur += (uint64_t)nsym * 16;
    uint64_t stroff = cur; cur += strtab.len;
    cur = align_up(cur, 16);
    uint64_t sig_off = cur;

    /* code signature size, needed now so __LINKEDIT covers it (dyld's strict
       validation requires the last segment to reach the end of the file). */
    const char *cs_ident = "a.out";
    uint32_t cs_nslots = (uint32_t)((sig_off + CS_PAGE - 1) / CS_PAGE);
    uint32_t cs_cd_len = 88 + (uint32_t)strlen(cs_ident) + 1 + cs_nslots * 32;
    uint32_t cs_sb_len = 20 + cs_cd_len;
    uint64_t file_end = sig_off + cs_sb_len;

    uint64_t entry_addr = gsym_addr(ld, &L, entry_g);
    uint64_t entryoff = entry_addr - TEXT_BASE;

    /****** assemble the file image ******/
    struct buf f = {0};

    /* mach_header_64 */
    {
        uint8_t h[32];
        memset(h, 0, 32);
        w32(h + 0, MH_MAGIC_64);
        w32(h + 4, CPU_TYPE_ARM64);
        w32(h + 8, CPU_SUBTYPE_ARM64_ALL);
        w32(h + 12, MH_EXECUTE);
        w32(h + 16, ncmds);
        w32(h + 20, sizeofcmds);
        w32(h + 24, MH_DYLDLINK | MH_TWOLEVEL | MH_PIE);
        bput(&f, h, 32);
    }

    /* segment emitters */
#define SEG(name, vmaddr, vmsize, fileoff, filesize, maxp, initp, nsects, segflags) \
    do { uint8_t c[72]; memset(c, 0, 72); \
         w32(c + 0, LC_SEGMENT_64); w32(c + 4, 72 + (uint32_t)(nsects) * 80); \
         strncpy((char *)c + 8, (name), 16); \
         w64(c + 24, (vmaddr)); w64(c + 32, (vmsize)); \
         w64(c + 40, (fileoff)); w64(c + 48, (filesize)); \
         w32(c + 56, (maxp)); w32(c + 60, (initp)); \
         w32(c + 64, (uint32_t)(nsects)); w32(c + 68, (segflags)); \
         bput(&f, c, 72); } while (0)
#define SECT(sect, seg, addr, size, off, al, flags) \
    do { uint8_t c[80]; memset(c, 0, 80); \
         strncpy((char *)c + 0, (sect), 16); strncpy((char *)c + 16, (seg), 16); \
         w64(c + 32, (addr)); w64(c + 40, (size)); w32(c + 48, (uint32_t)(off)); \
         w32(c + 52, (al)); w32(c + 64, (flags)); \
         bput(&f, c, 80); } while (0)

    SEG("__PAGEZERO", 0, PAGEZERO_SZ, 0, 0, 0, 0, 0, 0);

    SEG("__TEXT", TEXT_BASE, text_vmsize, 0, text_filesize,
        VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_EXECUTE,
        text_nsects, 0);
    SECT("__text", "__TEXT", L.m[MSEC_TEXT].out_addr, L.m[MSEC_TEXT].size,
         L.m[MSEC_TEXT].out_fileoff, 2,
         S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
    if (L.stubs_size)
        SECT("__stubs", "__TEXT", L.stubs_addr, L.stubs_size, L.stubs_fileoff, 2,
             S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);

    /* __DATA_CONST (got + const): SG_READ_ONLY, so dyld maps it writable, applies
       the fixup chain, then protects it read-only. */
    if (have_dc) {
        SEG("__DATA_CONST", dc_vmaddr, dc_vmsize, dc_fileoff, dc_filesize,
            VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
            dc_nsects, SG_READ_ONLY);
        if (L.got_size)
            SECT("__got", "__DATA_CONST", L.got_addr, L.got_size, L.got_fileoff,
                 3, S_REGULAR);
        if (L.m[MSEC_CONST].size)
            SECT("__const", "__DATA_CONST", L.m[MSEC_CONST].out_addr,
                 L.m[MSEC_CONST].size, L.m[MSEC_CONST].out_fileoff, 3, S_REGULAR);
    }
    if (have_data) {
        SEG("__DATA", data_vmaddr, data_vmsize, data_fileoff, data_filesize,
            VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
            data_nsects, 0);
        if (L.m[MSEC_DATA].size)
            SECT("__data", "__DATA", L.m[MSEC_DATA].out_addr, L.m[MSEC_DATA].size,
                 L.m[MSEC_DATA].out_fileoff, 3, S_REGULAR);
        if (L.m[MSEC_BSS].size)
            SECT("__bss", "__DATA", L.m[MSEC_BSS].out_addr, L.m[MSEC_BSS].size, 0, 3,
                 S_ZEROFILL);
    }

    /* __DWARF: the debug sections, read-only and not slid (S_ATTR_DEBUG).  The
       absolute addresses inside were resolved by apply_relocs to unslid vm
       addresses; a DWARF consumer applies the image slide itself. */
    if (have_dw) {
        SEG("__DWARF", dw_vmaddr, dw_vmsize, dw_fileoff, dw_filesize,
            VM_PROT_READ, VM_PROT_READ, dw_nsects, 0);
        if (L.m[MSEC_DEBUG_LINE].size)
            SECT("__debug_line", "__DWARF", L.m[MSEC_DEBUG_LINE].out_addr,
                 L.m[MSEC_DEBUG_LINE].size, L.m[MSEC_DEBUG_LINE].out_fileoff,
                 0, S_ATTR_DEBUG);
        if (L.m[MSEC_DEBUG_INFO].size)
            SECT("__debug_info", "__DWARF", L.m[MSEC_DEBUG_INFO].out_addr,
                 L.m[MSEC_DEBUG_INFO].size, L.m[MSEC_DEBUG_INFO].out_fileoff,
                 0, S_ATTR_DEBUG);
        if (L.m[MSEC_DEBUG_ABBREV].size)
            SECT("__debug_abbrev", "__DWARF", L.m[MSEC_DEBUG_ABBREV].out_addr,
                 L.m[MSEC_DEBUG_ABBREV].size, L.m[MSEC_DEBUG_ABBREV].out_fileoff,
                 0, S_ATTR_DEBUG);
    }

    {
        uint64_t le_filesize = file_end - le_fileoff;
        SEG("__LINKEDIT", le_vmaddr, align_up(le_filesize, SEG_ALIGN),
            le_fileoff, le_filesize, VM_PROT_READ, VM_PROT_READ, 0, 0);
    }

    /* LC_DYLD_CHAINED_FIXUPS */
    {
        uint8_t c[16]; memset(c, 0, 16);
        w32(c + 0, LC_DYLD_CHAINED_FIXUPS); w32(c + 4, 16);
        w32(c + 8, (uint32_t)chainfix_off); w32(c + 12, (uint32_t)chainfix.len);
        bput(&f, c, 16);
    }
    /* LC_SYMTAB */
    {
        uint8_t c[24]; memset(c, 0, 24);
        w32(c + 0, LC_SYMTAB); w32(c + 4, 24);
        w32(c + 8, (uint32_t)symoff); w32(c + 12, (uint32_t)nsym);
        w32(c + 16, (uint32_t)stroff); w32(c + 20, (uint32_t)strtab.len);
        bput(&f, c, 24);
    }
    /* LC_DYSYMTAB */
    {
        uint8_t c[80]; memset(c, 0, 80);
        w32(c + 0, LC_DYSYMTAB); w32(c + 4, 80);
        w32(c + 8, 0); w32(c + 12, 0);               /* ilocalsym / nlocalsym */
        w32(c + 16, 0); w32(c + 20, (uint32_t)nlocal_def);   /* iextdef / nextdef */
        w32(c + 24, (uint32_t)nlocal_def);                   /* iundefsym */
        w32(c + 28, (uint32_t)ld->nimport);                  /* nundefsym */
        bput(&f, c, 80);
    }
    /* LC_LOAD_DYLINKER */
    {
        uint8_t c[12]; memset(c, 0, 12);
        w32(c + 0, LC_LOAD_DYLINKER); w32(c + 4, dyld_lc); w32(c + 8, 12);
        bput(&f, c, 12);
        bput(&f, dyld_path, strlen(dyld_path) + 1);
        while ((f.len) % 8) bu8(&f, 0);
    }
    /* LC_UUID */
    {
        uint8_t c[24]; memset(c, 0, 24);
        w32(c + 0, LC_UUID); w32(c + 4, 24);
        for (i = 0; i < 16; i++) c[8 + i] = (uint8_t)(0x11 * (i + 1));
        bput(&f, c, 24);
    }
    /* LC_BUILD_VERSION */
    {
        uint8_t c[24]; memset(c, 0, 24);
        w32(c + 0, LC_BUILD_VERSION); w32(c + 4, 24);
        w32(c + 8, PLATFORM_MACOS); w32(c + 12, 0x000B0000u); w32(c + 16, 0x000B0000u);
        bput(&f, c, 24);
    }
    /* LC_MAIN */
    {
        uint8_t c[24]; memset(c, 0, 24);
        w32(c + 0, LC_MAIN); w32(c + 4, 24);
        w64(c + 8, entryoff); w64(c + 16, 0);
        bput(&f, c, 24);
    }
    /* LC_LOAD_DYLIB */
    {
        uint8_t c[24]; memset(c, 0, 24);
        w32(c + 0, LC_LOAD_DYLIB); w32(c + 4, dylib_lc); w32(c + 8, 24);
        w32(c + 12, 2); w32(c + 16, 0x10000); w32(c + 20, 0x10000);  /* ts/cur/compat */
        bput(&f, c, 24);
        bput(&f, dylib_path, strlen(dylib_path) + 1);
        while ((f.len) % 8) bu8(&f, 0);
    }
    /* LC_CODE_SIGNATURE (dataoff/size filled after we size the blob) */
    size_t cs_lc_pos = f.len;
    {
        uint8_t c[16]; memset(c, 0, 16);
        w32(c + 0, LC_CODE_SIGNATURE); w32(c + 4, 16);
        bput(&f, c, 16);
    }

    /* ---- section bytes ---- */
    while (f.len < L.m[MSEC_TEXT].out_fileoff) bu8(&f, 0);
    if (L.m[MSEC_TEXT].size) bput(&f, L.m[MSEC_TEXT].data, (size_t)L.m[MSEC_TEXT].size);
    if (L.stubs_size) { while (f.len < L.stubs_fileoff) bu8(&f, 0);
                        bput(&f, stub_bytes, (size_t)L.stubs_size); }
    if (L.got_size) { while (f.len < L.got_fileoff) bu8(&f, 0);
                      bput(&f, got_bytes, (size_t)L.got_size); }
    if (L.m[MSEC_CONST].size) { while (f.len < L.m[MSEC_CONST].out_fileoff) bu8(&f, 0);
                                bput(&f, L.m[MSEC_CONST].data, (size_t)L.m[MSEC_CONST].size); }
    if (L.m[MSEC_DATA].size) { while (f.len < L.m[MSEC_DATA].out_fileoff) bu8(&f, 0);
                               bput(&f, L.m[MSEC_DATA].data, (size_t)L.m[MSEC_DATA].size); }

    /* ---- __DWARF debug sections ---- */
    {
        int dk;
        for (dk = MSEC_DEBUG_FIRST; dk < MSEC_KIND_COUNT; dk++) {
            if (!L.m[dk].size)
                continue;
            while (f.len < L.m[dk].out_fileoff) bu8(&f, 0);
            bput(&f, L.m[dk].data, (size_t)L.m[dk].size);
        }
    }

    /* ---- LINKEDIT ---- */
    while (f.len < le_fileoff) bu8(&f, 0);
    bput(&f, chainfix.d, chainfix.len);
    bput(&f, symtab.d, symtab.len);
    bput(&f, strtab.d, strtab.len);
    while (f.len < sig_off) bu8(&f, 0);

    /* ---- ad-hoc code signature ---- */
    {
        const char *ident = cs_ident;
        uint32_t nslots = cs_nslots;
        uint32_t ident_off = 88;    /* v20400 fixed header size (through execSegFlags) */
        uint32_t hoff = ident_off + (uint32_t)strlen(ident) + 1;
        uint32_t cd_len = cs_cd_len;
        struct buf cd = {0};
        uint8_t *cdp;

        /* Patch LC_CODE_SIGNATURE before hashing: those bytes lie inside the
           hashed region [0, sig_off), so the on-disk header must be final. */
        w32(f.d + cs_lc_pos + 8, (uint32_t)sig_off);
        w32(f.d + cs_lc_pos + 12, (uint32_t)cs_sb_len);

        /* Build the CodeDirectory. */
        bput(&cd, NULL, cd_len);
        cdp = cd.d;
        w32be(cdp + 0, CSMAGIC_CODEDIRECTORY);
        w32be(cdp + 4, cd_len);
        w32be(cdp + 8, 0x20400);           /* version */
        w32be(cdp + 12, CS_ADHOC);         /* flags */
        w32be(cdp + 16, hoff);             /* hashOffset */
        w32be(cdp + 20, ident_off);        /* identOffset */
        w32be(cdp + 24, 0);                /* nSpecialSlots */
        w32be(cdp + 28, nslots);           /* nCodeSlots */
        w32be(cdp + 32, (uint32_t)sig_off);/* codeLimit */
        cdp[36] = 32;                      /* hashSize */
        cdp[37] = CS_HASHTYPE_SHA256;      /* hashType */
        cdp[38] = 0;                       /* platform */
        cdp[39] = 12;                      /* pageSize log2 (4096) */
        w32be(cdp + 40, 0);                /* spare2 */
        /* v20100 scatterOffset(44), v20200 teamOffset(48), v20300 spare3(52),
           v20300 codeLimit64(56,8), v20400 execSegBase(64,8), limit(72,8),
           flags(80,8) */
        w64be(cdp + 64, 0);                           /* execSegBase */
        w64be(cdp + 72, text_vmsize);                 /* execSegLimit */
        w64be(cdp + 80, CS_EXECSEG_MAIN_BINARY);      /* execSegFlags */
        memcpy(cdp + ident_off, ident, strlen(ident) + 1);
        {
            uint32_t s;
            for (s = 0; s < nslots; s++) {
                struct sha256 sh;
                uint64_t start = (uint64_t)s * CS_PAGE;
                uint64_t n = sig_off - start;
                if (n > CS_PAGE) n = CS_PAGE;
                sha256_init(&sh);
                sha256_update(&sh, f.d + start, (size_t)n);
                sha256_final(&sh, cdp + hoff + s * 32);
            }
        }

        /* SuperBlob: one slot (CodeDirectory). */
        {
            struct buf sb = {0};
            uint32_t sb_len = 12 + 8 + cd_len;
            uint8_t hdr[20];
            w32be(hdr + 0, CSMAGIC_EMBEDDED_SIGNATURE);
            w32be(hdr + 4, sb_len);
            w32be(hdr + 8, 1);                 /* count */
            w32be(hdr + 12, CSSLOT_CODEDIRECTORY);
            w32be(hdr + 16, 20);               /* offset of the CodeDirectory */
            bput(&sb, hdr, 20);
            bput(&sb, cd.d, cd.len);
            bput(&f, sb.d, sb.len);
            free(sb.d);
        }
        free(cd.d);
    }

    /* ---- write it out ---- */
    {
        FILE *out = fopen(outpath, "wb");
        if (!out)
            die("cannot open %s for writing", outpath);
        fwrite(f.d, 1, f.len, out);
        fclose(out);
        chmod(outpath, 0755);
    }

    free(chainfix.d); free(symtab.d); free(strtab.d); free(f.d);
    free(rb);
}
