/* tex.c - libpsx texture upload.
 *
 * Takes a TIM image (the PlayStation's standard texture format, produced by
 * libpsx/tools/timconv) and copies it into VRAM: the palette, if the image is
 * paletted, and the pixels. After that the GPU draws from it, and psx_draw_sprite
 * (gpu.c) references it through the psx_texinfo this fills in.
 *
 * The transfer is a CPU-driven VRAM load (GP0 0xA0), not DMA: textures upload
 * once at load time, so the simple polled path is clear and fast enough, and it
 * does not contend with the per-frame primitive DMA. Each GP0 word is gated on
 * the "ready to receive command" bit of GPUSTAT.
 *
 * All TIM fields are read little-endian through explicit byte assembly, so the
 * same image bytes are valid whatever the host byte order that produced them.
 *
 * VRAM is 1024x512 16-bit pixels. The two 320x240 framebuffers occupy x 0..319,
 * y 0..479 (see gpu.c), so textures and palettes live at x >= 320, where
 * timconv places them by default.
 */

#include <stddef.h>

#include "psx.h"

#define GP0        (*(volatile uint32_t *)0xBF801810u)  /* command/data (write) */
#define GPUSTAT    (*(volatile uint32_t *)0xBF801814u)  /* status (read) */
#define GPU_RDY_CMD 0x04000000u                         /* ready to receive cmd */

/* TIM pixel modes (the low bits of the flags word). */
#define TIM_PMODE_4   0
#define TIM_PMODE_8   1
#define TIM_PMODE_16  2
#define TIM_FLAG_CLUT 0x08

static uint32_t
rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t
rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void
gpu_wait(void)
{
    while (!(GPUSTAT & GPU_RDY_CMD))
        ;
}

/* Copy a rectangle of 16-bit VRAM words from a little-endian byte stream to
 * VRAM (x,y), sized w halfwords by h rows. Two halfwords pack into each GP0
 * word. */
static void
vram_load(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *data)
{
    gpu_wait(); GP0 = 0xA0000000u;                      /* copy CPU -> VRAM */
    gpu_wait(); GP0 = ((uint32_t)y << 16) | x;
    gpu_wait(); GP0 = ((uint32_t)h << 16) | w;

    uint32_t n = (uint32_t)w * h;                       /* 16-bit words to send */
    for (uint32_t i = 0; i < n; i += 2) {
        uint32_t lo = rd_u16(data + (size_t)i * 2);
        uint32_t hi = (i + 1 < n) ? rd_u16(data + (size_t)(i + 1) * 2) : 0;
        gpu_wait();
        GP0 = lo | (hi << 16);
    }
}

void
psx_vram_upload(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *data)
{
    vram_load(x, y, w, h, data);
}

int
psx_tim_upload(const void *tim, psx_texinfo *out)
{
    const uint8_t *p = tim;
    if (rd_u32(p) != 0x10)                              /* TIM magic */
        return -1;
    uint32_t flags = rd_u32(p + 4);
    int pmode = (int)(flags & 0x07);
    p += 8;

    uint16_t clut_id = 0;
    if (flags & TIM_FLAG_CLUT) {
        uint32_t bnum = rd_u32(p);
        uint16_t cx = rd_u16(p + 4);
        uint16_t cy = rd_u16(p + 6);
        uint16_t cw = rd_u16(p + 8);
        uint16_t ch = rd_u16(p + 10);
        vram_load(cx, cy, cw, ch, p + 12);
        /* CLUT id: X in 16-pixel units (bits 0..5), Y (bits 6..14). */
        clut_id = (uint16_t)(((cy & 0x1FFu) << 6) | ((cx >> 4) & 0x3Fu));
        p += bnum;
    } else if (pmode != TIM_PMODE_16) {
        return -2;                                      /* paletted but no CLUT */
    }

    uint32_t ibnum = rd_u32(p);
    uint16_t ix = rd_u16(p + 4);
    uint16_t iy = rd_u16(p + 6);
    uint16_t iw = rd_u16(p + 8);                        /* width in halfwords */
    uint16_t ih = rd_u16(p + 10);
    (void)ibnum;
    vram_load(ix, iy, iw, ih, p + 12);

    /* Texels per 16-bit VRAM word, and the GPU texture-color code. */
    int texels_per_word, texcolor;
    if (pmode == TIM_PMODE_8) {
        texels_per_word = 2;
        texcolor = 1;
    } else if (pmode == TIM_PMODE_16) {
        texels_per_word = 1;
        texcolor = 2;
    } else {
        return -3;                                      /* 4-bit not handled yet */
    }

    /* Texture page: X base in 64-halfword units, Y base 0 or 256. The texel
     * offset within the page is whatever the image's VRAM position is past that
     * base; timconv places images on a page boundary, so u and v are normally 0. */
    int tpx = ix / 64;
    int tpy = (iy >= 256) ? 1 : 0;
    uint16_t tpage = (uint16_t)((tpx & 0xF) | ((tpy & 1) << 4) | (texcolor << 7));

    int base_texel_x = tpx * 64 * texels_per_word;
    int base_texel_y = tpy * 256;

    out->tpage = tpage;
    out->clut = clut_id;
    out->u = (uint8_t)((int)ix * texels_per_word - base_texel_x);
    out->v = (uint8_t)((int)iy - base_texel_y);
    out->w = (uint16_t)(iw * texels_per_word);
    out->h = ih;
    return 0;
}

/* The handle table: psx_tex_upload fills the next free slot and returns its
 * index, and psx_tex_at resolves a handle back to the stored info. This keeps
 * the raw TIM pointer on the C side, so the handle draws are callable from a
 * language with no pointer type. */
static psx_texinfo tex_table[PSX_MAX_TEX];
static int tex_count;

int
psx_tex_upload(const void *tim)
{
    if (tex_count >= PSX_MAX_TEX)
        return -1;
    int rc = psx_tim_upload(tim, &tex_table[tex_count]);
    if (rc != 0)
        return rc;
    return tex_count++;
}

const psx_texinfo *
psx_tex_at(int handle)
{
    if (handle < 0 || handle >= tex_count)
        return 0;
    return &tex_table[handle];
}
