/* ot.c - libpsx ordering table (depth-sorted primitive submission).
 *
 * An ordering table turns per-primitive depth into draw order in one pass. It
 * is an array of list heads, one per depth bucket, cleared so the GPU DMA walks
 * them from the farthest bucket to the nearest. A primitive is added to the
 * bucket for its depth (the GTE's OTZ, from psx_gte_avsz3), so when the table
 * is drawn the far primitives come first and the near ones last, painting the
 * scene back to front without a separate sort.
 *
 * The table and its primitive packets live in RAM; psx_ot_link hands the table
 * to the GPU chain so a single psx_display_swap draws the frame's clear and
 * then the sorted primitives. This is the renderer a real 3D scene uses in
 * place of the flat, order-as-issued path.
 */

#include "psx.h"

#define OT_SIZE   2048                  /* depth buckets */
#define PKT_WORDS 8192                  /* primitive packet pool */
#define LINK_END  0xFFFFFFu

static uint32_t ot[OT_SIZE];
static uint32_t pkt[PKT_WORDS];
static uint32_t pktpos;

static uint32_t
rgb(uint32_t cmd, uint8_t r, uint8_t g, uint8_t b)
{
    return (cmd << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static uint32_t
vertex(int x, int y)
{
    return ((uint32_t)(y & 0xFFFF) << 16) | (uint32_t)(x & 0xFFFF);
}

/* Pack a 0xRRGGBB color into a GP0 word (red low, blue high) with a command
 * byte on top; cmd is 0 for the per-vertex color words of a shaded primitive. */
static uint32_t
rgb24(uint32_t cmd, uint32_t c)
{
    uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
    return (cmd << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

void
psx_ot_clear(void)
{
    pktpos = 0;
    ot[0] = LINK_END;                   /* bucket 0 ends the list */
    for (int i = 1; i < OT_SIZE; i++)
        ot[i] = (uint32_t)(uintptr_t)&ot[i - 1] & LINK_END;
}

/* Reserve a primitive node of n payload words, link it at the head of bucket z,
 * and return a pointer to its payload words (or NULL when the pool is full). */
static uint32_t *
node(int z, int n)
{
    if (z < 0)
        z = 0;
    if (z >= OT_SIZE)
        z = OT_SIZE - 1;
    if (pktpos + (uint32_t)n + 1u > PKT_WORDS)
        return 0;

    uint32_t *p = &pkt[pktpos];
    pktpos += (uint32_t)n + 1u;
    p[0] = ((uint32_t)n << 24) | (ot[z] & LINK_END);    /* count + old head */
    ot[z] = (uint32_t)(uintptr_t)p & LINK_END;          /* bucket -> this node */
    return p;
}

void
psx_ot_add_poly3(int x0, int y0, int x1, int y1, int x2, int y2,
                 uint8_t r, uint8_t g, uint8_t b, int z)
{
    uint32_t *p = node(z, 4);                   /* GP0(20h): flat triangle */
    if (!p)
        return;
    p[1] = rgb(0x20u, r, g, b);
    p[2] = vertex(x0, y0);
    p[3] = vertex(x1, y1);
    p[4] = vertex(x2, y2);
}

void
psx_ot_add_poly4(int x0, int y0, int x1, int y1, int x2, int y2,
                 int x3, int y3, uint8_t r, uint8_t g, uint8_t b, int z)
{
    uint32_t *p = node(z, 5);                   /* GP0(28h): flat quad */
    if (!p)
        return;
    p[1] = rgb(0x28u, r, g, b);
    p[2] = vertex(x0, y0);
    p[3] = vertex(x1, y1);
    p[4] = vertex(x2, y2);
    p[5] = vertex(x3, y3);
}

void
psx_ot_add_gouraud3(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1,
                    int x2, int y2, uint32_t c2, int z)
{
    uint32_t *p = node(z, 6);                   /* GP0(30h): Gouraud triangle */
    if (!p)
        return;
    p[1] = rgb24(0x30u, c0);
    p[2] = vertex(x0, y0);
    p[3] = rgb24(0, c1);
    p[4] = vertex(x1, y1);
    p[5] = rgb24(0, c2);
    p[6] = vertex(x2, y2);
}

void
psx_ot_add_gouraud4(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1,
                    int x2, int y2, uint32_t c2, int x3, int y3, uint32_t c3,
                    int z)
{
    uint32_t *p = node(z, 8);                   /* GP0(38h): Gouraud quad */
    if (!p)
        return;
    p[1] = rgb24(0x38u, c0);
    p[2] = vertex(x0, y0);
    p[3] = rgb24(0, c1);
    p[4] = vertex(x1, y1);
    p[5] = rgb24(0, c2);
    p[6] = vertex(x2, y2);
    p[7] = rgb24(0, c3);
    p[8] = vertex(x3, y3);
}

void
psx_ot_add_tex4(int handle, int x0, int y0, int x1, int y1,
                int x2, int y2, int x3, int y3, int z)
{
    const psx_texinfo *t = psx_tex_at(handle);
    if (!t)
        return;
    uint32_t *p = node(z, 9);                   /* GP0(2Dh): textured quad, raw */
    if (!p)
        return;
    int u0 = t->u, v0 = t->v, u1 = t->u + t->w, v1 = t->v + t->h;
    p[1] = (0x2Du << 24) | 0x808080u;
    p[2] = vertex(x0, y0);
    p[3] = ((uint32_t)t->clut << 16) | ((uint32_t)(v0 & 0xFF) << 8) | (uint32_t)(u0 & 0xFF);
    p[4] = vertex(x1, y1);
    p[5] = ((uint32_t)t->tpage << 16) | ((uint32_t)(v0 & 0xFF) << 8) | (uint32_t)(u1 & 0xFF);
    p[6] = vertex(x2, y2);
    p[7] = ((uint32_t)(v1 & 0xFF) << 8) | (uint32_t)(u0 & 0xFF);
    p[8] = vertex(x3, y3);
    p[9] = ((uint32_t)(v1 & 0xFF) << 8) | (uint32_t)(u1 & 0xFF);
}

void
psx_ot_link(void)
{
    /* Chain the table after the frame's clear; the farthest bucket is the DMA
     * start, so the list draws back to front. */
    psx_gpu_chain_link((uint32_t)(uintptr_t)&ot[OT_SIZE - 1]);
}
