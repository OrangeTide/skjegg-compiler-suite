/* macho_link.c : lay out a PIE MH_EXECUTE from the input objects, apply the
 * arm64 relocations, bind the libSystem imports through stubs and a __got with
 * classic dyld bind opcodes, and self-sign with an ad-hoc code signature.
 *
 * Layout (arm64, 16 KiB segment pages):
 *   __PAGEZERO  (no file)         4 GiB guard
 *   __TEXT  r-x  @ 0x100000000    mach header + load commands, __text, __stubs
 *   __DATA  rw                    __got, __const, __data, __bss(zerofill)
 *   __LINKEDIT r                  dyld info, symbol table, string table, signature
 *
 * The imports are bound with classic LC_DYLD_INFO_ONLY bind opcodes writing the
 * __got slots directly (no indirect symbol table, no dyld_stub_binder); each
 * `bl _import` is redirected to a three-instruction stub that loads its __got
 * slot and branches.  Absolute data pointers to our own symbols get a rebase. */

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

static void
uleb(struct buf *b, uint64_t v)
{
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        if (v)
            byte |= 0x80;
        bu8(b, byte);
    } while (v);
}

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
                    if (!t->is_import) {
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

/* indices into the segment list, for bind/rebase segment references */
enum { SEG_PAGEZERO, SEG_TEXT, SEG_DATA, SEG_LINKEDIT };

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

    /* ---- compute the load-command size, so we know where __text starts ---- */
    int text_nsects = 1 + (L.stubs_size ? 1 : 0);          /* __text [+ __stubs] */
    int data_nsects = (L.got_size ? 1 : 0)
                    + (L.m[MSEC_CONST].size ? 1 : 0)
                    + (L.m[MSEC_DATA].size ? 1 : 0)
                    + (L.m[MSEC_BSS].size ? 1 : 0);
    const char *dyld_path = "/usr/lib/dyld";
    const char *dylib_path = "/usr/lib/libSystem.B.dylib";
    uint32_t dyld_lc = (uint32_t)align_up(12 + strlen(dyld_path) + 1, 8);
    uint32_t dylib_lc = (uint32_t)align_up(24 + strlen(dylib_path) + 1, 8);

    uint32_t sizeofcmds =
        72                                  /* PAGEZERO */
      + 72 + (uint32_t)text_nsects * 80     /* TEXT */
      + 72 + (uint32_t)data_nsects * 80     /* DATA */
      + 72                                  /* LINKEDIT */
      + 48                                  /* DYLD_INFO_ONLY */
      + 24                                  /* SYMTAB */
      + 80                                  /* DYSYMTAB */
      + dyld_lc                             /* LOAD_DYLINKER */
      + 24                                  /* UUID */
      + 24                                  /* BUILD_VERSION */
      + 24                                  /* MAIN */
      + dylib_lc                            /* LOAD_DYLIB */
      + 16;                                 /* CODE_SIGNATURE */
    uint32_t ncmds = 4 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1;  /* see below */
    ncmds = 13;
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

    /* ---- __DATA layout ---- */
    uint64_t data_vmaddr = TEXT_BASE + text_vmsize;
    uint64_t data_fileoff = text_vmsize;
    uint64_t d = 0;
    if (L.got_size) {
        d = align_up(d, 8);
        L.got_fileoff = data_fileoff + d;
        L.got_addr = data_vmaddr + d;
        d += L.got_size;
    }
    if (L.m[MSEC_CONST].size) {
        d = align_up(d, (uint64_t)L.m[MSEC_CONST].align);
        L.m[MSEC_CONST].out_fileoff = data_fileoff + d;
        L.m[MSEC_CONST].out_addr = data_vmaddr + d;
        d += L.m[MSEC_CONST].size;
    }
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

    /* ---- __got contents: start zero; dyld binds them ---- */
    uint8_t *got_bytes = NULL;
    if (L.got_size) {
        got_bytes = arena_alloc(&ld->arena, (size_t)L.got_size);
        memset(got_bytes, 0, (size_t)L.got_size);
    }

    /* ---- dyld info: rebase + bind ---- */
    struct buf rebase = {0}, bind = {0};
    if (nrb) {
        bu8(&rebase, REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER);
        for (i = 0; i < nrb; i++) {
            uint64_t seg_off;
            if (rb[i].kind == MSEC_CONST)
                seg_off = (L.m[MSEC_CONST].out_addr - data_vmaddr) + rb[i].off;
            else
                seg_off = (L.m[MSEC_DATA].out_addr - data_vmaddr) + rb[i].off;
            bu8(&rebase, REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | SEG_DATA);
            uleb(&rebase, seg_off);
            bu8(&rebase, REBASE_OPCODE_DO_REBASE_ULEB_TIMES);
            uleb(&rebase, 1);
        }
        bu8(&rebase, REBASE_OPCODE_DONE);
    }
    if (ld->nimport) {
        bu8(&bind, BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | 1);   /* libSystem = 1 */
        bu8(&bind, BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER);
        for (i = 0; i < ld->nimport; i++) {
            const char *nm = ld->gsyms[ld->imports[i]].name;
            uint64_t seg_off = (L.got_addr - data_vmaddr) + (uint64_t)i * 8;
            bu8(&bind, BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM);
            bput(&bind, nm, strlen(nm) + 1);
            bu8(&bind, BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | SEG_DATA);
            uleb(&bind, seg_off);
            bu8(&bind, BIND_OPCODE_DO_BIND);
        }
        bu8(&bind, BIND_OPCODE_DONE);
    }
    while (rebase.len & 7) bu8(&rebase, 0);
    while (bind.len & 7) bu8(&bind, 0);

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

    /* ---- LINKEDIT layout ---- */
    uint64_t le_vmaddr = data_vmaddr + data_vmsize;
    uint64_t le_fileoff = data_fileoff + data_vmsize;
    uint64_t cur = le_fileoff;
    uint64_t rebase_off = cur; cur += rebase.len;
    uint64_t bind_off = cur; cur += bind.len;
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
#define SEG(name, vmaddr, vmsize, fileoff, filesize, maxp, initp, nsects) \
    do { uint8_t c[72]; memset(c, 0, 72); \
         w32(c + 0, LC_SEGMENT_64); w32(c + 4, 72 + (uint32_t)(nsects) * 80); \
         strncpy((char *)c + 8, (name), 16); \
         w64(c + 24, (vmaddr)); w64(c + 32, (vmsize)); \
         w64(c + 40, (fileoff)); w64(c + 48, (filesize)); \
         w32(c + 56, (maxp)); w32(c + 60, (initp)); \
         w32(c + 64, (uint32_t)(nsects)); w32(c + 68, 0); \
         bput(&f, c, 72); } while (0)
#define SECT(sect, seg, addr, size, off, al, flags) \
    do { uint8_t c[80]; memset(c, 0, 80); \
         strncpy((char *)c + 0, (sect), 16); strncpy((char *)c + 16, (seg), 16); \
         w64(c + 32, (addr)); w64(c + 40, (size)); w32(c + 48, (uint32_t)(off)); \
         w32(c + 52, (al)); w32(c + 64, (flags)); \
         bput(&f, c, 80); } while (0)

    SEG("__PAGEZERO", 0, PAGEZERO_SZ, 0, 0, 0, 0, 0);

    SEG("__TEXT", TEXT_BASE, text_vmsize, 0, text_filesize,
        VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_EXECUTE,
        text_nsects);
    SECT("__text", "__TEXT", L.m[MSEC_TEXT].out_addr, L.m[MSEC_TEXT].size,
         L.m[MSEC_TEXT].out_fileoff, 2,
         S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
    if (L.stubs_size)
        SECT("__stubs", "__TEXT", L.stubs_addr, L.stubs_size, L.stubs_fileoff, 2,
             S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);

    SEG("__DATA", data_vmaddr, data_vmsize, data_fileoff, data_filesize,
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, data_nsects);
    if (L.got_size)
        SECT("__got", "__DATA", L.got_addr, L.got_size, L.got_fileoff, 3, S_REGULAR);
    if (L.m[MSEC_CONST].size)
        SECT("__const", "__DATA", L.m[MSEC_CONST].out_addr, L.m[MSEC_CONST].size,
             L.m[MSEC_CONST].out_fileoff, 3, S_REGULAR);
    if (L.m[MSEC_DATA].size)
        SECT("__data", "__DATA", L.m[MSEC_DATA].out_addr, L.m[MSEC_DATA].size,
             L.m[MSEC_DATA].out_fileoff, 3, S_REGULAR);
    if (L.m[MSEC_BSS].size)
        SECT("__bss", "__DATA", L.m[MSEC_BSS].out_addr, L.m[MSEC_BSS].size, 0, 3,
             S_ZEROFILL);

    {
        uint64_t le_filesize = file_end - le_fileoff;
        SEG("__LINKEDIT", le_vmaddr, align_up(le_filesize, SEG_ALIGN),
            le_fileoff, le_filesize, VM_PROT_READ, VM_PROT_READ, 0);
    }

    /* LC_DYLD_INFO_ONLY */
    {
        uint8_t c[48]; memset(c, 0, 48);
        w32(c + 0, LC_DYLD_INFO_ONLY); w32(c + 4, 48);
        if (rebase.len) { w32(c + 8, (uint32_t)rebase_off); w32(c + 12, (uint32_t)rebase.len); }
        if (bind.len)   { w32(c + 16, (uint32_t)bind_off); w32(c + 20, (uint32_t)bind.len); }
        bput(&f, c, 48);
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
    while (f.len < data_fileoff) bu8(&f, 0);
    if (L.got_size) { while (f.len < L.got_fileoff) bu8(&f, 0);
                      bput(&f, got_bytes, (size_t)L.got_size); }
    if (L.m[MSEC_CONST].size) { while (f.len < L.m[MSEC_CONST].out_fileoff) bu8(&f, 0);
                                bput(&f, L.m[MSEC_CONST].data, (size_t)L.m[MSEC_CONST].size); }
    if (L.m[MSEC_DATA].size) { while (f.len < L.m[MSEC_DATA].out_fileoff) bu8(&f, 0);
                               bput(&f, L.m[MSEC_DATA].data, (size_t)L.m[MSEC_DATA].size); }

    /* ---- LINKEDIT ---- */
    while (f.len < le_fileoff) bu8(&f, 0);
    bput(&f, rebase.d, rebase.len);
    bput(&f, bind.d, bind.len);
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

    free(rebase.d); free(bind.d); free(symtab.d); free(strtab.d); free(f.d);
    free(rb);
}
