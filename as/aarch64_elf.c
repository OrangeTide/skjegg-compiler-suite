/* aarch64_elf.c : ELF64 little-endian relocatable object writer (EM_AARCH64).
 *
 * Structurally identical to as/x86_64_elf.c (ELFCLASS64, 24-byte Elf64_Sym,
 * SHT_RELA with 24-byte Elf64_Rela, the same local-then-global symbol ordering
 * and section-symbol folding of local defined relocations); only the machine id
 * and the section enum differ.  The bounded input has no .loc, so there are no
 * DWARF sections; an empty .note.GNU-stack marks the stack non-executable.
 */

#include "aarch64.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define ELFMAG        "\177ELF"
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1
#define ELFOSABI_NONE 0

#define ET_REL       1

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

#define ELF64_ST_INFO(b, t) (((b) << 4) | ((t) & 0xF))

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
sec_addralign(const struct section *s)
{
    return s->align > 1 ? (uint64_t)s->align : 1;
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
 * ELF symbol builder (Elf64_Sym, 24 bytes)
 ****************************************************************/

struct elf_sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

static void
write_sym(uint8_t *buf, struct elf_sym *s)
{
    put32(buf + 0,  s->st_name);
    buf[4] = s->st_info;
    buf[5] = s->st_other;
    put16(buf + 6,  s->st_shndx);
    put64(buf + 8,  s->st_value);
    put64(buf + 16, s->st_size);
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
    SH_NOTE_GNU_STACK,
    SH_SHSTRTAB,
    SH_COUNT,
};

/* Map an assembler section id to its ELF section header index. */
static int
xsec_shndx(int xsec)
{
    switch (xsec) {
    case ASEC_TEXT:   return SH_TEXT;
    case ASEC_RODATA: return SH_RODATA;
    case ASEC_DATA:   return SH_DATA;
    case ASEC_BSS:    return SH_BSS;
    default:          return SHN_UNDEF;
    }
}

/****************************************************************
 * ELF writer
 ****************************************************************/

void
a64_elf_write(struct a64_asm *a, FILE *out)
{
    struct strtab shstrtab;
    struct strtab strtab;
    int shname[SH_COUNT];
    uint8_t ehdr[64];
    uint8_t shdr[SH_COUNT][64];
    uint64_t offset;
    int nsyms_local, nsyms_total;
    int k;
    int sec_sym[ASEC_COUNT];
    int *sym_map;
    struct elf_sym *esyms;
    int nesyms = 0, esym_cap;
    int len[ASEC_COUNT];
    int nrel[ASEC_COUNT];

    for (k = 0; k < ASEC_COUNT; k++) {
        len[k] = a->sections[k].len;
        nrel[k] = a->sections[k].nrelocs;
    }

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
    shname[SH_NOTE_GNU_STACK] = strtab_add(&shstrtab, ".note.GNU-stack");
    shname[SH_SHSTRTAB] = strtab_add(&shstrtab, ".shstrtab");

    /* Symbol table: null, section syms, local defined, global/undefined. */
    strtab_init(&strtab);
    sym_map = xmalloc((a->st.nsyms ? a->st.nsyms : 1) * sizeof(int));
    esym_cap = 1 + ASEC_COUNT + a->st.nsyms;
    esyms = xmalloc(esym_cap * sizeof(struct elf_sym));

    memset(&esyms[nesyms], 0, sizeof(struct elf_sym));
    nesyms++;

    for (k = 0; k < ASEC_COUNT; k++) {
        /* The __DWARF sections have no ELF home (xsec_shndx returns UNDEF) and
           are never populated on the ELF path; carry no section symbol for them
           so ELF output stays identical to the pre-DWARF layout. */
        if (xsec_shndx(k) == SHN_UNDEF) {
            sec_sym[k] = 0;
            continue;
        }
        esyms[nesyms] = (struct elf_sym){
            .st_name = 0, .st_value = 0, .st_size = 0,
            .st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION),
            .st_other = 0, .st_shndx = (uint16_t)xsec_shndx(k),
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
            .st_info = ELF64_ST_INFO(STB_LOCAL,
                a->st.syms[k].is_func ? STT_FUNC : STT_NOTYPE),
            .st_other = 0,
            .st_shndx = (uint16_t)xsec_shndx(a->st.syms[k].section),
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
            shndx = (uint16_t)xsec_shndx(a->st.syms[k].section);
        esyms[nesyms] = (struct elf_sym){
            .st_name = (uint32_t)strtab_add(&strtab, a->st.syms[k].name),
            .st_value = a->st.syms[k].defined ? a->st.syms[k].value : 0,
            .st_size = a->st.syms[k].size,
            .st_info = ELF64_ST_INFO(
                a->st.syms[k].weak ? STB_WEAK : STB_GLOBAL,
                a->st.syms[k].is_func ? STT_FUNC : STT_NOTYPE),
            .st_other = 0,
            .st_shndx = shndx,
        };
        sym_map[k] = nesyms++;
    }

    nsyms_total = nesyms;

    /* Build RELA buffers for text, rodata and data (24-byte Elf64_Rela). */
    uint8_t *rela_buf[ASEC_COUNT];
    int rela_size[ASEC_COUNT];
    for (k = 0; k < ASEC_COUNT; k++) {
        rela_size[k] = nrel[k] * 24;
        rela_buf[k] = nrel[k] ? xmalloc(rela_size[k]) : NULL;
    }

    for (k = 0; k < ASEC_COUNT; k++) {
        int j;
        for (j = 0; j < nrel[k]; j++) {
            struct reloc *r = &a->sections[k].relocs[j];
            uint8_t *p = rela_buf[k] + j * 24;
            int elf_sym_idx = sym_map[r->sym_idx];
            int64_t addend = r->addend;

            if (a->st.syms[r->sym_idx].defined
                && !a->st.syms[r->sym_idx].global) {
                addend += (int64_t)a->st.syms[r->sym_idx].value;
                elf_sym_idx = sec_sym[a->st.syms[r->sym_idx].section];
            }
            put64(p + 0, r->offset);
            put64(p + 8, ((uint64_t)elf_sym_idx << 32)
                         | (uint64_t)(uint32_t)r->type);
            put64(p + 16, (uint64_t)addend);
        }
    }

    /* File layout: ehdr, .text, .rodata, .data, .symtab, .strtab,
       .rela.text, .rela.rodata, .rela.data, .shstrtab, section headers. */
    int symtab_size = nsyms_total * 24;
    uint64_t sec_off[ASEC_COUNT];
    uint64_t symtab_off, strtab_off, rela_off[ASEC_COUNT], shstrtab_off;
    uint64_t shdr_off;

    offset = 64;
    sec_off[ASEC_TEXT] = offset;   offset += (uint64_t)len[ASEC_TEXT];
    sec_off[ASEC_RODATA] = offset; offset += (uint64_t)len[ASEC_RODATA];
    sec_off[ASEC_DATA] = offset;   offset += (uint64_t)len[ASEC_DATA];
    sec_off[ASEC_BSS] = offset;    /* NOBITS: no file bytes */

    symtab_off = offset;           offset += (uint64_t)symtab_size;
    strtab_off = offset;           offset += (uint64_t)strtab.len;

    rela_off[ASEC_TEXT] = offset;   offset += (uint64_t)rela_size[ASEC_TEXT];
    rela_off[ASEC_RODATA] = offset; offset += (uint64_t)rela_size[ASEC_RODATA];
    rela_off[ASEC_DATA] = offset;   offset += (uint64_t)rela_size[ASEC_DATA];

    shstrtab_off = offset;         offset += (uint64_t)shstrtab.len;
    shdr_off = offset;

    /* ELF header */
    memset(ehdr, 0, sizeof(ehdr));
    memcpy(ehdr, ELFMAG, 4);
    ehdr[4] = ELFCLASS64;
    ehdr[5] = ELFDATA2LSB;
    ehdr[6] = EV_CURRENT;
    ehdr[7] = ELFOSABI_NONE;
    put16(ehdr + 16, ET_REL);
    put16(ehdr + 18, EM_AARCH64);
    put32(ehdr + 20, EV_CURRENT);
    put64(ehdr + 40, shdr_off);    /* e_shoff */
    put32(ehdr + 48, 0);           /* e_flags */
    put16(ehdr + 52, 64);          /* e_ehsize */
    put16(ehdr + 58, 64);          /* e_shentsize */
    put16(ehdr + 60, SH_COUNT);    /* e_shnum */
    put16(ehdr + 62, SH_SHSTRTAB); /* e_shstrndx */

    memset(shdr, 0, sizeof(shdr));

    /* SH_TEXT */
    put32(shdr[SH_TEXT] + 0, (uint32_t)shname[SH_TEXT]);
    put32(shdr[SH_TEXT] + 4, SHT_PROGBITS);
    put64(shdr[SH_TEXT] + 8, SHF_ALLOC | SHF_EXECINSTR);
    put64(shdr[SH_TEXT] + 24, sec_off[ASEC_TEXT]);
    put64(shdr[SH_TEXT] + 32, (uint64_t)len[ASEC_TEXT]);
    put64(shdr[SH_TEXT] + 48, sec_addralign(&a->sections[ASEC_TEXT]));

    /* SH_RODATA */
    put32(shdr[SH_RODATA] + 0, (uint32_t)shname[SH_RODATA]);
    put32(shdr[SH_RODATA] + 4, SHT_PROGBITS);
    put64(shdr[SH_RODATA] + 8, SHF_ALLOC);
    put64(shdr[SH_RODATA] + 24, sec_off[ASEC_RODATA]);
    put64(shdr[SH_RODATA] + 32, (uint64_t)len[ASEC_RODATA]);
    put64(shdr[SH_RODATA] + 48, sec_addralign(&a->sections[ASEC_RODATA]));

    /* SH_DATA */
    put32(shdr[SH_DATA] + 0, (uint32_t)shname[SH_DATA]);
    put32(shdr[SH_DATA] + 4, SHT_PROGBITS);
    put64(shdr[SH_DATA] + 8, SHF_WRITE | SHF_ALLOC);
    put64(shdr[SH_DATA] + 24, sec_off[ASEC_DATA]);
    put64(shdr[SH_DATA] + 32, (uint64_t)len[ASEC_DATA]);
    put64(shdr[SH_DATA] + 48, sec_addralign(&a->sections[ASEC_DATA]));

    /* SH_BSS */
    put32(shdr[SH_BSS] + 0, (uint32_t)shname[SH_BSS]);
    put32(shdr[SH_BSS] + 4, SHT_NOBITS);
    put64(shdr[SH_BSS] + 8, SHF_WRITE | SHF_ALLOC);
    put64(shdr[SH_BSS] + 24, sec_off[ASEC_BSS]);
    put64(shdr[SH_BSS] + 32, (uint64_t)len[ASEC_BSS]);
    put64(shdr[SH_BSS] + 48, sec_addralign(&a->sections[ASEC_BSS]));

    /* SH_SYMTAB */
    put32(shdr[SH_SYMTAB] + 0, (uint32_t)shname[SH_SYMTAB]);
    put32(shdr[SH_SYMTAB] + 4, SHT_SYMTAB);
    put64(shdr[SH_SYMTAB] + 24, symtab_off);
    put64(shdr[SH_SYMTAB] + 32, (uint64_t)symtab_size);
    put32(shdr[SH_SYMTAB] + 40, SH_STRTAB);
    put32(shdr[SH_SYMTAB] + 44, (uint32_t)nsyms_local);
    put64(shdr[SH_SYMTAB] + 48, 8);
    put64(shdr[SH_SYMTAB] + 56, 24);

    /* SH_STRTAB */
    put32(shdr[SH_STRTAB] + 0, (uint32_t)shname[SH_STRTAB]);
    put32(shdr[SH_STRTAB] + 4, SHT_STRTAB);
    put64(shdr[SH_STRTAB] + 24, strtab_off);
    put64(shdr[SH_STRTAB] + 32, (uint64_t)strtab.len);
    put64(shdr[SH_STRTAB] + 48, 1);

    /* SH_RELA_TEXT / RODATA / DATA */
    {
        struct { int sh; int xsec; int target_sh; } rela[3] = {
            { SH_RELA_TEXT,   ASEC_TEXT,   SH_TEXT },
            { SH_RELA_RODATA, ASEC_RODATA, SH_RODATA },
            { SH_RELA_DATA,   ASEC_DATA,   SH_DATA },
        };
        int i;
        for (i = 0; i < 3; i++) {
            int sh = rela[i].sh;
            put32(shdr[sh] + 0, (uint32_t)shname[sh]);
            put32(shdr[sh] + 4, SHT_RELA);
            put64(shdr[sh] + 24, rela_off[rela[i].xsec]);
            put64(shdr[sh] + 32, (uint64_t)rela_size[rela[i].xsec]);
            put32(shdr[sh] + 40, SH_SYMTAB);
            put32(shdr[sh] + 44, (uint32_t)rela[i].target_sh);
            put64(shdr[sh] + 48, 8);
            put64(shdr[sh] + 56, 24);
        }
    }

    /* SH_NOTE_GNU_STACK: empty, no SHF_EXECINSTR -> non-executable stack. */
    put32(shdr[SH_NOTE_GNU_STACK] + 0, (uint32_t)shname[SH_NOTE_GNU_STACK]);
    put32(shdr[SH_NOTE_GNU_STACK] + 4, SHT_PROGBITS);
    put64(shdr[SH_NOTE_GNU_STACK] + 24, shstrtab_off); /* zero-length, offset unused */
    put64(shdr[SH_NOTE_GNU_STACK] + 32, 0);
    put64(shdr[SH_NOTE_GNU_STACK] + 48, 1);

    /* SH_SHSTRTAB */
    put32(shdr[SH_SHSTRTAB] + 0, (uint32_t)shname[SH_SHSTRTAB]);
    put32(shdr[SH_SHSTRTAB] + 4, SHT_STRTAB);
    put64(shdr[SH_SHSTRTAB] + 24, shstrtab_off);
    put64(shdr[SH_SHSTRTAB] + 32, (uint64_t)shstrtab.len);
    put64(shdr[SH_SHSTRTAB] + 48, 1);

    /* Emit the file */
    fwrite(ehdr, 1, 64, out);
    if (len[ASEC_TEXT] > 0)
        fwrite(a->sections[ASEC_TEXT].data, 1, len[ASEC_TEXT], out);
    if (len[ASEC_RODATA] > 0)
        fwrite(a->sections[ASEC_RODATA].data, 1, len[ASEC_RODATA], out);
    if (len[ASEC_DATA] > 0)
        fwrite(a->sections[ASEC_DATA].data, 1, len[ASEC_DATA], out);

    for (k = 0; k < nsyms_total; k++) {
        uint8_t sbuf[24];
        write_sym(sbuf, &esyms[k]);
        fwrite(sbuf, 1, 24, out);
    }

    fwrite(strtab.data, 1, strtab.len, out);

    if (rela_size[ASEC_TEXT] > 0)
        fwrite(rela_buf[ASEC_TEXT], 1, rela_size[ASEC_TEXT], out);
    if (rela_size[ASEC_RODATA] > 0)
        fwrite(rela_buf[ASEC_RODATA], 1, rela_size[ASEC_RODATA], out);
    if (rela_size[ASEC_DATA] > 0)
        fwrite(rela_buf[ASEC_DATA], 1, rela_size[ASEC_DATA], out);

    fwrite(shstrtab.data, 1, shstrtab.len, out);

    for (k = 0; k < SH_COUNT; k++)
        fwrite(shdr[k], 1, 64, out);

    free(sym_map);
    free(esyms);
    for (k = 0; k < ASEC_COUNT; k++)
        free(rela_buf[k]);
    free(strtab.data);
    free(shstrtab.data);
}
