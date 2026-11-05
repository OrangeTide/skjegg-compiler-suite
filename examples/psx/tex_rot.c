/* tex_rot.c - sprite-sheet cells and a rotated sprite.
 *
 * Exercises the two richer sprite draws on top of the plain one: psx_draw_sprite_uv
 * picks a cell out of a texture (the sprite-sheet case), and psx_draw_rot draws a
 * texture as a quad with rotated corners (the 2D transform a game uses to spin a
 * sprite). It draws two cells of the four-quadrant texture and a solid sprite
 * rotated 45 degrees, then exits 0.
 *
 * The framebuffer check reads known colors: the top-left cell is red, the
 * top-right cell is green, and the rotated solid sprite reaches past where an
 * axis-aligned sprite of the same size could, which only a rotation can do.
 */

#include "psx.h"

extern const unsigned char psx_rtex[];      /* four-quadrant texture */
extern const unsigned      psx_rtex_len;
extern const unsigned char psx_stex[];      /* solid magenta texture */
extern const unsigned      psx_stex_len;

#define FRAMES 120

int
main(void)
{
    psx_texinfo quad, solid;

    psx_gpu_init();
    psx_irq_init();
    psx_set_bg(0, 0, 20);

    if (psx_tim_upload(psx_rtex, &quad) != 0)
        return 2;
    if (psx_tim_upload(psx_stex, &solid) != 0)
        return 3;

    for (int f = 0; f < FRAMES; f++) {
        psx_draw_sprite_uv(&quad, 40, 40, 0, 0, 32, 32);    /* top-left cell: red */
        psx_draw_sprite_uv(&quad, 90, 40, 32, 0, 32, 32);   /* top-right cell: green */
        psx_draw_rot(&solid, 200, 120, 128, PSX_FIX_ONE);   /* 45 deg, 1:1 */
        psx_vsync();
        psx_display_swap();
    }

    return 0;
}
