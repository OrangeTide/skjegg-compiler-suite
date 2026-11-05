/* gpu.c - libpsx GPU driver (milestone 1).
 *
 * Drives the PlayStation GPU to put flat primitives on a double-buffered
 * 320x240 NTSC display. Rendering commands are not written to the GP0 port one
 * at a time; they are built into a linked list in RAM and shipped to the GPU
 * by DMA (dma.c). This module owns the list building, the drawing environment,
 * and the buffer swap. Display-control commands (reset, mode, and the buffer
 * flip) still go straight to the GP1 port, which is immediate rather than
 * FIFO-queued.
 *
 * The linked list is the DMA "ordering table" idea in its simplest form: a
 * forward chain built in submission order. Each node is a header word followed
 * by its GP0 payload words; the header's low 24 bits point to the next node
 * and its top byte holds the payload length. The GPU DMA walks the chain until
 * a link reads 0xFFFFFF. A z-sorted table cleared by the OTC DMA channel is a
 * later addition for when depth ordering is needed; flat 2D drawn in order does
 * not need it.
 *
 * The R3000 has no transparent data cache (its data cache is the scratchpad),
 * so a list the CPU writes to RAM is seen by the DMA without any flush.
 *
 * Register access is through KSEG1 (0xBF80xxxx), the uncached window, which is
 * mandatory for I/O.
 *
 * Display layout. VRAM is 1024x512 16-bit pixels. The two 320x240 buffers are
 * stacked vertically: buffer 0 at VRAM y=0, buffer 1 at y=240. One is shown
 * while the other is drawn, and psx_display_swap exchanges them.
 */

#include "psx.h"

#define GP1 (*(volatile uint32_t *)0xBF801814u)   /* display control (write) */

/* The primitive chain, in bss. 4096 words is far more than a flat 2D frame
 * needs and keeps the whole thing well inside the 2 MB budget. */
#define CHAIN_WORDS 4096
#define LINK_END    0xFFFFFFu

static uint32_t chain[CHAIN_WORDS];
static uint32_t chain_pos;      /* next free word */
static int chain_tail;          /* index of the last node's header, -1 if none */
static uint32_t chain_head;     /* absolute address of the first node's header */

static int shown;               /* displayed buffer index (0 or 1) */
static uint8_t bg_r, bg_g, bg_b;

/* Pack a 24-bit color into a GP0 command word: red low, then green, blue, with
 * the command byte on top. */
