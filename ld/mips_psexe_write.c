/* mips_psexe_write.c : PlayStation "PS-X EXE" executable writer.
 *
 * The PS-EXE is the format the PlayStation BIOS loader (and every emulator)
 * consumes: a 2048-byte header followed by a flat text+data image copied into
 * RAM at a fixed address.  It is not ELF, so this writer does not touch the
 * program/section header machinery; it takes the sections ld_layout placed and
 * mips_ld_link relocated, and serializes the loaded image plus the header the
 * loader reads.
 *
 * Header layout (all little-endian 32-bit unless noted):
 *   0x00  8  "PS-X EXE"
 *   0x10  4  pc0     initial PC (entry)
 *   0x14  4  gp0     initial $gp (0; this toolchain does not use $gp)
 *   0x18  4  t_addr  RAM address the image is copied to
 *   0x1C  4  t_size  image length, a multiple of 2048
 *   0x20  4  d_addr  (unused, 0)
 *   0x24  4  d_size  (unused, 0)
 *   0x28  4  b_addr  bss address
 *   0x2C  4  b_size  bss length (the loader zero-fills it)
 *   0x30  4  s_addr  stack base
 *   0x34  4  s_size  stack offset (added to s_addr)
 *   0x38..0x4B  saved sp/fp/gp/ra/s0 for the loader (0)
 * The rest of the 2048-byte header is zero.  The region/license marker some
 * tools place at 0x4C is not required by the EXE loader and is left out.
 */

#include "mips_ld.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PSX_HEADER_SIZE   0x800
#define PSX_ALIGN         0x800
/* Top of the 2 MB main RAM, less a small guard; the BIOS sets $sp here. */
#define PSX_STACK_BASE    0x801ffff0u

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
mips_ld_write_psexe(struct linker *ld, const char *path)
{
    struct ld_script *sc = &ld->script;
    uint32_t base = sc->nregions > 0 ? sc->regions[0].origin : 0x80010000;
    uint32_t load_end = base;           /* end of initialized (non-bss) image */
    uint32_t bss_addr = 0, bss_end = 0;
    int have_bss = 0;
    uint8_t *img;
    uint32_t img_size, t_size;
    FILE *f;

    /* the initialized image runs from base to the end of the last non-bss
       section; bss is tracked separately (the loader clears it in place) */
    for (int i = 0; i < sc->nsections; i++) {
        struct ld_output_sec *os = &sc->sections[i];
        if (os->discard || os->size == 0)
            continue;
        uint32_t end = os->vaddr + os->size;
        if (os->nobits) {
            if (!have_bss || os->vaddr < bss_addr)
                bss_addr = os->vaddr;
            if (!have_bss || end > bss_end)
                bss_end = end;
            have_bss = 1;
        } else {
            if (end > load_end)
                load_end = end;
        }
    }

    img_size = load_end - base;
    t_size = align_up(img_size, PSX_ALIGN);

    img = calloc(t_size ? t_size : PSX_ALIGN, 1);
    if (!img)
        die("out of memory");

    for (int i = 0; i < sc->nsections; i++) {
        struct ld_output_sec *os = &sc->sections[i];
        if (os->discard || os->size == 0 || os->nobits || !os->data)
            continue;
        memcpy(img + (os->vaddr - base), os->data, os->size);
    }

    f = fopen(path, "wb");
    if (!f)
        die("cannot create %s", path);

    uint8_t hdr[PSX_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "PS-X EXE", 8);
    put32(hdr + 0x10, ld->entry_vaddr);         /* pc0 */
    put32(hdr + 0x14, 0);                        /* gp0 */
    put32(hdr + 0x18, base);                     /* t_addr */
    put32(hdr + 0x1c, t_size);                   /* t_size */
    put32(hdr + 0x20, 0);                        /* d_addr */
    put32(hdr + 0x24, 0);                        /* d_size */
    put32(hdr + 0x28, have_bss ? bss_addr : 0);  /* b_addr */
    put32(hdr + 0x2c, have_bss ? bss_end - bss_addr : 0);   /* b_size */
    put32(hdr + 0x30, PSX_STACK_BASE);           /* s_addr */
    put32(hdr + 0x34, 0);                        /* s_size */

    fwrite(hdr, 1, PSX_HEADER_SIZE, f);
    fwrite(img, 1, t_size, f);

    fclose(f);
    free(img);
    chmod(path, 0755);
    return 0;
}
