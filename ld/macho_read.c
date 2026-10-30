/* macho_read.c : read an MH_OBJECT relocatable (the kind skj-as-aarch64 writes)
 * into the linker's in-memory model.  Only the constructs the in-tree assembler
 * emits are handled: one LC_SEGMENT_64 with __text/__const/__data/__bss, an
 * LC_SYMTAB, and external (r_extern=1) arm64 relocations with the optional
 * ARM64_RELOC_ADDEND prefix. */

#include "macho_ld.h"

#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t
rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint8_t *
slurp_file(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
        die("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = xmalloc((size_t)n);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)
        die("short read on %s", path);
    fclose(f);
    *len_out = n;
    return buf;
}

/* Find or create a global symbol by name. */
int
macho_gsym(struct macho_link *ld, const char *name)
{
    int i;

    for (i = 0; i < ld->ngsym; i++)
        if (strcmp(ld->gsyms[i].name, name) == 0)
            return i;
    if (ld->ngsym == ld->gsym_cap) {
        ld->gsym_cap = ld->gsym_cap ? ld->gsym_cap * 2 : 64;
        ld->gsyms = realloc(ld->gsyms,
                            (size_t)ld->gsym_cap * sizeof(struct min_gsym));
    }
    memset(&ld->gsyms[ld->ngsym], 0, sizeof(struct min_gsym));
    ld->gsyms[ld->ngsym].name = arena_strdup(&ld->arena, name);
    ld->gsyms[ld->ngsym].stub_index = -1;
    ld->gsyms[ld->ngsym].got_index = -1;
    return ld->ngsym++;
}

/* Map a (segname,sectname) pair to one of the kinds we place. */
static int
sec_kind(const char *seg, const char *sect)
{
    (void)seg;
    if (strcmp(sect, "__text") == 0)    return MSEC_TEXT;
    if (strcmp(sect, "__const") == 0)   return MSEC_CONST;
    if (strcmp(sect, "__cstring") == 0) return MSEC_CONST;
    if (strcmp(sect, "__data") == 0)    return MSEC_DATA;
    if (strcmp(sect, "__bss") == 0)     return MSEC_BSS;
    if (strcmp(sect, "__debug_line") == 0)   return MSEC_DEBUG_LINE;
    if (strcmp(sect, "__debug_info") == 0)   return MSEC_DEBUG_INFO;
    if (strcmp(sect, "__debug_abbrev") == 0) return MSEC_DEBUG_ABBREV;
    return -1;
}

void
macho_read_obj(struct macho_link *ld, const char *path)
{
    struct min_obj *o;
    uint8_t *f;
    long flen;
    uint32_t ncmds, k;
    const uint8_t *lc;
    /* per-object section bookkeeping, indexed by 1-based Mach-O section number */
    int secnum_kind[64];
    uint64_t secnum_addr[64];
    uint32_t secnum_reloff[64], secnum_nreloc[64];
    int nsecnum = 0;
    const uint8_t *symtab = NULL, *strtab = NULL;
    uint32_t nsyms = 0, strsize = 0;

    f = slurp_file(path, &flen);
    if (flen < 32 || rd32(f) != MH_MAGIC_64)
        die("%s: not a 64-bit Mach-O object", path);
    if (rd32(f + 4) != CPU_TYPE_ARM64)
        die("%s: not an arm64 object", path);

    ld->objs = realloc(ld->objs, (size_t)(ld->nobj + 1) * sizeof(struct min_obj));
    o = &ld->objs[ld->nobj++];
    memset(o, 0, sizeof(*o));
    o->path = arena_strdup(&ld->arena, path);
    o->file = f;
    o->filelen = flen;
    for (k = 0; k < MSEC_KIND_COUNT; k++)
        o->secs[k].kind = -1;

    ncmds = rd32(f + 16);
    lc = f + 32;
    for (k = 0; k < ncmds; k++) {
        uint32_t cmd = rd32(lc);
        uint32_t cmdsize = rd32(lc + 4);

        if (cmd == LC_SEGMENT_64) {
            uint32_t nsect = rd32(lc + 64);
            const uint8_t *sc = lc + 72;
            uint32_t j;
            for (j = 0; j < nsect; j++, sc += 80) {
                char sect[17], seg[17];
                int kind;
                memcpy(sect, sc + 0, 16);  sect[16] = 0;
                memcpy(seg, sc + 16, 16);  seg[16] = 0;
                kind = sec_kind(seg, sect);
                nsecnum++;                 /* 1-based */
                if (nsecnum < 64) {
                    secnum_kind[nsecnum] = kind;
                    secnum_addr[nsecnum] = rd64(sc + 32);
                    secnum_reloff[nsecnum] = rd32(sc + 56);
                    secnum_nreloc[nsecnum] = rd32(sc + 60);
                }
                if (kind >= 0) {
                    uint32_t flags = rd32(sc + 64);
                    uint64_t size = rd64(sc + 40);
                    uint32_t off = rd32(sc + 48);
                    uint32_t al = rd32(sc + 52);
                    o->secs[kind].kind = kind;
                    o->secs[kind].size = size;
                    o->secs[kind].align = al < 31 ? (1 << al) : 1;
                    o->secs[kind].data =
                        ((flags & 0xff) == S_ZEROFILL) ? NULL : f + off;
                }
            }
        } else if (cmd == LC_SYMTAB) {
            symtab = f + rd32(lc + 8);
            nsyms = rd32(lc + 12);
            strtab = f + rd32(lc + 16);
            strsize = rd32(lc + 20);
            (void)strsize;
        }
        lc += cmdsize;
    }

    /* Symbols: build the local->global map, recording definitions. */
    o->nsyms = (int)nsyms;
    o->symmap = arena_alloc(&ld->arena, (size_t)(nsyms ? nsyms : 1) * sizeof(int));
    for (k = 0; k < nsyms; k++) {
        const uint8_t *ns = symtab + (size_t)k * 16;
        uint32_t strx = rd32(ns);
        uint8_t ntype = ns[4];
        uint8_t nsect = ns[5];
        uint64_t nval = rd64(ns + 8);
        const char *name = (const char *)strtab + strx;
        int g = macho_gsym(ld, name);
        o->symmap[k] = g;
        (void)rd16;

        if ((ntype & N_STAB) == 0 && (ntype & N_TYPE) == N_SECT) {
            /* a definition in this object */
            int kind = (nsect < 64) ? secnum_kind[nsect] : -1;
            uint64_t base = (nsect < 64) ? secnum_addr[nsect] : 0;
            if (!ld->gsyms[g].defined) {
                ld->gsyms[g].defined = 1;
                ld->gsyms[g].obj = ld->nobj - 1;
                ld->gsyms[g].seckind = kind;
                ld->gsyms[g].value = nval - base;   /* section-relative for now */
            }
        }
    }

    /* Relocations per placed section. */
    {
        int kind;
        for (kind = 0; kind < MSEC_KIND_COUNT; kind++) {
            int sn, found = -1;
            if (o->secs[kind].kind < 0)
                continue;
            for (sn = 1; sn <= nsecnum && sn < 64; sn++)
                if (secnum_kind[sn] == kind) { found = sn; break; }
            if (found < 0 || secnum_nreloc[found] == 0)
                continue;
            {
                uint32_t nr = secnum_nreloc[found];
                const uint8_t *rp = f + secnum_reloff[found];
                struct min_reloc *rel =
                    arena_alloc(&ld->arena, (size_t)nr * sizeof(struct min_reloc));
                int n = 0;
                int64_t pending_addend = 0;
                uint32_t r;
                for (r = 0; r < nr; r++) {
                    int32_t raddr = (int32_t)rd32(rp + r * 8);
                    uint32_t w = rd32(rp + r * 8 + 4);
                    uint32_t symnum = w & 0x00FFFFFFu;
                    int pcrel = (w >> 24) & 1;
                    int length = (w >> 25) & 3;
                    int ext = (w >> 27) & 1;
                    int type = (w >> 28) & 0xf;

                    if (type == ARM64_RELOC_ADDEND) {
                        pending_addend = (int64_t)symnum;
                        continue;
                    }
                    rel[n].offset = (uint32_t)raddr;
                    rel[n].kind = type;
                    rel[n].length = length;
                    rel[n].pcrel = pcrel;
                    rel[n].addend = pending_addend;
                    pending_addend = 0;
                    if (!ext)
                        die("%s: unsupported section-based relocation", path);
                    rel[n].target_sym = o->symmap[symnum];
                    n++;
                }
                o->secs[kind].relocs = rel;
                o->secs[kind].nrelocs = n;
            }
        }
    }
}
