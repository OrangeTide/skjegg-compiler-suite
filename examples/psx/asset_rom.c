/* asset_rom.c - the milestone-3 libpsx sample: assets from an embedded romfs.
 *
 * The game's assets are packed into a romfs image at build time (skj-mkromfs),
 * embedded in the executable as a byte array (bin2c), and read at runtime
 * through the shared fs/ layer: the same fs_open / fs_read a sandbox or VM
 * guest uses, here over an in-memory romfs mount. A later milestone adds an
 * ISO 9660 backend behind the same interface for assets that stay on the disc.
 *
 * This loads a known file and checks its bytes, then draws a rectangle tinted
 * from the loaded data so the asset visibly reaches the screen. It exits 0 on
 * success; distinct nonzero codes name the step that failed.
 */

#include "psx.h"
#include "fs.h"

extern const unsigned char psx_asset_img[];
extern const unsigned long psx_asset_img_len;

#define FRAMES 120

int
main(void)
{
    unsigned char buf[16];

    fs_init();
    if (romfs_mount("rom", psx_asset_img, (unsigned)psx_asset_img_len, 0) < 0)
        return 1;

    int fd = fs_open("rom:sprite.raw", FS_O_RDONLY, 0);
    if (fd < 0)
        return 2;
    int n = fs_read(fd, buf, sizeof buf);
    fs_close(fd);
    if (n != 16)
        return 3;
    for (int i = 0; i < 16; i++) {
        if (buf[i] != (unsigned char)(i * 16))
            return 4;
    }

    /* A path that is not there must fail. */
    if (fs_open("rom:absent", FS_O_RDONLY, 0) >= 0)
        return 5;

    psx_gpu_init();
    psx_irq_init();
    psx_set_bg(0, 0, 40);

    for (int f = 0; f < FRAMES; f++) {
        /* Tint and place the square from bytes read out of the asset. */
        psx_draw_rect(buf[2], buf[3], 32, 32, buf[4], buf[8], buf[12]);
        psx_vsync();
        psx_display_swap();
    }

    return 0;
}
