/* mips_elf_write.c : ELF32 little-endian MIPS executable writer
 *
 * The RISC-V writer with the machine id changed to EM_MIPS, plus one piece
 * MIPS needs and the others do not: a .MIPS.abiflags record described by a
 * PT_MIPS_ABIFLAGS program header.  qemu-mipsel reads that header to choose
 * the floating-point register mode (o32 double, FR=0); without it every
 * double-precision computation runs in the wrong mode and produces garbage.
 *
 * The default script declares two program headers so ld_layout reserves the
 * header space for them.  The writer emits the first as PT_MIPS_ABIFLAGS and
 * the second as the single loadable segment.  The abiflags record itself sits
 * after the loadable image, outside the load segment (qemu reads it from the
 * file by p_offset), and is also given a section header for the benefit of
 * ordinary ELF tools. */

#include "mips_ld.h"

#include <string.h>
#include <sys/stat.h>

#define PT_MIPS_ABIFLAGS   0x70000003
#define SHT_MIPS_ABIFLAGS  0x7000002a

/* Mips_elf_abiflags_v0, 24 bytes: version 0, isa_level 1 (MIPS-I), isa_rev 0,
 * gpr_size 1 (AFL_REG_32), cpr1_size 1 (AFL_REG_32), cpr2_size 0, fp_abi 1
 * (Val_GNU_MIPS_ABI_FP_DOUBLE), then isa_ext/ases/flags1/flags2 all zero.
 * Byte-identical to what mipsel-none-elf-ld writes for an o32 MIPS-I image. */
static const uint8_t abiflags_bytes[24] = {
    0x00, 0x00,                 /* version */
    0x01,                       /* isa_level: MIPS-I */
    0x00,                       /* isa_rev */
    0x01,                       /* gpr_size: 32 */
    0x01,                       /* cpr1_size: 32 */
    0x00,                       /* cpr2_size */
    0x01,                       /* fp_abi: DOUBLE */
    0x00, 0x00, 0x00, 0x00,     /* isa_ext */
    0x00, 0x00, 0x00, 0x00,     /* ases */
    0x00, 0x00, 0x00, 0x00,     /* flags1 */
    0x00, 0x00, 0x00, 0x00,     /* flags2 */
};

static void
put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void
put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t
align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

