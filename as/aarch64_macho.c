/* aarch64_macho.c : Mach-O (MH_OBJECT) relocatable writer for the Darwin arm64
 * target, the counterpart of as/aarch64_elf.c.  It consumes the same populated
 * struct a64_asm (sections, symbols, relocations) and writes a 64-bit little-
 * endian arm64 object: a mach_header_64, one LC_SEGMENT_64 holding the present
 * sections, LC_SYMTAB and LC_DYSYMTAB, the section bytes, the per-section
 * relocation_info arrays, the symbol table (nlist_64) and the string table.
 *
 * The assembler records the neutral ELF reloc code in reloc->type; this writer
 * maps it to an ARM64_RELOC_* kind.  Mach-O relocations carry no addend field,
 * so a data symbol reference (ARM64_RELOC_UNSIGNED) keeps its addend inline in
 * the section bytes, and a nonzero addend on an instruction relocation is
 * carried by a preceding ARM64_RELOC_ADDEND entry.
 */

#include "aarch64.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* Mach-O constants */
#define MH_MAGIC_64        0xFEEDFACFu
#define CPU_TYPE_ARM64     0x0100000Cu     /* CPU_TYPE_ARM | CPU_ARCH_ABI64 */
#define CPU_SUBTYPE_ARM64_ALL 0u
#define MH_OBJECT          1u

#define LC_SYMTAB          0x02u
#define LC_DYSYMTAB        0x0Bu
#define LC_SEGMENT_64      0x19u
#define LC_BUILD_VERSION   0x32u
#define PLATFORM_MACOS     1u

#define S_REGULAR               0x0u
#define S_ZEROFILL              0x1u
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u
#define S_ATTR_DEBUG            0x02000000u

#define N_EXT              0x01u
#define N_SECT             0x0Eu
#define N_UNDF             0x00u

#define VM_PROT_READ       0x1u
#define VM_PROT_WRITE      0x2u
#define VM_PROT_EXECUTE    0x4u

/* ARM64 relocation kinds (r_type) */
#define ARM64_RELOC_UNSIGNED   0
#define ARM64_RELOC_BRANCH26   2
#define ARM64_RELOC_PAGE21     3
#define ARM64_RELOC_PAGEOFF12  4
#define ARM64_RELOC_ADDEND     10

/****************************************************************
 * Little-endian write helpers
 ****************************************************************/

