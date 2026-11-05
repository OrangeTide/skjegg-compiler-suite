/* sprite_asset.c - the C accessor that loads the sprite samples' textures.
 *
 * Pascal and Kobold cannot pass a raw data pointer, so the pointer to
 * an embedded TIM stays here in C: these functions upload a texture and return
 * its integer handle, which the Pascal and Kobold samples then draw through the
 * psx unit's handle calls. It is the small per-game glue the handle API expects.
 */

#include "psx.h"

extern const unsigned char psx_rtex[];      /* four-quadrant texture */
extern const unsigned char psx_stex[];      /* solid magenta texture */

int
psx_load_quad(void)
{
    return psx_tex_upload(psx_rtex);
}

int
psx_load_solid(void)
{
    return psx_tex_upload(psx_stex);
}
