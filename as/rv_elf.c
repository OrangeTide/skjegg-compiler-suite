/* rv_elf.c : ELF32 little-endian relocatable object writer for RISC-V
 *
 * Mirrors the ColdFire writer's structure (fixed section table, section
 * symbols, local-then-global symbol ordering, local-defined relocations
 * folded onto the section symbol with the value carried in the addend) but
 * emits little-endian EM_RISCV objects and preserves each relocation's own
 * type code. */

#include "rv.h"

#include <stdlib.h>
#include <string.h>

#define ELFMAG        "\177ELF"
#define ELFCLASS32    1
#define ELFDATA2LSB   1
#define EV_CURRENT    1
#define ELFOSABI_NONE 0

#define ET_REL       1
#define EM_RISCV     243

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8

#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4

#define STB_LOCAL    0
#define STB_GLOBAL   1
#define STB_WEAK     2
#define STT_NOTYPE   0
#define STT_FUNC     2
#define STT_SECTION  3

#define SHN_UNDEF    0

#define ELF32_ST_INFO(b, t) (((b) << 4) | ((t) & 0xF))

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

static uint32_t
sec_addralign(const struct section *s)
{
    return s->align > 4 ? (uint32_t)s->align : 4;
}

/****************************************************************
 * String table builder
 ****************************************************************/

struct strtab {
    char *data;
    int len;
    int cap;
};

static void
strtab_init(struct strtab *st)
{
    st->cap = 256;
    st->data = xmalloc(st->cap);
    st->data[0] = '\0';
    st->len = 1;
}

static int
strtab_add(struct strtab *st, const char *s)
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
 * ELF symbol builder
 ****************************************************************/

struct elf_sym {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
};

static void
write_sym(uint8_t *buf, struct elf_sym *s)
{
    put32(buf + 0,  s->st_name);
    put32(buf + 4,  s->st_value);
    put32(buf + 8,  s->st_size);
    buf[12] = s->st_info;
    buf[13] = s->st_other;
    put16(buf + 14, s->st_shndx);
}

/****************************************************************
 * Section indices
 ****************************************************************/

enum {
    SH_NULL,
    SH_TEXT,
    SH_RODATA,
    SH_DATA,
    SH_BSS,
    SH_SYMTAB,
    SH_STRTAB,
    SH_RELA_TEXT,
    SH_RELA_RODATA,
    SH_RELA_DATA,
    SH_DEBUG_LINE,
    SH_DEBUG_INFO,
    SH_DEBUG_ABBREV,
    SH_RELA_DEBUG_LINE,
    SH_RELA_DEBUG_INFO,
    SH_SHSTRTAB,
    SH_COUNT,
};

/* Map an assembler section id to its ELF section header index. */
static int
rsec_shndx(int rsec)
{
    switch (rsec) {
    case RSEC_TEXT:         return SH_TEXT;
    case RSEC_RODATA:       return SH_RODATA;
    case RSEC_DATA:         return SH_DATA;
    case RSEC_BSS:          return SH_BSS;
    case RSEC_DEBUG_LINE:   return SH_DEBUG_LINE;
    case RSEC_DEBUG_INFO:   return SH_DEBUG_INFO;
    case RSEC_DEBUG_ABBREV: return SH_DEBUG_ABBREV;
    default:                return SHN_UNDEF;
    }
}

/* Like rsec_shndx but also maps an extra named section (index >= RSEC_COUNT) to
   its appended progbits header, which sits right after the fixed SH_* headers
   (one per named section, in section-index order). */
static int
rshndx_of(int rsec)
{
    if (rsec >= RSEC_COUNT)
        return SH_COUNT + (rsec - RSEC_COUNT);
    return rsec_shndx(rsec);
}

/****************************************************************
 * ELF writer
 ****************************************************************/