static void
put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void
put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void
put64(uint8_t *p, uint64_t v)
{
    put32(p + 0, (uint32_t)(v & 0xffffffffu));
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t
align_up(uint64_t v, uint64_t a)
{
    return a > 1 ? (v + a - 1) & ~(a - 1) : v;
}

static uint64_t
sec_align_bytes(const struct section *s)
{
    return s->align > 1 ? (uint64_t)s->align : 1;
}

static int
align_log2(uint64_t a)
{
    int n = 0;
    while (a > 1) { a >>= 1; n++; }
    return n;
}

/****************************************************************
 * String table builder (same shape as the ELF writer's)
 ****************************************************************/

struct mstrtab {
    char *data;
    int len;
    int cap;
};

static void
mstrtab_init(struct mstrtab *st)
{
    st->cap = 256;
    st->data = xmalloc(st->cap);
    st->data[0] = '\0';         /* index 0 is the empty string */
    st->len = 1;
}

static int
mstrtab_add(struct mstrtab *st, const char *s)
{
    int slen = (int)strlen(s) + 1;
    int pos = st->len;

    while (st->len + slen > st->cap) {
        st->cap *= 2;
        st->data = realloc(st->data, st->cap);
    }
    memcpy(st->data + st->len, s, slen);
    st->len += slen;
    return pos;
}

/****************************************************************
 * Section bookkeeping
 ****************************************************************/

/* One output Mach-O section.  The zerofill __bss carries no file bytes. */
struct msec {
    int xsec;                   /* ASEC_* source id */
    const char *segname;        /* "__TEXT" / "__DATA" */
    const char *sectname;       /* "__text" / "__const" / "__data" / "__bss" */
    uint32_t flags;
    int zerofill;
    uint64_t addr;              /* vm address within the segment */
    uint64_t size;
    uint32_t offset;            /* file offset (0 for zerofill) */
    uint32_t reloff;
    uint32_t nreloc;
};

/* An nlist_64 symbol under construction. */
struct mnlist {
    uint32_t n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;
    uint16_t n_desc;
    uint64_t n_value;
};

void
a64_macho_write(struct a64_asm *a, FILE *out)
{
    struct mstrtab strtab;
    struct msec secs[ASEC_COUNT];
    int nsec = 0;
    int macho_secnum[ASEC_COUNT];   /* 1-based Mach-O section number, 0 if absent */
    uint64_t sec_vmaddr[ASEC_COUNT];
    int k;

    for (k = 0; k < ASEC_COUNT; k++) {
        macho_secnum[k] = 0;
        sec_vmaddr[k] = 0;
    }

    /* Which sections are present (nonempty).  Order: __text, __const, __data,
       the __DWARF debug sections, then the zerofill __bss last, as Mach-O
       requires.  The __DWARF sections carry S_ATTR_DEBUG and are empty unless
       the input had .loc rows (a64_emit_debug), so a non-g object is unchanged. */
    {
        struct { int xsec; const char *seg, *sect; uint32_t flags; int zf; }
        want[ASEC_COUNT] = {
            { ASEC_TEXT,   "__TEXT", "__text",
              S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, 0 },
            { ASEC_RODATA, "__DATA", "__const", S_REGULAR, 0 },
            { ASEC_DATA,   "__DATA", "__data",  S_REGULAR, 0 },
            { ASEC_DEBUG_LINE,   "__DWARF", "__debug_line",   S_ATTR_DEBUG, 0 },
            { ASEC_DEBUG_INFO,   "__DWARF", "__debug_info",   S_ATTR_DEBUG, 0 },
            { ASEC_DEBUG_ABBREV, "__DWARF", "__debug_abbrev", S_ATTR_DEBUG, 0 },
            { ASEC_BSS,    "__DATA", "__bss",   S_ZEROFILL, 1 },
        };
        int i;
        for (i = 0; i < ASEC_COUNT; i++) {
            int xs = want[i].xsec;
            if (a->sections[xs].len == 0)
                continue;
            secs[nsec].xsec = xs;
            secs[nsec].segname = want[i].seg;
            secs[nsec].sectname = want[i].sect;
            secs[nsec].flags = want[i].flags;
            secs[nsec].zerofill = want[i].zf;
            secs[nsec].size = (uint64_t)a->sections[xs].len;
            macho_secnum[xs] = nsec + 1;
            nsec++;
        }
    }

    /* VM layout: addresses accumulate within the single segment from 0. */
    {
        uint64_t va = 0;
        for (k = 0; k < nsec; k++) {
            uint64_t al = sec_align_bytes(&a->sections[secs[k].xsec]);
            va = align_up(va, al);
            secs[k].addr = va;
            sec_vmaddr[secs[k].xsec] = va;
            va += secs[k].size;
        }
    }

    /* Symbol table, ordered local / external-defined / undefined.  Every symbol
       of the assembler table is carried so a relocation can reference it by its
       index with r_extern=1 (local symbols referenced across sections included). */
    mstrtab_init(&strtab);
    int nsyms = a->st.nsyms;
    struct mnlist *nl = xmalloc((nsyms ? nsyms : 1) * sizeof(struct mnlist));
    int *sym_order = xmalloc((nsyms ? nsyms : 1) * sizeof(int));
    int mi = 0;
    int nlocal = 0, nextdef = 0, nundef = 0;

    /* pass order: locals, then external defined, then undefined */
    for (k = 0; k < nsyms; k++) {
        if (a->st.syms[k].defined && !a->st.syms[k].global) {
            nl[mi].n_strx = (uint32_t)mstrtab_add(&strtab, a->st.syms[k].name);
            nl[mi].n_type = N_SECT;
            nl[mi].n_sect = (uint8_t)macho_secnum[a->st.syms[k].section];
            nl[mi].n_desc = 0;
            nl[mi].n_value = sec_vmaddr[a->st.syms[k].section]
                           + a->st.syms[k].value;
            sym_order[k] = mi++;
            nlocal++;
        }
    }
    for (k = 0; k < nsyms; k++) {
        if (a->st.syms[k].defined && a->st.syms[k].global) {
            nl[mi].n_strx = (uint32_t)mstrtab_add(&strtab, a->st.syms[k].name);
            nl[mi].n_type = N_SECT | N_EXT;
            nl[mi].n_sect = (uint8_t)macho_secnum[a->st.syms[k].section];
            nl[mi].n_desc = 0;
            nl[mi].n_value = sec_vmaddr[a->st.syms[k].section]
                           + a->st.syms[k].value;
            sym_order[k] = mi++;
            nextdef++;
        }
    }
    for (k = 0; k < nsyms; k++) {
        if (!a->st.syms[k].defined) {
            nl[mi].n_strx = (uint32_t)mstrtab_add(&strtab, a->st.syms[k].name);
            nl[mi].n_type = N_UNDF | N_EXT;
            nl[mi].n_sect = 0;
            nl[mi].n_desc = 0;
            nl[mi].n_value = 0;
            sym_order[k] = mi++;
            nundef++;
        }
    }

    /* Build the per-section relocation_info arrays (8 bytes each).  A nonzero
       addend on an instruction relocation gets a preceding ARM64_RELOC_ADDEND;
       a data (UNSIGNED) addend is patched inline into the section bytes. */
    uint8_t *relbuf[ASEC_COUNT];
    int relcount[ASEC_COUNT];
    for (k = 0; k < ASEC_COUNT; k++) { relbuf[k] = NULL; relcount[k] = 0; }

    for (k = 0; k < nsec; k++) {
        int xs = secs[k].xsec;
        struct section *s = &a->sections[xs];
        int j, cap = s->nrelocs * 2 + 1;
        uint8_t *buf = xmalloc((size_t)cap * 8);
        int n = 0;

        for (j = 0; j < s->nrelocs; j++) {
            struct reloc *r = &s->relocs[j];
            int rtype, length, pcrel;
            int32_t addend = r->addend;

            switch (r->type) {
            case R_AARCH64_ABS64:
                rtype = ARM64_RELOC_UNSIGNED; length = 3; pcrel = 0; break;
            case R_AARCH64_ABS32:
                rtype = ARM64_RELOC_UNSIGNED; length = 2; pcrel = 0; break;
            case R_AARCH64_CALL26:
            case R_AARCH64_JUMP26:
                rtype = ARM64_RELOC_BRANCH26; length = 2; pcrel = 1; break;
            case R_AARCH64_ADR_PREL_PG_HI21:
                rtype = ARM64_RELOC_PAGE21; length = 2; pcrel = 1; break;
            case R_AARCH64_ADD_ABS_LO12_NC:
                rtype = ARM64_RELOC_PAGEOFF12; length = 2; pcrel = 0; break;
            default:
                die("aarch64 mach-o: unsupported relocation type %d", r->type);
                return;
            }

            if (rtype == ARM64_RELOC_UNSIGNED) {
                /* addend lives inline in the section bytes */
                if (addend) {
                    uint32_t off = r->offset;
                    if (length == 3)
                        put64(s->data + off, (uint64_t)(int64_t)addend);
                    else
                        put32(s->data + off, (uint32_t)addend);
                }
            } else if (addend) {
                /* ARM64_RELOC_ADDEND carries the addend in r_symbolnum */
                uint8_t *p = buf + n * 8;
                uint32_t w = ((uint32_t)addend & 0x00FFFFFFu)
                           | (0u << 24)          /* r_pcrel = 0 */
                           | (2u << 25)          /* r_length = 2 */
                           | (0u << 27)          /* r_extern = 0 */
                           | ((uint32_t)ARM64_RELOC_ADDEND << 28);
                put32(p + 0, r->offset);
                put32(p + 4, w);
                n++;
            }

            {
                uint8_t *p = buf + n * 8;
                uint32_t symnum = (uint32_t)sym_order[r->sym_idx];
                uint32_t w = (symnum & 0x00FFFFFFu)
                           | ((uint32_t)(pcrel & 1) << 24)
                           | ((uint32_t)(length & 3) << 25)
                           | (1u << 27)          /* r_extern = 1 */
                           | ((uint32_t)rtype << 28);
                put32(p + 0, r->offset);
                put32(p + 4, w);
                n++;
            }
        }
        relbuf[xs] = buf;
        relcount[xs] = n;
    }

    /* File layout.  Header + load commands, then section bytes, then the
       relocations, then the symbol table and string table. */
    uint32_t sizeofcmds = 72 + (uint32_t)nsec * 80 + 24 + 24 + 80;
    uint32_t ncmds = 4;         /* SEGMENT_64, BUILD_VERSION, SYMTAB, DYSYMTAB */
    uint64_t hdr_end = 32 + sizeofcmds;

    uint64_t fo = hdr_end;
    uint64_t seg_fileoff = hdr_end, seg_filesize = 0;
    for (k = 0; k < nsec; k++) {
        if (secs[k].zerofill) {
            secs[k].offset = 0;
            continue;
        }
        uint64_t al = sec_align_bytes(&a->sections[secs[k].xsec]);
        fo = align_up(fo, al);
        secs[k].offset = (uint32_t)fo;
        fo += secs[k].size;
        seg_filesize = fo - seg_fileoff;
    }

    /* relocations (4-byte aligned) */
    for (k = 0; k < nsec; k++) {
        int xs = secs[k].xsec;
        if (relcount[xs] == 0) { secs[k].reloff = 0; secs[k].nreloc = 0; continue; }
        fo = align_up(fo, 4);
        secs[k].reloff = (uint32_t)fo;
        secs[k].nreloc = (uint32_t)relcount[xs];
        fo += (uint64_t)relcount[xs] * 8;
    }

    fo = align_up(fo, 8);
    uint64_t symoff = fo;
    fo += (uint64_t)mi * 16;
    uint64_t stroff = fo;
    fo += (uint64_t)strtab.len;

    uint64_t seg_vmsize = 0;
    for (k = 0; k < nsec; k++) {
        uint64_t end = secs[k].addr + secs[k].size;
        if (end > seg_vmsize)
            seg_vmsize = end;
    }

    /* ---- mach_header_64 ---- */
    uint8_t hdr[32];
    memset(hdr, 0, sizeof(hdr));
    put32(hdr + 0, MH_MAGIC_64);
    put32(hdr + 4, CPU_TYPE_ARM64);
    put32(hdr + 8, CPU_SUBTYPE_ARM64_ALL);
    put32(hdr + 12, MH_OBJECT);
    put32(hdr + 16, ncmds);
    put32(hdr + 20, sizeofcmds);
    put32(hdr + 24, 0);                 /* flags */
    put32(hdr + 28, 0);                 /* reserved */
    fwrite(hdr, 1, 32, out);

    /* ---- LC_SEGMENT_64 + section_64[] ---- */
    {
        uint8_t seg[72];
        memset(seg, 0, sizeof(seg));
        put32(seg + 0, LC_SEGMENT_64);
        put32(seg + 4, 72 + (uint32_t)nsec * 80);
        /* segname: empty for an MH_OBJECT single segment */
        put64(seg + 24, 0);             /* vmaddr */
        put64(seg + 32, seg_vmsize);    /* vmsize */
        put64(seg + 40, seg_fileoff);   /* fileoff */
        put64(seg + 48, seg_filesize);  /* filesize */
        put32(seg + 56, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE); /* maxprot */
        put32(seg + 60, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE); /* initprot */
        put32(seg + 64, (uint32_t)nsec);
        put32(seg + 68, 0);             /* flags */
        fwrite(seg, 1, 72, out);

        for (k = 0; k < nsec; k++) {
            uint8_t sc[80];
            memset(sc, 0, sizeof(sc));
            strncpy((char *)sc + 0, secs[k].sectname, 16);
            strncpy((char *)sc + 16, secs[k].segname, 16);
            put64(sc + 32, secs[k].addr);
            put64(sc + 40, secs[k].size);
            put32(sc + 48, secs[k].offset);
            put32(sc + 52, (uint32_t)align_log2(
                      sec_align_bytes(&a->sections[secs[k].xsec])));
            put32(sc + 56, secs[k].reloff);
            put32(sc + 60, secs[k].nreloc);
            put32(sc + 64, secs[k].flags);
            fwrite(sc, 1, 80, out);
        }
    }

    /* ---- LC_BUILD_VERSION ---- (names the platform so ld does not warn) */
    {
        uint8_t c[24];
        memset(c, 0, sizeof(c));
        put32(c + 0, LC_BUILD_VERSION);
        put32(c + 4, 24);
        put32(c + 8, PLATFORM_MACOS);
        put32(c + 12, 0x000B0000u);     /* minos 11.0.0 */
        put32(c + 16, 0x000B0000u);     /* sdk   11.0.0 */
        put32(c + 20, 0);               /* ntools */
        fwrite(c, 1, 24, out);
    }

    /* ---- LC_SYMTAB ---- */
    {
        uint8_t c[24];
        memset(c, 0, sizeof(c));
        put32(c + 0, LC_SYMTAB);
        put32(c + 4, 24);
        put32(c + 8, (uint32_t)symoff);
        put32(c + 12, (uint32_t)mi);
        put32(c + 16, (uint32_t)stroff);
        put32(c + 20, (uint32_t)strtab.len);
        fwrite(c, 1, 24, out);
    }

    /* ---- LC_DYSYMTAB ---- */
    {
        uint8_t c[80];
        memset(c, 0, sizeof(c));
        put32(c + 0, LC_DYSYMTAB);
        put32(c + 4, 80);
        put32(c + 8, 0);                        /* ilocalsym */
        put32(c + 12, (uint32_t)nlocal);        /* nlocalsym */
        put32(c + 16, (uint32_t)nlocal);        /* iextdefsym */
        put32(c + 20, (uint32_t)nextdef);       /* nextdefsym */
        put32(c + 24, (uint32_t)(nlocal + nextdef)); /* iundefsym */
        put32(c + 28, (uint32_t)nundef);        /* nundefsym */
        /* the remaining table offsets are unused in an object */
        fwrite(c, 1, 80, out);
    }

    /* ---- section bytes ---- */
    {
        uint64_t cur = hdr_end;
        for (k = 0; k < nsec; k++) {
            if (secs[k].zerofill)
                continue;
            while (cur < secs[k].offset) { fputc(0, out); cur++; }
            fwrite(a->sections[secs[k].xsec].data, 1,
                   (size_t)secs[k].size, out);
            cur += secs[k].size;
        }

        /* ---- relocations ---- */
        for (k = 0; k < nsec; k++) {
            int xs = secs[k].xsec;
            if (secs[k].nreloc == 0)
                continue;
            while (cur < secs[k].reloff) { fputc(0, out); cur++; }
            fwrite(relbuf[xs], 1, (size_t)secs[k].nreloc * 8, out);
            cur += (uint64_t)secs[k].nreloc * 8;
        }

        /* ---- symbol table ---- */
        while (cur < symoff) { fputc(0, out); cur++; }
        for (k = 0; k < mi; k++) {
            uint8_t sb[16];
            put32(sb + 0, nl[k].n_strx);
            sb[4] = nl[k].n_type;
            sb[5] = nl[k].n_sect;
            put16(sb + 6, nl[k].n_desc);
            put64(sb + 8, nl[k].n_value);
            fwrite(sb, 1, 16, out);
            cur += 16;
        }

        /* ---- string table ---- */
        fwrite(strtab.data, 1, (size_t)strtab.len, out);
    }

    for (k = 0; k < ASEC_COUNT; k++)
        free(relbuf[k]);
    free(nl);
    free(sym_order);
    free(strtab.data);
}