static uint32_t
rgb(uint32_t cmd, uint8_t r, uint8_t g, uint8_t b)
{
    return (cmd << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

/* Reserve a node of n payload words and link it onto the end of the chain.
 * Returns the header index, or -1 when the chain is full (the primitive is
 * then dropped, never a buffer overrun). */
static int
node(int n)
{
    if (chain_pos + (uint32_t)n + 1u > CHAIN_WORDS)
        return -1;

    uint32_t h = chain_pos;
    chain[h] = ((uint32_t)n << 24) | LINK_END;      /* new node ends the chain */
    uint32_t addr = (uint32_t)(uintptr_t)&chain[h];
    if (chain_tail >= 0)
        chain[chain_tail] = (chain[chain_tail] & 0xFF000000u) | (addr & LINK_END);
    else
        chain_head = addr;
    chain_tail = (int)h;
    chain_pos += (uint32_t)n + 1u;
    return (int)h;
}

/* A one-word GP0 command (the drawing-environment setup words). */
static void
cmd(uint32_t word)
{
    int h = node(1);
    if (h >= 0)
        chain[h + 1] = word;
}

/* Point the display controller at buffer i. GP1(05h) is immediate. */
static void
set_display(int i)
{
    GP1 = 0x05000000u | (((uint32_t)i * PSX_SCREEN_H) << 10);
}

/* Start a frame: reset the chain, then queue the drawing environment for the
 * back buffer and the background clear, so draw calls that follow render into
 * the back buffer in screen coordinates. */
static void
frame_begin(int back)
{
    uint32_t y = (uint32_t)back * PSX_SCREEN_H;

    chain_pos = 0;
    chain_tail = -1;
    chain_head = 0;

    cmd(0xE1000400u);                               /* draw mode */
    cmd(0xE3000000u | (y << 10));                   /* draw area top-left */
    cmd(0xE4000000u | (uint32_t)(PSX_SCREEN_W - 1)
        | ((y + PSX_SCREEN_H - 1) << 10));          /* draw area bottom-right */
    cmd(0xE5000000u | (y << 11));                   /* draw offset = buffer origin */

    int h = node(3);                                /* GP0(02h): fill the buffer */
    if (h >= 0) {
        chain[h + 1] = rgb(0x02u, bg_r, bg_g, bg_b);
        chain[h + 2] = y << 16;                     /* top-left x=0, y */
        chain[h + 3] = ((uint32_t)PSX_SCREEN_H << 16) | PSX_SCREEN_W;
    }
}

void
psx_gpu_init(void)
{
    psx_dma_init();

    GP1 = 0x00000000u;      /* reset the GPU */
    GP1 = 0x03000000u;      /* display enable */
    GP1 = 0x08000001u;      /* 320x240, NTSC, 15-bit, non-interlace */
    GP1 = 0x06000000u | 0x260u | (0xC60u << 12);    /* horizontal range */
    GP1 = 0x07000000u | 0x010u | (0x100u << 10);    /* vertical range */
    GP1 = 0x04000002u;      /* DMA direction: CPU -> GP0 */

    shown = 0;
    bg_r = bg_g = bg_b = 0;
    set_display(0);
    frame_begin(1);
}

void
psx_set_bg(uint8_t r, uint8_t g, uint8_t b)
{
    bg_r = r;
    bg_g = g;
    bg_b = b;
}

void
psx_draw_rect(int x, int y, int w, int h,
              uint8_t r, uint8_t g, uint8_t b)
{
    int p = node(3);                                /* GP0(60h): flat rectangle */
    if (p < 0)
        return;
    chain[p + 1] = rgb(0x60u, r, g, b);
    chain[p + 2] = ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF);
    chain[p + 3] = ((uint32_t)(h & 0xFFFF) << 16) | (uint32_t)(w & 0xFFFF);
}

void
psx_draw_sprite(const psx_texinfo *t, int x, int y)
{
    /* Select the sprite's texture page in the draw-mode register. Rectangle
     * primitives take their texture page from this register, not the packet,
     * so it is set just before the rect. The 0x400 bit (draw to display area
     * allowed) matches the frame's default draw mode. */
    cmd(0xE1000400u | t->tpage);

    int p = node(4);                                /* GP0(65h): textured rect, raw */
    if (p < 0)
        return;
    chain[p + 1] = (0x65u << 24) | 0x808080u;       /* raw texture: color ignored */
    chain[p + 2] = ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF);
    chain[p + 3] = ((uint32_t)t->clut << 16)
                 | ((uint32_t)t->v << 8) | (uint32_t)t->u;
    chain[p + 4] = ((uint32_t)t->h << 16) | (uint32_t)t->w;
}

void
psx_draw_rect_alpha(int x, int y, int w, int h,
                    uint8_t r, uint8_t g, uint8_t b, int mode)
{
    /* The blend equation lives in the draw-mode register's semi-transparency
     * bits (5-6); the primitive's semi-transparent bit then selects it. */
    cmd(0xE1000400u | ((uint32_t)(mode & 3) << 5));

    int p = node(3);                                /* GP0(62h): semi-transparent rect */
    if (p < 0)
        return;
    chain[p + 1] = rgb(0x62u, r, g, b);
    chain[p + 2] = ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF);
    chain[p + 3] = ((uint32_t)(h & 0xFFFF) << 16) | (uint32_t)(w & 0xFFFF);
}

void
psx_draw_sprite_uv(const psx_texinfo *t, int x, int y,
                   int u, int v, int w, int h)
{
    cmd(0xE1000400u | t->tpage);

    int p = node(4);                                /* GP0(65h): textured rect, raw */
    if (p < 0)
        return;
    chain[p + 1] = (0x65u << 24) | 0x808080u;
    chain[p + 2] = ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF);
    chain[p + 3] = ((uint32_t)t->clut << 16)
                 | ((uint32_t)((t->v + v) & 0xFF) << 8) | (uint32_t)((t->u + u) & 0xFF);
    chain[p + 4] = ((uint32_t)(h & 0xFFFF) << 16) | (uint32_t)(w & 0xFFFF);
}

/* Pack a signed screen coordinate pair into a GPU vertex word. */
static uint32_t
vertex(int x, int y)
{
    return ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF);
}