void
rv_elf_write(struct rv_asm *a, FILE *out)
{
    struct strtab shstrtab;
    struct strtab strtab;
    int shname[SH_COUNT];
    uint8_t ehdr[52];
    uint8_t shdr[SH_COUNT][40];
    uint32_t offset;
    int nsyms_local, nsyms_total;
    int k, j;
    int *sec_sym;               /* esym index of each section's STT_SECTION sym */
    int *sym_map;
    struct elf_sym *esyms;
    int nesyms = 0, esym_cap;
    int len[RSEC_COUNT];
    int nrel[RSEC_COUNT];

    /* Extra named sections (.text.<fn> etc.) under -ffunction-sections: each
       gets one appended progbits header (at SH_COUNT + its position) and, when
       it has relocations, one appended rela header after all the progbits ones.
       When there are none the file is byte-identical to the fixed-layout form. */
    int nextra = a->nsections - RSEC_COUNT;
    int n_named_rela = 0;
    int *nprog_name = nextra ? xmalloc(nextra * sizeof(int)) : NULL;
    int *nrela_name = nextra ? xmalloc(nextra * sizeof(int)) : NULL;
    int *nrela_sh   = nextra ? xmalloc(nextra * sizeof(int)) : NULL;
    uint32_t *noff      = nextra ? xmalloc(nextra * sizeof(uint32_t)) : NULL;
    uint32_t *nrela_off = nextra ? xmalloc(nextra * sizeof(uint32_t)) : NULL;
    uint8_t **nrela_buf = nextra ? xmalloc(nextra * sizeof(uint8_t *)) : NULL;
    int *nrela_size = nextra ? xmalloc(nextra * sizeof(int)) : NULL;
    uint8_t (*nshdr)[40] = NULL;

    sec_sym = xmalloc(a->nsections * sizeof(int));

    for (k = 0; k < RSEC_COUNT; k++) {
        len[k] = a->sections[k].len;
        nrel[k] = a->sections[k].nrelocs;
    }
    for (j = 0; j < nextra; j++)
        if (a->sections[RSEC_COUNT + j].nrelocs)
            n_named_rela++;

    strtab_init(&shstrtab);
    shname[SH_NULL] = 0;
    shname[SH_TEXT] = strtab_add(&shstrtab, ".text");
    shname[SH_RODATA] = strtab_add(&shstrtab, ".rodata");
    shname[SH_DATA] = strtab_add(&shstrtab, ".data");
    shname[SH_BSS] = strtab_add(&shstrtab, ".bss");
    shname[SH_SYMTAB] = strtab_add(&shstrtab, ".symtab");
    shname[SH_STRTAB] = strtab_add(&shstrtab, ".strtab");
    shname[SH_RELA_TEXT] = strtab_add(&shstrtab, ".rela.text");
    shname[SH_RELA_RODATA] = strtab_add(&shstrtab, ".rela.rodata");
    shname[SH_RELA_DATA] = strtab_add(&shstrtab, ".rela.data");
    shname[SH_DEBUG_LINE] = strtab_add(&shstrtab, ".debug_line");
    shname[SH_DEBUG_INFO] = strtab_add(&shstrtab, ".debug_info");
    shname[SH_DEBUG_ABBREV] = strtab_add(&shstrtab, ".debug_abbrev");
    shname[SH_RELA_DEBUG_LINE] = strtab_add(&shstrtab, ".rela.debug_line");
    shname[SH_RELA_DEBUG_INFO] = strtab_add(&shstrtab, ".rela.debug_info");
    shname[SH_SHSTRTAB] = strtab_add(&shstrtab, ".shstrtab");

    /* Names for the appended named sections (and their rela sections), after
       all the fixed names so the base shstrtab bytes are unchanged.  Assign the
       rela header indices in the same pass: SH_COUNT + nextra onward. */
    {
        int rsh = SH_COUNT + nextra;
        for (j = 0; j < nextra; j++) {
            struct section *s = &a->sections[RSEC_COUNT + j];
            nprog_name[j] = strtab_add(&shstrtab, s->name);
            if (s->nrelocs) {
                char rbuf[256];
                snprintf(rbuf, sizeof(rbuf), ".rela%s", s->name);
                nrela_name[j] = strtab_add(&shstrtab, rbuf);
                nrela_sh[j] = rsh++;
            } else {
                nrela_name[j] = 0;
                nrela_sh[j] = -1;
            }
        }
    }

    /* Symbol table: null, section syms, local defined, global/undefined. */
    strtab_init(&strtab);
    sym_map = xmalloc((a->st.nsyms ? a->st.nsyms : 1) * sizeof(int));
    esym_cap = 1 + a->nsections + a->st.nsyms;
    esyms = xmalloc(esym_cap * sizeof(struct elf_sym));

    memset(&esyms[nesyms], 0, sizeof(struct elf_sym));
    nesyms++;

    for (k = 0; k < RSEC_COUNT; k++) {
        esyms[nesyms] = (struct elf_sym){
            .st_name = 0, .st_value = 0, .st_size = 0,
            .st_info = ELF32_ST_INFO(STB_LOCAL, STT_SECTION),
            .st_other = 0, .st_shndx = (uint16_t)rsec_shndx(k),
        };
        sec_sym[k] = nesyms++;
    }
    for (k = RSEC_COUNT; k < a->nsections; k++) {
        esyms[nesyms] = (struct elf_sym){
            .st_name = 0, .st_value = 0, .st_size = 0,
            .st_info = ELF32_ST_INFO(STB_LOCAL, STT_SECTION),
            .st_other = 0, .st_shndx = (uint16_t)rshndx_of(k),
        };
        sec_sym[k] = nesyms++;
    }

    for (k = 0; k < a->st.nsyms; k++) {
        if (a->st.syms[k].global || !a->st.syms[k].defined)
            continue;
        esyms[nesyms] = (struct elf_sym){
            .st_name = (uint32_t)strtab_add(&strtab, a->st.syms[k].name),
            .st_value = a->st.syms[k].value,
            .st_size = a->st.syms[k].size,
            .st_info = ELF32_ST_INFO(STB_LOCAL,
                a->st.syms[k].is_func ? STT_FUNC : STT_NOTYPE),
            .st_other = 0,
            .st_shndx = (uint16_t)rshndx_of(a->st.syms[k].section),
        };
        sym_map[k] = nesyms++;
    }

    nsyms_local = nesyms;

    for (k = 0; k < a->st.nsyms; k++) {
        uint16_t shndx;
        if (!a->st.syms[k].global && a->st.syms[k].defined)
            continue;
        if (!a->st.syms[k].defined)
            shndx = SHN_UNDEF;
        else
            shndx = (uint16_t)rshndx_of(a->st.syms[k].section);
        esyms[nesyms] = (struct elf_sym){
            .st_name = (uint32_t)strtab_add(&strtab, a->st.syms[k].name),
            .st_value = a->st.syms[k].defined ? a->st.syms[k].value : 0,
            .st_size = a->st.syms[k].size,
            .st_info = ELF32_ST_INFO(
                a->st.syms[k].weak ? STB_WEAK : STB_GLOBAL,
                a->st.syms[k].is_func ? STT_FUNC : STT_NOTYPE),
            .st_other = 0,
            .st_shndx = shndx,
        };
        sym_map[k] = nesyms++;
    }

    nsyms_total = nesyms;

    /* Build RELA buffers for text, rodata and data. */
    uint8_t *rela_buf[RSEC_COUNT];
    int rela_size[RSEC_COUNT];
    for (k = 0; k < RSEC_COUNT; k++) {
        rela_size[k] = nrel[k] * 12;
        rela_buf[k] = nrel[k] ? xmalloc(rela_size[k]) : NULL;
    }

    for (k = 0; k < RSEC_COUNT; k++) {
        int j;
        for (j = 0; j < nrel[k]; j++) {
            struct reloc *r = &a->sections[k].relocs[j];
            uint8_t *p = rela_buf[k] + j * 12;
            int elf_sym_idx = sym_map[r->sym_idx];
            int32_t addend = r->addend;

            if (a->st.syms[r->sym_idx].defined
                && !a->st.syms[r->sym_idx].global) {
                addend += (int32_t)a->st.syms[r->sym_idx].value;
                elf_sym_idx = sec_sym[a->st.syms[r->sym_idx].section];
            }
            put32(p + 0, r->offset);
            put32(p + 4, (uint32_t)(((uint32_t)elf_sym_idx << 8)
                                    | (uint32_t)(r->type & 0xff)));
            put32(p + 8, (uint32_t)addend);
        }
    }

    /* Named-section rela buffers (same encoding as the base loop above). */
    for (j = 0; j < nextra; j++) {
        struct section *s = &a->sections[RSEC_COUNT + j];
        int i;
        nrela_size[j] = s->nrelocs * 12;
        nrela_buf[j] = s->nrelocs ? xmalloc(nrela_size[j]) : NULL;
        for (i = 0; i < s->nrelocs; i++) {
            struct reloc *r = &s->relocs[i];
            uint8_t *p = nrela_buf[j] + i * 12;
            int elf_sym_idx = sym_map[r->sym_idx];
            int32_t addend = r->addend;

            if (a->st.syms[r->sym_idx].defined
                && !a->st.syms[r->sym_idx].global) {
                addend += (int32_t)a->st.syms[r->sym_idx].value;
                elf_sym_idx = sec_sym[a->st.syms[r->sym_idx].section];
            }
            put32(p + 0, r->offset);
            put32(p + 4, (uint32_t)(((uint32_t)elf_sym_idx << 8)
                                    | (uint32_t)(r->type & 0xff)));
            put32(p + 8, (uint32_t)addend);
        }
    }

    /*
     * File layout: ehdr, .text, .rodata, .data, .symtab, .strtab,
     * .rela.text, .rela.rodata, .rela.data, .shstrtab, section headers.
     */
    int symtab_size = nsyms_total * 16;
    uint32_t sec_off[RSEC_COUNT];
    uint32_t symtab_off, strtab_off, rela_off[RSEC_COUNT], shstrtab_off;
    uint32_t shdr_off;

    offset = 52;
    sec_off[RSEC_TEXT] = offset;   offset += (uint32_t)len[RSEC_TEXT];
    sec_off[RSEC_RODATA] = offset; offset += (uint32_t)len[RSEC_RODATA];
    sec_off[RSEC_DATA] = offset;   offset += (uint32_t)len[RSEC_DATA];
    sec_off[RSEC_DEBUG_LINE] = offset;
    offset += (uint32_t)len[RSEC_DEBUG_LINE];
    sec_off[RSEC_DEBUG_INFO] = offset;
    offset += (uint32_t)len[RSEC_DEBUG_INFO];
    sec_off[RSEC_DEBUG_ABBREV] = offset;
    offset += (uint32_t)len[RSEC_DEBUG_ABBREV];
    sec_off[RSEC_BSS] = offset;    /* NOBITS: no file bytes */

    /* Named-section bytes follow the base section bytes (SECK_BSS carries none,
       like the base .bss).  Empty when there are no named sections. */
    for (j = 0; j < nextra; j++) {
        struct section *s = &a->sections[RSEC_COUNT + j];
        noff[j] = offset;
        if (s->kind != SECK_BSS)
            offset += (uint32_t)s->len;
    }

    symtab_off = offset;           offset += (uint32_t)symtab_size;
    strtab_off = offset;           offset += (uint32_t)strtab.len;

    rela_off[RSEC_TEXT] = offset;  offset += (uint32_t)rela_size[RSEC_TEXT];
    rela_off[RSEC_RODATA] = offset; offset += (uint32_t)rela_size[RSEC_RODATA];
    rela_off[RSEC_DATA] = offset;  offset += (uint32_t)rela_size[RSEC_DATA];
    rela_off[RSEC_DEBUG_LINE] = offset;
    offset += (uint32_t)rela_size[RSEC_DEBUG_LINE];
    rela_off[RSEC_DEBUG_INFO] = offset;
    offset += (uint32_t)rela_size[RSEC_DEBUG_INFO];
    rela_off[RSEC_DEBUG_ABBREV] = offset;  /* no relocations, size 0 */

    /* Named-section rela buffers follow the base rela buffers. */
    for (j = 0; j < nextra; j++) {
        nrela_off[j] = offset;
        offset += (uint32_t)nrela_size[j];
    }

    shstrtab_off = offset;         offset += (uint32_t)shstrtab.len;
    shdr_off = offset;

    /* ELF header */
    memset(ehdr, 0, sizeof(ehdr));
    memcpy(ehdr, ELFMAG, 4);
    ehdr[4] = ELFCLASS32;
    ehdr[5] = ELFDATA2LSB;
    ehdr[6] = EV_CURRENT;
    ehdr[7] = ELFOSABI_NONE;
    put16(ehdr + 16, ET_REL);
    put16(ehdr + 18, EM_RISCV);
    put32(ehdr + 20, EV_CURRENT);
    put32(ehdr + 32, shdr_off);    /* e_shoff */
    put32(ehdr + 36, 0);           /* e_flags: ilp32 soft-float ABI, no RVC */
    put16(ehdr + 40, 52);          /* e_ehsize */
    put16(ehdr + 46, 40);          /* e_shentsize */
    put16(ehdr + 48, (uint16_t)(SH_COUNT + nextra + n_named_rela)); /* e_shnum */
    put16(ehdr + 50, SH_SHSTRTAB); /* e_shstrndx */

    memset(shdr, 0, sizeof(shdr));

    /* SH_TEXT */
    put32(shdr[SH_TEXT] + 0, (uint32_t)shname[SH_TEXT]);
    put32(shdr[SH_TEXT] + 4, SHT_PROGBITS);
    put32(shdr[SH_TEXT] + 8, SHF_ALLOC | SHF_EXECINSTR);
    put32(shdr[SH_TEXT] + 16, sec_off[RSEC_TEXT]);
    put32(shdr[SH_TEXT] + 20, (uint32_t)len[RSEC_TEXT]);
    put32(shdr[SH_TEXT] + 32, sec_addralign(&a->sections[RSEC_TEXT]));

    /* SH_RODATA */
    put32(shdr[SH_RODATA] + 0, (uint32_t)shname[SH_RODATA]);
    put32(shdr[SH_RODATA] + 4, SHT_PROGBITS);
    put32(shdr[SH_RODATA] + 8, SHF_ALLOC);
    put32(shdr[SH_RODATA] + 16, sec_off[RSEC_RODATA]);
    put32(shdr[SH_RODATA] + 20, (uint32_t)len[RSEC_RODATA]);
    put32(shdr[SH_RODATA] + 32, sec_addralign(&a->sections[RSEC_RODATA]));

    /* SH_DATA */
    put32(shdr[SH_DATA] + 0, (uint32_t)shname[SH_DATA]);
    put32(shdr[SH_DATA] + 4, SHT_PROGBITS);
    put32(shdr[SH_DATA] + 8, SHF_WRITE | SHF_ALLOC);
    put32(shdr[SH_DATA] + 16, sec_off[RSEC_DATA]);
    put32(shdr[SH_DATA] + 20, (uint32_t)len[RSEC_DATA]);
    put32(shdr[SH_DATA] + 32, sec_addralign(&a->sections[RSEC_DATA]));

    /* SH_BSS */
    put32(shdr[SH_BSS] + 0, (uint32_t)shname[SH_BSS]);
    put32(shdr[SH_BSS] + 4, SHT_NOBITS);
    put32(shdr[SH_BSS] + 8, SHF_WRITE | SHF_ALLOC);
    put32(shdr[SH_BSS] + 16, sec_off[RSEC_BSS]);
    put32(shdr[SH_BSS] + 20, (uint32_t)len[RSEC_BSS]);
    put32(shdr[SH_BSS] + 32, sec_addralign(&a->sections[RSEC_BSS]));

    /* SH_SYMTAB */
    put32(shdr[SH_SYMTAB] + 0, (uint32_t)shname[SH_SYMTAB]);
    put32(shdr[SH_SYMTAB] + 4, SHT_SYMTAB);
    put32(shdr[SH_SYMTAB] + 16, symtab_off);
    put32(shdr[SH_SYMTAB] + 20, (uint32_t)symtab_size);
    put32(shdr[SH_SYMTAB] + 24, SH_STRTAB);
    put32(shdr[SH_SYMTAB] + 28, (uint32_t)nsyms_local);
    put32(shdr[SH_SYMTAB] + 32, 4);
    put32(shdr[SH_SYMTAB] + 36, 16);

    /* SH_STRTAB */
    put32(shdr[SH_STRTAB] + 0, (uint32_t)shname[SH_STRTAB]);
    put32(shdr[SH_STRTAB] + 4, SHT_STRTAB);
    put32(shdr[SH_STRTAB] + 16, strtab_off);
    put32(shdr[SH_STRTAB] + 20, (uint32_t)strtab.len);
    put32(shdr[SH_STRTAB] + 36, 1);

    /* SH_DEBUG_LINE / INFO / ABBREV: non-alloc PROGBITS, byte-aligned. */
    {
        struct { int sh; int rsec; } dbg[3] = {
            { SH_DEBUG_LINE,   RSEC_DEBUG_LINE },
            { SH_DEBUG_INFO,   RSEC_DEBUG_INFO },
            { SH_DEBUG_ABBREV, RSEC_DEBUG_ABBREV },
        };
        int i;
        for (i = 0; i < 3; i++) {
            int sh = dbg[i].sh;
            put32(shdr[sh] + 0, (uint32_t)shname[sh]);
            put32(shdr[sh] + 4, SHT_PROGBITS);
            put32(shdr[sh] + 8, 0);     /* no SHF_ALLOC: debug, not loaded */
            put32(shdr[sh] + 16, sec_off[dbg[i].rsec]);
            put32(shdr[sh] + 20, (uint32_t)len[dbg[i].rsec]);
            put32(shdr[sh] + 32, 1);    /* byte-aligned */
        }
    }

    /* SH_RELA_TEXT / RODATA / DATA / DEBUG_LINE / DEBUG_INFO */
    {
        struct { int sh; int rsec; int target_sh; } rela[5] = {
            { SH_RELA_TEXT,   RSEC_TEXT,   SH_TEXT },
            { SH_RELA_RODATA, RSEC_RODATA, SH_RODATA },
            { SH_RELA_DATA,   RSEC_DATA,   SH_DATA },
            { SH_RELA_DEBUG_LINE, RSEC_DEBUG_LINE, SH_DEBUG_LINE },
            { SH_RELA_DEBUG_INFO, RSEC_DEBUG_INFO, SH_DEBUG_INFO },
        };
        int i;
        for (i = 0; i < 5; i++) {
            int sh = rela[i].sh;
            put32(shdr[sh] + 0, (uint32_t)shname[sh]);
            put32(shdr[sh] + 4, SHT_RELA);
            put32(shdr[sh] + 16, rela_off[rela[i].rsec]);
            put32(shdr[sh] + 20, (uint32_t)rela_size[rela[i].rsec]);
            put32(shdr[sh] + 24, SH_SYMTAB);
            put32(shdr[sh] + 28, (uint32_t)rela[i].target_sh);
            put32(shdr[sh] + 32, 4);
            put32(shdr[sh] + 36, 12);
        }
    }

    /* SH_SHSTRTAB */
    put32(shdr[SH_SHSTRTAB] + 0, (uint32_t)shname[SH_SHSTRTAB]);
    put32(shdr[SH_SHSTRTAB] + 4, SHT_STRTAB);
    put32(shdr[SH_SHSTRTAB] + 16, shstrtab_off);
    put32(shdr[SH_SHSTRTAB] + 20, (uint32_t)shstrtab.len);
    put32(shdr[SH_SHSTRTAB] + 36, 1);

    /* Appended named-section headers: the progbits headers (one per named
       section, in order) then the rela headers (for those with relocs). */
    if (nextra) {
        nshdr = xmalloc((size_t)(nextra + n_named_rela) * 40);
        memset(nshdr, 0, (size_t)(nextra + n_named_rela) * 40);
        for (j = 0; j < nextra; j++) {
            struct section *s = &a->sections[RSEC_COUNT + j];
            uint32_t flags, type;
            switch (s->kind) {
            case SECK_TEXT:   flags = SHF_ALLOC | SHF_EXECINSTR; type = SHT_PROGBITS; break;
            case SECK_RODATA: flags = SHF_ALLOC;                 type = SHT_PROGBITS; break;
            case SECK_BSS:    flags = SHF_WRITE | SHF_ALLOC;     type = SHT_NOBITS;   break;
            default:          flags = SHF_WRITE | SHF_ALLOC;     type = SHT_PROGBITS; break;
            }
            put32(nshdr[j] + 0, (uint32_t)nprog_name[j]);
            put32(nshdr[j] + 4, type);
            put32(nshdr[j] + 8, flags);
            put32(nshdr[j] + 16, noff[j]);
            put32(nshdr[j] + 20, (uint32_t)s->len);
            put32(nshdr[j] + 32, sec_addralign(s));
            if (nrela_sh[j] >= 0) {
                uint8_t *rh = nshdr[nrela_sh[j] - SH_COUNT];
                put32(rh + 0, (uint32_t)nrela_name[j]);
                put32(rh + 4, SHT_RELA);
                put32(rh + 16, nrela_off[j]);
                put32(rh + 20, (uint32_t)nrela_size[j]);
                put32(rh + 24, SH_SYMTAB);                     /* sh_link */
                put32(rh + 28, (uint32_t)rshndx_of(RSEC_COUNT + j)); /* sh_info */
                put32(rh + 32, 4);
                put32(rh + 36, 12);
            }
        }
    }

    /* Emit the file */
    fwrite(ehdr, 1, 52, out);
    if (len[RSEC_TEXT] > 0)
        fwrite(a->sections[RSEC_TEXT].data, 1, len[RSEC_TEXT], out);
    if (len[RSEC_RODATA] > 0)
        fwrite(a->sections[RSEC_RODATA].data, 1, len[RSEC_RODATA], out);
    if (len[RSEC_DATA] > 0)
        fwrite(a->sections[RSEC_DATA].data, 1, len[RSEC_DATA], out);
    if (len[RSEC_DEBUG_LINE] > 0)
        fwrite(a->sections[RSEC_DEBUG_LINE].data, 1, len[RSEC_DEBUG_LINE], out);
    if (len[RSEC_DEBUG_INFO] > 0)
        fwrite(a->sections[RSEC_DEBUG_INFO].data, 1, len[RSEC_DEBUG_INFO], out);
    if (len[RSEC_DEBUG_ABBREV] > 0)
        fwrite(a->sections[RSEC_DEBUG_ABBREV].data, 1,
               len[RSEC_DEBUG_ABBREV], out);

    /* Write named-section bytes (PROGBITS kinds only; NOBITS carries none). */
    for (j = 0; j < nextra; j++) {
        struct section *s = &a->sections[RSEC_COUNT + j];
        if (s->kind != SECK_BSS && s->len > 0)
            fwrite(s->data, 1, s->len, out);
    }

    for (k = 0; k < nsyms_total; k++) {
        uint8_t sbuf[16];
        write_sym(sbuf, &esyms[k]);
        fwrite(sbuf, 1, 16, out);
    }

    fwrite(strtab.data, 1, strtab.len, out);

    if (rela_size[RSEC_TEXT] > 0)
        fwrite(rela_buf[RSEC_TEXT], 1, rela_size[RSEC_TEXT], out);
    if (rela_size[RSEC_RODATA] > 0)
        fwrite(rela_buf[RSEC_RODATA], 1, rela_size[RSEC_RODATA], out);
    if (rela_size[RSEC_DATA] > 0)
        fwrite(rela_buf[RSEC_DATA], 1, rela_size[RSEC_DATA], out);
    if (rela_size[RSEC_DEBUG_LINE] > 0)
        fwrite(rela_buf[RSEC_DEBUG_LINE], 1, rela_size[RSEC_DEBUG_LINE], out);
    if (rela_size[RSEC_DEBUG_INFO] > 0)
        fwrite(rela_buf[RSEC_DEBUG_INFO], 1, rela_size[RSEC_DEBUG_INFO], out);

    /* Write named-section rela buffers (in the same order the offsets used). */
    for (j = 0; j < nextra; j++)
        if (nrela_size[j] > 0)
            fwrite(nrela_buf[j], 1, nrela_size[j], out);

    fwrite(shstrtab.data, 1, shstrtab.len, out);

    /* Section header table: the fixed headers, then the named ones. */
    for (k = 0; k < SH_COUNT; k++)
        fwrite(shdr[k], 1, 40, out);
    for (k = 0; k < nextra + n_named_rela; k++)
        fwrite(nshdr[k], 1, 40, out);

    free(sym_map);
    free(esyms);
    free(sec_sym);
    for (k = 0; k < RSEC_COUNT; k++)
        free(rela_buf[k]);
    for (j = 0; j < nextra; j++)
        free(nrela_buf[j]);
    free(nprog_name);
    free(nrela_name);
    free(nrela_sh);
    free(noff);
    free(nrela_off);
    free(nrela_buf);
    free(nrela_size);
    free(nshdr);
    free(strtab.data);
    free(shstrtab.data);
}
