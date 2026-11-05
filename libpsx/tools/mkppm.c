/* mkppm.c - write a deterministic test-pattern PPM for the texture tests.
 *
 * Emits a square binary (P6) PPM split into four solid-color quadrants: red,
 * green, blue, white. timconv converts it to a TIM, the sample draws it, and
 * shotcheck probes the four quadrant colors in the captured frame, so the whole
 * texture path is checked against known pixels. The four colors are chosen to
 * survive the console's 15-bit color (each channel is 0 or 255, so no rounding
 * ambiguity).
 *
 * Usage: mkppm <size> <out.ppm> [--gradient | --solid R G B]
 *   default    four solid quadrants (red, green, blue, white)
 *   --gradient a smooth RGB gradient (thousands of colors, for the quantizer)
 *   --solid    one solid color (for the rotation test, any covered pixel is it)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
    int gradient = 0, solid = 0;
    int sr = 0, sg = 0, sb = 0;
    const char *sz = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gradient")) gradient = 1;
        else if (!strcmp(argv[i], "--solid") && i + 3 < argc) {
            solid = 1;
            sr = atoi(argv[++i]); sg = atoi(argv[++i]); sb = atoi(argv[++i]);
        } else if (!sz) sz = argv[i];
        else out = argv[i];
    }
    if (!sz || !out) {
        fprintf(stderr, "usage: mkppm <size> <out.ppm> [--gradient | --solid R G B]\n");
        return 2;
    }
    int n = atoi(sz);
    if (n < 2 || n > 256 || (n & 1)) {
        fprintf(stderr, "mkppm: size must be even, 2..256\n");
        return 2;
    }
    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "mkppm: cannot write %s\n", out);
        return 2;
    }
    fprintf(f, "P6\n%d %d\n255\n", n, n);
    int half = n / 2;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            unsigned char px[3];
            if (solid) {
                px[0] = (unsigned char)sr; px[1] = (unsigned char)sg; px[2] = (unsigned char)sb;
            } else if (gradient) {
                px[0] = (unsigned char)(x * 255 / (n - 1));      /* R across */
                px[1] = (unsigned char)(y * 255 / (n - 1));      /* G down */
                px[2] = (unsigned char)((x + y) * 255 / (2 * (n - 1)));
            } else {
                int top = y < half, left = x < half;
                if (top && left) {          px[0] = 255; px[1] = 0;   px[2] = 0;   }
                else if (top && !left) {    px[0] = 0;   px[1] = 255; px[2] = 0;   }
                else if (!top && left) {    px[0] = 0;   px[1] = 0;   px[2] = 255; }
                else {                      px[0] = 255; px[1] = 255; px[2] = 255; }
            }
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}
