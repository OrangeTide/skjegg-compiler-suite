/* asset_iso.c - the milestone-7 sample: assets from an ISO 9660 image through
 * the shared fs/ layer.
 *
 * The same fs_open / fs_read as the romfs sample, over an isofs mount instead.
 * Here the ISO image is embedded in the executable and mounted from RAM, which
 * exercises the ISO 9660 parser on the console. On real hardware the image
 * stays on the disc and isofs streams sectors through libpsx/cd.c; the parser
 * is the same either way.
 *
 * It loads a known file, checks its bytes, then draws from them, and exits 0 on
 * success. Distinct nonzero codes name the step that failed.
 */

#include "psx.h"
#include "fs.h"
#include "isofs.h"

extern const unsigned char psx_iso_img[];
extern const unsigned long psx_iso_img_len;

#define FRAMES 120

int
main(void)
{
    char buf[32];

    fs_init();
    if (isofs_mount("cd", psx_iso_img, (unsigned)psx_iso_img_len, 0) < 0)
        return 1;

    int fd = fs_open("cd:greet.txt", FS_O_RDONLY, 0);
    if (fd < 0)
        return 2;
    int n = fs_read(fd, buf, sizeof buf);
    fs_close(fd);
    if (n != 21)                        /* "hello from the cd-rom" */
        return 3;
    if (fs_open("cd:absent.txt", FS_O_RDONLY, 0) >= 0)
        return 4;

    psx_gpu_init();
    psx_irq_init();
    psx_set_bg(0, 30, 0);

    for (int f = 0; f < FRAMES; f++) {
        psx_draw_rect((unsigned char)buf[0], 100, 40, 40, 255, 255, 0);
        psx_vsync();
        psx_display_swap();
    }

    return 0;
}