int
mips_ld_write_exec(struct linker *ld, const char *path)
{
    struct ld_script *sc = &ld->script;
    FILE *f;
    int nalloc = 0;

    for (int i = 0; i < sc->nsections; i++) {
        if (!sc->sections[i].discard && sc->sections[i].size > 0)
            nalloc++;
    }

    /* Two program headers: PT_MIPS_ABIFLAGS + PT_LOAD.  The default script
     * declares them so ld_layout reserved matching header space. */
    int nphdrs = sc->nphdrs >= 2 ? sc->nphdrs : 2;

    uint32_t ehdr_size = 52;
    uint32_t phdr_size = (uint32_t)nphdrs * 32;
    uint32_t headers_size = ehdr_size + phdr_size;
    uint32_t file_offset = headers_size;

    char shstrtab[512];
    int shstrtab_len = 0;
    shstrtab[shstrtab_len++] = '\0';

    /* section count: null + alloc sections + .MIPS.abiflags + .shstrtab */
    int shnum = 1 + nalloc + 1 + 1;

    struct {
        int sec_idx;
        uint32_t file_off;
        uint32_t shname_off;
    } alloc_map[MAX_OUT_SECS];
    int nmap = 0;

    for (int i = 0; i < sc->nsections; i++) {
        struct ld_output_sec *os = &sc->sections[i];
        if (os->discard || os->size == 0)
            continue;
        alloc_map[nmap].sec_idx = i;
        alloc_map[nmap].file_off = os->nobits ? 0 : file_offset;
        size_t nlen = strlen(os->name) + 1;
        alloc_map[nmap].shname_off = (uint32_t)shstrtab_len;
        memcpy(shstrtab + shstrtab_len, os->name, nlen);
        shstrtab_len += (int)nlen;
        if (!os->nobits)
            file_offset += os->size;
        nmap++;
    }

    /* the abiflags record follows the loadable data (outside the load
     * segment; qemu reads it from the file by offset) */
    uint32_t abiflags_off = align_up(file_offset, 8);
    uint32_t abiflags_name_off = (uint32_t)shstrtab_len;
    {
        const char *nm = ".MIPS.abiflags";
        size_t nlen = strlen(nm) + 1;
        memcpy(shstrtab + shstrtab_len, nm, nlen);
        shstrtab_len += (int)nlen;
    }
    file_offset = abiflags_off + sizeof(abiflags_bytes);

    uint32_t shstrtab_name_off = (uint32_t)shstrtab_len;
    {
        const char *nm = ".shstrtab";
        size_t nlen = strlen(nm) + 1;
        memcpy(shstrtab + shstrtab_len, nm, nlen);
        shstrtab_len += (int)nlen;
    }

    uint32_t shstrtab_file_off = file_offset;
    file_offset += (uint32_t)shstrtab_len;
    uint32_t shoff = file_offset;

    uint32_t load_vaddr = sc->nregions > 0 ?
        sc->regions[0].origin : 0x00400000;
    uint32_t load_end = 0, load_filesz = 0, load_memsz = 0;

    for (int i = 0; i < sc->nsections; i++) {
        struct ld_output_sec *os = &sc->sections[i];
        if (os->discard || os->size == 0)
            continue;
        uint32_t end = os->vaddr + os->size;
        if (end > load_end)
            load_end = end;
    }
    load_memsz = load_end - load_vaddr;

    for (int i = 0; i < nmap; i++) {
        struct ld_output_sec *os = &sc->sections[alloc_map[i].sec_idx];
        if (!os->nobits) {
            uint32_t end = alloc_map[i].file_off + os->size;
            if (end > load_filesz)
                load_filesz = end;
        }
    }

    f = fopen(path, "wb");
    if (!f)
        die("cannot create %s", path);

    uint8_t ehdr[52];
    memset(ehdr, 0, sizeof(ehdr));
    memcpy(ehdr, "\177ELF", 4);
    ehdr[4] = ELFCLASS32;
    ehdr[5] = ELFDATA2LSB;
    ehdr[6] = EV_CURRENT;
    put16(ehdr + 16, ET_EXEC);
    put16(ehdr + 18, EM_MIPS);
    put32(ehdr + 20, EV_CURRENT);
    put32(ehdr + 24, ld->entry_vaddr);
    put32(ehdr + 28, ehdr_size);
    put32(ehdr + 32, shoff);
    put32(ehdr + 36, 0);            /* e_flags: o32, no ABI/ISA marker */
    put16(ehdr + 40, 52);
    put16(ehdr + 42, 32);
    put16(ehdr + 44, (uint16_t)nphdrs);
    put16(ehdr + 46, 40);
    put16(ehdr + 48, (uint16_t)shnum);
    put16(ehdr + 50, (uint16_t)(shnum - 1));
    fwrite(ehdr, 1, 52, f);

    /* phdr 0: PT_MIPS_ABIFLAGS */
    {
        uint8_t phdr[32];
        memset(phdr, 0, sizeof(phdr));
        put32(phdr + 0, PT_MIPS_ABIFLAGS);
        put32(phdr + 4, abiflags_off);
        put32(phdr + 8, 0);         /* not mapped; qemu reads by file offset */
        put32(phdr + 12, 0);
        put32(phdr + 16, sizeof(abiflags_bytes));
        put32(phdr + 20, sizeof(abiflags_bytes));
        put32(phdr + 24, PF_R);
        put32(phdr + 28, 8);
        fwrite(phdr, 1, 32, f);
    }

    /* phdr 1: the loadable image */
    {
        uint8_t phdr[32];
        memset(phdr, 0, sizeof(phdr));
        put32(phdr + 0, PT_LOAD);
        put32(phdr + 4, 0);
        put32(phdr + 8, load_vaddr);
        put32(phdr + 12, load_vaddr);
        put32(phdr + 16, load_filesz);
        put32(phdr + 20, load_memsz);
        put32(phdr + 24, (uint32_t)(PF_R | PF_W | PF_X));
        put32(phdr + 28, 0x1000);
        fwrite(phdr, 1, 32, f);
    }

    /* any further reserved phdr slots (a custom script with >2): pad */
    for (int i = 2; i < nphdrs; i++) {
        uint8_t phdr[32];
        memset(phdr, 0, sizeof(phdr));
        fwrite(phdr, 1, 32, f);
    }

    for (int i = 0; i < nmap; i++) {
        struct ld_output_sec *os = &sc->sections[alloc_map[i].sec_idx];
        if (os->nobits)
            continue;
        fwrite(os->data, 1, os->size, f);
    }

    /* pad to the aligned abiflags offset, then the record */
    {
        uint32_t here = headers_size;
        for (int i = 0; i < nmap; i++) {
            struct ld_output_sec *os = &sc->sections[alloc_map[i].sec_idx];
            if (!os->nobits)
                here = alloc_map[i].file_off + os->size;
        }
        while (here < abiflags_off) {
            fputc(0, f);
            here++;
        }
    }
    fwrite(abiflags_bytes, 1, sizeof(abiflags_bytes), f);

    fwrite(shstrtab, 1, (size_t)shstrtab_len, f);

    {
        uint8_t null_sh[40];
        memset(null_sh, 0, sizeof(null_sh));
        fwrite(null_sh, 1, 40, f);
    }

    for (int i = 0; i < nmap; i++) {
        struct ld_output_sec *os = &sc->sections[alloc_map[i].sec_idx];
        uint8_t sh[40];
        uint32_t sflags = SHF_ALLOC;
        memset(sh, 0, sizeof(sh));
        put32(sh + 0, alloc_map[i].shname_off);
        put32(sh + 4, os->nobits ? SHT_NOBITS : SHT_PROGBITS);
        if (strstr(os->name, ".text"))
            sflags |= SHF_EXECINSTR;
        if (strstr(os->name, ".data") || strstr(os->name, ".bss"))
            sflags |= SHF_WRITE;
        put32(sh + 8, sflags);
        put32(sh + 12, os->vaddr);
        put32(sh + 16, os->nobits ? 0 : alloc_map[i].file_off);
        put32(sh + 20, os->size);
        put32(sh + 32, 4);
        fwrite(sh, 1, 40, f);
    }

    /* .MIPS.abiflags section header (not allocated; the phdr carries it) */
    {
        uint8_t sh[40];
        memset(sh, 0, sizeof(sh));
        put32(sh + 0, abiflags_name_off);
        put32(sh + 4, SHT_MIPS_ABIFLAGS);
        put32(sh + 16, abiflags_off);
        put32(sh + 20, sizeof(abiflags_bytes));
        put32(sh + 32, 8);
        put32(sh + 36, sizeof(abiflags_bytes));    /* sh_entsize */
        fwrite(sh, 1, 40, f);
    }

    {
        uint8_t sh[40];
        memset(sh, 0, sizeof(sh));
        put32(sh + 0, shstrtab_name_off);
        put32(sh + 4, SHT_STRTAB);
        put32(sh + 16, shstrtab_file_off);
        put32(sh + 20, (uint32_t)shstrtab_len);
        put32(sh + 32, 1);
        fwrite(sh, 1, 40, f);
    }

    fclose(f);
    chmod(path, 0755);
    return 0;
}
