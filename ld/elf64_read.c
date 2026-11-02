/* elf64_read.c : ELFCLASS64 little-endian relocatable object reader, shared by
 * the x86-64 and AArch64 linkers.
 *
 * The layout core keeps addresses in 32 bits (executables live in the low
 * 4 GiB), so a 64-bit value field that does not fit is a link error rather than
 * a silent truncation.  Only the relocation addend is kept full-width. */

#include "elf64_ld.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Little-endian read helpers
 ****************************************************************/

static uint16_t
get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* A 64-bit value that the 32-bit layout core must hold.  Executable addresses
 * live in the low 4 GiB, so anything larger is a malformed or unsupported
 * object, not something to truncate silently. */
static uint32_t
to32(uint64_t v, const char *path, const char *what)
{
    if (v > 0xffffffffULL)
        die("%s: %s 0x%llx does not fit the low 4 GiB", path, what,
            (unsigned long long)v);
    return (uint32_t)v;
}

/* Elf64_Shdr is 64 bytes. */
static const uint8_t *
shdr_at(const uint8_t *buf, uint64_t shoff, int idx)
{
    return buf + shoff + (uint64_t)idx * 64;
}

/****************************************************************
 * Reader
 ****************************************************************/

int
elf64_parse(struct arena *a, struct ld_object *obj, const uint8_t *buf,
            size_t fsize, const char *path, int machine)
{
    uint64_t shoff;
    int shnum, shstrndx;
    const uint8_t *shstrtab_hdr;
    const char *shstrtab;
    int i;
    int symtab_idx = -1;

    obj->path = path;

    if (fsize < 64)
        die("%s: too small for ELF64 header", path);
    if (memcmp(buf, "\177ELF", 4) != 0)
        die("%s: not an ELF file", path);
    if (buf[4] != ELFCLASS64)
        die("%s: not ELF64", path);
    if (buf[5] != ELFDATA2LSB)
        die("%s: not little-endian", path);
    if (get16(buf + 16) != ET_REL)
        die("%s: not a relocatable object", path);
    if (get16(buf + 18) != (uint16_t)machine)
        die("%s: wrong machine (want %d, got %d)", path, machine,
            get16(buf + 18));

    shoff = get64(buf + 40);
    shnum = get16(buf + 60);
    shstrndx = get16(buf + 62);

    if (shstrndx >= shnum)
        die("%s: bad shstrndx", path);

    shstrtab_hdr = shdr_at(buf, shoff, shstrndx);
    shstrtab = (const char *)buf + get64(shstrtab_hdr + 24);

    obj->nsections = shnum;
    obj->sections = arena_zalloc(a, (size_t)shnum * sizeof(struct ld_input_sec));
    obj->relocs = NULL;
    obj->nrelocs = 0;
    obj->reloc_cap = 0;
    obj->syms = NULL;
    obj->nsyms = 0;

    for (i = 0; i < shnum; i++) {
        const uint8_t *sh = shdr_at(buf, shoff, i);
        uint32_t sh_name = get32(sh + 0);
        uint32_t sh_type = get32(sh + 4);
        uint64_t sh_flags = get64(sh + 8);
        uint64_t sh_offset = get64(sh + 24);
        uint64_t sh_size = get64(sh + 32);
        uint64_t sh_addralign = get64(sh + 48);

        obj->sections[i].name = shstrtab + sh_name;
        obj->sections[i].type = (int)sh_type;
        obj->sections[i].flags = (uint32_t)sh_flags;
        obj->sections[i].size = to32(sh_size, path, "section size");
        obj->sections[i].align = sh_addralign ? (uint32_t)sh_addralign : 1;
        obj->sections[i].assigned_vaddr = 0;
        obj->sections[i].matched = 0;

        if (sh_type == SHT_PROGBITS || sh_type == SHT_NOBITS) {
            if (sh_type == SHT_PROGBITS && sh_size > 0)
                obj->sections[i].data = buf + sh_offset;
            else
                obj->sections[i].data = NULL;
        }

        if (sh_type == SHT_SYMTAB)
            symtab_idx = i;
    }

    if (symtab_idx < 0)
        die("%s: no symbol table", path);

    {
        const uint8_t *symsh = shdr_at(buf, shoff, symtab_idx);
        uint64_t sym_off = get64(symsh + 24);
        uint64_t sym_size = get64(symsh + 32);
        int link = (int)get32(symsh + 40);
        int count = (int)(sym_size / 24);     /* Elf64_Sym is 24 bytes */
        const uint8_t *strtab_sh = shdr_at(buf, shoff, link);
        const char *strtab = (const char *)buf + get64(strtab_sh + 24);

        obj->nsyms = count;
        obj->syms = arena_zalloc(a, (size_t)count * sizeof(struct ld_input_sym));

        for (i = 0; i < count; i++) {
            const uint8_t *s = buf + sym_off + (uint64_t)i * 24;
            uint32_t st_name = get32(s + 0);
            uint8_t st_info = s[4];
            uint16_t st_shndx = get16(s + 6);
            uint64_t st_value = get64(s + 8);
            uint64_t st_size = get64(s + 16);

            obj->syms[i].name = strtab + st_name;
            obj->syms[i].value = to32(st_value, path, "symbol value");
            obj->syms[i].size = to32(st_size, path, "symbol size");
            obj->syms[i].shndx = (int)st_shndx;
            obj->syms[i].binding = ELF64_ST_BIND(st_info);
            obj->syms[i].type = ELF64_ST_TYPE(st_info);
        }
    }

    for (i = 0; i < shnum; i++) {
        const uint8_t *sh = shdr_at(buf, shoff, i);
        uint32_t sh_type = get32(sh + 4);
        uint64_t rela_off, rela_size;
        int info_sec, count;

        if (sh_type != SHT_RELA)
            continue;

        rela_off = get64(sh + 24);
        rela_size = get64(sh + 32);
        info_sec = (int)get32(sh + 44);       /* sh_info */
        count = (int)(rela_size / 24);        /* Elf64_Rela is 24 bytes */

        int need = obj->nrelocs + count;
        if (need > obj->reloc_cap) {
            obj->reloc_cap = need * 2;
            obj->relocs = realloc(obj->relocs,
                                  (size_t)obj->reloc_cap *
                                  sizeof(struct ld_reloc));
            if (!obj->relocs)
                die("out of memory");
        }

        for (int j = 0; j < count; j++) {
            const uint8_t *r = buf + rela_off + (uint64_t)j * 24;
            struct ld_reloc *rel = &obj->relocs[obj->nrelocs++];
            uint64_t r_info = get64(r + 8);

            rel->offset = to32(get64(r + 0), path, "reloc offset");
            rel->sym_idx = (int)ELF64_R_SYM(r_info);
            rel->type = (int)ELF64_R_TYPE(r_info);
            rel->addend = (int64_t)get64(r + 16);
            rel->section = info_sec;
        }
    }

    return 0;
}

int
elf64_read_object(struct arena *a, struct ld_object *obj, const char *path,
                  int machine)
{
    if (mapfile_open(&obj->mf, path) != OK)
        die("cannot open %s", path);
    return elf64_parse(a, obj, obj->mf.data, obj->mf.len, path, machine);
}
