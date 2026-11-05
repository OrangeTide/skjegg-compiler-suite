/* cd_stream.c - read an asset from the CD-ROM by streaming, not from RAM.
 *
 * Unlike asset_iso.c, which mounts an ISO image embedded in the executable,
 * this mounts the real disc: psx_cd_mount wires the ISO 9660 backend to the
 * CD-ROM controller (libpsx/cd.c) so sectors are read on demand. A disc-sized
 * game keeps its assets on the disc this way instead of holding them in the
 * 2 MB of main RAM.
 *
 * It opens a known file through the ordinary fs_open / fs_read, checks the
 * bytes, draws a bar from them, and exits 0 on success. Distinct nonzero codes
 * name the step that failed.
 *
 * This needs a real disc in the drive, so it runs under an emulator booted with
 * a data disc image, not the bare PS-EXE path.
 */

#include "psx.h"
#include "fs.h"

#define FRAMES 60

int
main(void)
{
    char buf[32];

    psx_gpu_init();
    psx_irq_init();
    psx_set_bg(0, 0, 30);

    psx_cd_init();
    fs_init();
    if (psx_cd_mount("cd") < 0)
        return 2;

    int fd = fs_open("cd:greet.txt", FS_O_RDONLY, 0);
    if (fd < 0)
        return 3;
    int n = fs_read(fd, buf, sizeof buf);
    fs_close(fd);
    if (n != 21)                        /* "hello from the cd-rom" */
        return 4;

    for (int f = 0; f < FRAMES; f++) {
        psx_draw_rect((unsigned char)buf[0], 100, 40, 40, 0, 255, 0);
        psx_vsync();
        psx_display_swap();
    }

    return 0;
}