void
psx_draw_poly4(int x0, int y0, int x1, int y1, int x2, int y2,
               int x3, int y3, uint8_t r, uint8_t g, uint8_t b)
{
    int p = node(5);                                /* GP0(28h): flat 4-point poly */
    if (p < 0)
        return;
    chain[p + 1] = rgb(0x28u, r, g, b);
    chain[p + 2] = vertex(x0, y0);
    chain[p + 3] = vertex(x1, y1);
    chain[p + 4] = vertex(x2, y2);
    chain[p + 5] = vertex(x3, y3);
}

void
psx_draw_rot(const psx_texinfo *t, int cx, int cy, int angle, fix16 scale)
{
    /* Half extents in fixed point, scaled. */
    fix16 hw = psx_fmul((fix16)t->w << 16, scale) >> 1;
    fix16 hh = psx_fmul((fix16)t->h << 16, scale) >> 1;
    fix16 c = psx_fcos(angle);
    fix16 s = psx_fsin(angle);

    /* The four corners (TL, TR, BL, BR) as fixed-point offsets, rotated by the
     * angle and added to the center. Rotating the sprite's corners is the whole
     * 2D transform; the GPU fills the quad between them. */
    int cxv[4], cyv[4];
    fix16 ox[4] = { -hw,  hw, -hw,  hw };
    fix16 oy[4] = { -hh, -hh,  hh,  hh };
    for (int i = 0; i < 4; i++) {
        fix16 rx = psx_fmul(ox[i], c) - psx_fmul(oy[i], s);
        fix16 ry = psx_fmul(ox[i], s) + psx_fmul(oy[i], c);
        cxv[i] = cx + (rx >> 16);
        cyv[i] = cy + (ry >> 16);
    }

    int u0 = t->u, v0 = t->v, u1 = t->u + t->w, v1 = t->v + t->h;

    int p = node(9);                                /* GP0(2Dh): textured quad, raw */
    if (p < 0)
        return;
    chain[p + 1] = (0x2Du << 24) | 0x808080u;
    chain[p + 2] = vertex(cxv[0], cyv[0]);
    chain[p + 3] = ((uint32_t)t->clut << 16) | ((uint32_t)(v0 & 0xFF) << 8) | (uint32_t)(u0 & 0xFF);
    chain[p + 4] = vertex(cxv[1], cyv[1]);
    chain[p + 5] = ((uint32_t)t->tpage << 16) | ((uint32_t)(v0 & 0xFF) << 8) | (uint32_t)(u1 & 0xFF);
    chain[p + 6] = vertex(cxv[2], cyv[2]);
    chain[p + 7] = ((uint32_t)(v1 & 0xFF) << 8) | (uint32_t)(u0 & 0xFF);
    chain[p + 8] = vertex(cxv[3], cyv[3]);
    chain[p + 9] = ((uint32_t)(v1 & 0xFF) << 8) | (uint32_t)(u1 & 0xFF);
}

/* The handle forms look the texture up in the registry (tex.c) and call the
 * struct draws. A bad handle resolves to NULL and draws nothing. */
void
psx_tex_draw(int handle, int x, int y)
{
    const psx_texinfo *t = psx_tex_at(handle);
    if (t)
        psx_draw_sprite(t, x, y);
}

void
psx_tex_draw_uv(int handle, int x, int y, int u, int v, int w, int h)
{
    const psx_texinfo *t = psx_tex_at(handle);
    if (t)
        psx_draw_sprite_uv(t, x, y, u, v, w, h);
}

void
psx_tex_rot(int handle, int cx, int cy, int angle, fix16 scale)
{
    const psx_texinfo *t = psx_tex_at(handle);
    if (t)
        psx_draw_rot(t, cx, cy, angle, scale);
}

/* Link the tail of this frame's chain (the drawing environment and background
 * clear) to another DMA list, so a single psx_display_swap draws the chain and
 * then that list. The ordering table (ot.c) uses this to append its
 * depth-sorted primitives after the clear. */
void
psx_gpu_chain_link(uint32_t next_addr)
{
    if (chain_tail >= 0)
        chain[chain_tail] = (chain[chain_tail] & 0xFF000000u) | (next_addr & LINK_END);
}

void
psx_display_swap(void)
{
    psx_dma_gpu_list((const uint32_t *)(uintptr_t)chain_head);  /* draw back buffer */
    shown ^= 1;
    set_display(shown);             /* show it */
    frame_begin(shown ^ 1);         /* prepare the next back buffer */
}
