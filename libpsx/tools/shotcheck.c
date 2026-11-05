/* shotcheck.c - verify a PlayStation framebuffer captured by redux_shot.lua.
 *
 * Reads the capture file (u16 width, u16 height, u16 bpp, then pixels), decodes
 * it to 8-bit RGB, and checks pixels against expected colors. This is the
 * golden-framebuffer side of the libpsx test tier: redux_shot.lua grabs what
 * the GPU actually put on screen with no window open, and this host tool turns
 * that into an exit-code pass/fail the Makefile can gate on.
 *
 * It can also write a PPM so a human can look at the frame.
 *
 * Usage:
 *   shotcheck <shot.bin> [--ppm out.ppm]
 *                        [--at X Y R G B [tol]]   probe one pixel
 *                        [--find R G B [tol]]     require the color somewhere
 * Each --at / --find that fails makes the tool exit nonzero. With no checks it
 * just reports the dimensions (and writes the PPM if asked), exiting 0.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int width, height, bpp;
static unsigned char *rgb;              /* width*height*3, 8-bit */

/* Expand a 5-bit channel to 8 bits the way the hardware's 15-bit output reads:
 * replicate the top bits into the low ones. */
static int
c5to8(int c)
{
    return (c << 3) | (c >> 2);
}

static void
decode(const unsigned char *px)
{
    rgb = malloc((size_t)width * height * 3);
    if (!rgb) {
        fprintf(stderr, "shotcheck: out of memory\n");
        exit(9);
    }
    for (int i = 0; i < width * height; i++) {
        int r, g, b;
        if (bpp == 0) {                 /* BGR555, little-endian */
            int v = px[i * 2] | (px[i * 2 + 1] << 8);
            r = c5to8(v & 0x1f);
            g = c5to8((v >> 5) & 0x1f);
            b = c5to8((v >> 10) & 0x1f);
        } else {                        /* 24-bit, R,G,B in memory order */
            r = px[i * 3];
            g = px[i * 3 + 1];
            b = px[i * 3 + 2];
        }
        rgb[i * 3] = (unsigned char)r;
        rgb[i * 3 + 1] = (unsigned char)g;
        rgb[i * 3 + 2] = (unsigned char)b;
    }
}

static int
near(int a, int b, int tol)
{
    int d = a - b;
    if (d < 0) d = -d;
    return d <= tol;
}

static void
write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "shotcheck: cannot write %s\n", path);
        exit(9);
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    fwrite(rgb, 1, (size_t)width * height * 3, f);
    fclose(f);
    fprintf(stderr, "shotcheck: wrote %s (%dx%d)\n", path, width, height);
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: shotcheck <shot.bin> [--ppm out] "
                        "[--at x y r g b [tol]] [--find r g b [tol]]\n");
        return 9;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "shotcheck: cannot open %s\n", argv[1]);
        return 9;
    }
    unsigned char hdr[6];
    if (fread(hdr, 1, 6, f) != 6) {
        fprintf(stderr, "shotcheck: short header\n");
        return 9;
    }
    width = hdr[0] | (hdr[1] << 8);
    height = hdr[2] | (hdr[3] << 8);
    bpp = hdr[4] | (hdr[5] << 8);
    if (width <= 0 || height <= 0 || width > 1024 || height > 512) {
        fprintf(stderr, "shotcheck: bad dimensions %dx%d\n", width, height);
        return 1;
    }
    int bytespp = (bpp == 0) ? 2 : 3;
    size_t npx = (size_t)width * height;
    unsigned char *px = malloc(npx * bytespp);
    if (!px || fread(px, 1, npx * bytespp, f) != npx * bytespp) {
        fprintf(stderr, "shotcheck: short pixel data\n");
        return 9;
    }
    fclose(f);
    decode(px);
    free(px);

    fprintf(stderr, "shotcheck: %dx%d, %s\n", width, height,
            bpp == 0 ? "15-bit" : "24-bit");

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--ppm") && i + 1 < argc) {
            write_ppm(argv[i + 1]);
            i += 1;
        } else if (!strcmp(argv[i], "--at") && i + 5 < argc) {
            int x = atoi(argv[i + 1]), y = atoi(argv[i + 2]);
            int r = atoi(argv[i + 3]), g = atoi(argv[i + 4]), b = atoi(argv[i + 5]);
            int tol = 8;
            int consumed = 5;
            if (i + 6 < argc && argv[i + 6][0] >= '0' && argv[i + 6][0] <= '9') {
                tol = atoi(argv[i + 6]);
                consumed = 6;
            }
            if (x < 0 || x >= width || y < 0 || y >= height) {
                fprintf(stderr, "  at %d,%d: out of frame\n", x, y);
                rc = 1;
            } else {
                unsigned char *p = &rgb[(y * width + x) * 3];
                int ok = near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol);
                fprintf(stderr, "  at %d,%d = %d,%d,%d want %d,%d,%d (+-%d): %s\n",
                        x, y, p[0], p[1], p[2], r, g, b, tol, ok ? "ok" : "FAIL");
                if (!ok) rc = 1;
            }
            i += consumed;
        } else if (!strcmp(argv[i], "--find") && i + 3 < argc) {
            int r = atoi(argv[i + 1]), g = atoi(argv[i + 2]), b = atoi(argv[i + 3]);
            int tol = 8;
            int consumed = 3;
            if (i + 4 < argc && argv[i + 4][0] >= '0' && argv[i + 4][0] <= '9') {
                tol = atoi(argv[i + 4]);
                consumed = 4;
            }
            int found = 0;
            for (size_t k = 0; k < npx && !found; k++) {
                unsigned char *p = &rgb[k * 3];
                if (near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol))
                    found = 1;
            }
            fprintf(stderr, "  find %d,%d,%d (+-%d): %s\n", r, g, b, tol,
                    found ? "ok" : "FAIL");
            if (!found) rc = 1;
            i += consumed;
        } else {
            fprintf(stderr, "shotcheck: unknown arg %s\n", argv[i]);
            return 9;
        }
    }
    free(rgb);
    return rc;
}
