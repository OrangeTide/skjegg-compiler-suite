/* game.c - the milestone-4 libpsx sample: a complete little 2D game.
 *
 * Move the yellow player square with the d-pad to touch the bobbing coin. Each
 * touch scores a point, plays a tone, and respawns the coin at a pseudo-random
 * spot. It pulls together everything in the C SDK so far: GPU/DMA/vsync, pad
 * input, SPU sound, romfs assets (a palette), and fixed-point math (player
 * motion and the coin's sine bob). The game ends at ten points or after a time
 * limit, exiting 0.
 *
 * On a headless PCSX-Redux run there is no input, so the player stays put and
 * the loop runs to its frame limit and exits 0, exercising every subsystem.
 */

#include "psx.h"
#include "fs.h"

extern const unsigned char psx_asset_img[];
extern const unsigned long psx_asset_img_len;
extern const unsigned char psx_tone[];
extern const unsigned psx_tone_len;

#define SZ 20
#define FRAMES 900
#define WIN_SCORE 10
#define TONE_ADDR 0x1010u

static unsigned rng_state = 1;

static unsigned
rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 16;
}

static int
overlap(int ax, int ay, int bx, int by)
{
    return ax < bx + SZ && bx < ax + SZ && ay < by + SZ && by < ay + SZ;
}

int
main(void)
{
    unsigned char pal[16];
    int i;

    fs_init();
    romfs_mount("rom", psx_asset_img, (unsigned)psx_asset_img_len, 0);
    int fd = fs_open("rom:sprite.raw", FS_O_RDONLY, 0);
    if (fd >= 0) {
        fs_read(fd, pal, 16);
        fs_close(fd);
    } else {
        for (i = 0; i < 16; i++)
            pal[i] = (unsigned char)(i * 16);
    }

    psx_gpu_init();
    psx_irq_init();
    psx_spu_init();
    psx_spu_upload(TONE_ADDR, psx_tone, psx_tone_len);
    psx_set_bg(pal[0], pal[1], pal[8]);

    fix16 px = PSX_FIX((PSX_SCREEN_W - SZ) / 2);
    fix16 py = PSX_FIX((PSX_SCREEN_H - SZ) / 2);
    fix16 speed = PSX_FIX(2);
    int cx = 40, cy = 40, score = 0;

    for (int f = 0; f < FRAMES && score < WIN_SCORE; f++) {
        uint16_t b = psx_pad_read();
        if (b & PSX_PAD_LEFT)
            px -= speed;
        if (b & PSX_PAD_RIGHT)
            px += speed;
        if (b & PSX_PAD_UP)
            py -= speed;
        if (b & PSX_PAD_DOWN)
            py += speed;

        int x = PSX_FIX_INT(px);
        int y = PSX_FIX_INT(py);
        if (x < 0) { x = 0; px = 0; }
        if (x > PSX_SCREEN_W - SZ) { x = PSX_SCREEN_W - SZ; px = PSX_FIX(x); }
        if (y < 0) { y = 0; py = 0; }
        if (y > PSX_SCREEN_H - SZ) { y = PSX_SCREEN_H - SZ; py = PSX_FIX(y); }

        /* The coin bobs with a fixed-point sine. */
        int cby = cy + PSX_FIX_INT(psx_fmul(psx_fsin(f * 8), PSX_FIX(6)));

        if (overlap(x, y, cx, cby)) {
            score++;
            psx_spu_play(0, TONE_ADDR, PSX_SPU_PITCH_22050, 0x3FFF);
            cx = (int)(rng() % (unsigned)(PSX_SCREEN_W - SZ));
            cy = (int)(rng() % (unsigned)(PSX_SCREEN_H - SZ - 12)) + 6;
        }

        psx_draw_rect(cx, cby, SZ, SZ, pal[4], pal[8], pal[12]);   /* coin */
        psx_draw_rect(x, y, SZ, SZ, 255, 220, 0);                  /* player */
        psx_vsync();
        psx_display_swap();
    }

    return 0;
}
