/* timconv.c - host converter from a PPM image to a PlayStation TIM texture.
 *
 * TIM is the console's standard texture format: a flags word, an optional CLUT
 * (palette) block with its VRAM destination, then the pixel block with its own
 * VRAM destination. libpsx/tex.c (psx_tim_upload) reads exactly this layout and
 * copies the blocks into VRAM. Keeping the real format means the output is a
 * normal TIM, not a private one.
 *
 * Two pixel modes: 16-bit direct color (--16, always lossless to 15-bit) and
 * 8-bit paletted (--8). The 8-bit palette is built from the image's distinct
 * colors; an image with more than 256 of them is rejected rather than quantized,
 * so the conversion is deterministic and exact. A quantizer for richer images is
 * a later addition.
 *
 * Colors become 15-bit BGR555 the way the GPU stores them. VRAM destinations
 * default to the free region past the framebuffers (see tex.c); --vram and
 * --clut override them so several textures can be laid out without overlap.
 *
 * Usage:
 *   timconv <in.ppm> <out> [--8|--16] [--vram X Y] [--clut X Y]
 *                          [-c SYMBOL]   emit a C array instead of a .tim
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t
bgr555(unsigned char r, unsigned char g, unsigned char b)
{
    return (uint16_t)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

/* Read a binary (P6) PPM. Returns the pixel buffer (w*h*3 bytes) or NULL. */
static unsigned char *
read_ppm(const char *path, int *pw, int *ph)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "timconv: cannot open %s\n", path);
        return NULL;
    }
    char magic[3] = {0};
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fprintf(stderr, "timconv: %s is not a P6 PPM\n", path);
        fclose(f);
        return NULL;
    }
    if (fscanf(f, "%d %d %d", &w, &h, &maxv) != 3 || maxv != 255) {
        fprintf(stderr, "timconv: bad PPM header (need maxval 255)\n");
        fclose(f);
        return NULL;
    }
    fgetc(f);                                   /* the single whitespace byte */
    size_t n = (size_t)w * h * 3;
    unsigned char *buf = malloc(n);
    if (!buf || fread(buf, 1, n, f) != n) {
        fprintf(stderr, "timconv: short pixel data\n");
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *pw = w;
    *ph = h;
    return buf;
}

/* Append a little-endian 16/32-bit value to a growing byte vector. */
static void
put16(unsigned char *d, size_t *n, uint16_t v)
{
    d[(*n)++] = (unsigned char)(v & 0xFF);
    d[(*n)++] = (unsigned char)(v >> 8);
}

static void
put32(unsigned char *d, size_t *n, uint32_t v)
{
    d[(*n)++] = (unsigned char)(v & 0xFF);
    d[(*n)++] = (unsigned char)((v >> 8) & 0xFF);
    d[(*n)++] = (unsigned char)((v >> 16) & 0xFF);
    d[(*n)++] = (unsigned char)((v >> 24) & 0xFF);
}

/* --- 8-bit palette building: exact when the image has few enough colors,
 * median-cut quantization when it does not. ---
 *
 * Everything works on the 15-bit BGR555 grid the GPU stores, so a palette entry
 * is already a storable color and no second rounding step can merge entries
 * behind the quantizer's back. There are at most 32768 distinct colors on that
 * grid, so the distinct set and per-color counts fit a direct 32768-entry table.
 */

/* Unique colors on the 5-bit-per-channel grid, with pixel counts. */
static int qr[32768], qg[32768], qb[32768];
static uint32_t qcount[32768];

static int qsort_channel;        /* channel median-cut currently sorts on */

static int
cmp_item(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    int va = (qsort_channel == 0) ? qr[ia] : (qsort_channel == 1) ? qg[ia] : qb[ia];
    int vb = (qsort_channel == 0) ? qr[ib] : (qsort_channel == 1) ? qg[ib] : qb[ib];
    if (va != vb)
        return va - vb;
    return ia - ib;                     /* stable, deterministic tie-break */
}

/* Reduce a distinct-color set (qr/qg/qb/qcount, nuniq entries) to at most
 * ncolors representative colors by median cut. Writes the 5-bit palette to
 * pal_r/g/b and returns the palette size. */
static int
median_cut(int nuniq, int ncolors, int *pal_r, int *pal_g, int *pal_b)
{
    int *ord = malloc((size_t)nuniq * sizeof *ord);
    for (int i = 0; i < nuniq; i++)
        ord[i] = i;

    /* Boxes are contiguous ranges of ord[]. */
    int *bstart = malloc((size_t)ncolors * sizeof *bstart);
    int *blen = malloc((size_t)ncolors * sizeof *blen);
    bstart[0] = 0;
    blen[0] = nuniq;
    int nboxes = 1;

    while (nboxes < ncolors) {
        /* Pick the splittable box holding the most pixels. */
        int pick = -1;
        uint32_t best = 0;
        for (int b = 0; b < nboxes; b++) {
            if (blen[b] < 2)
                continue;
            uint32_t sum = 0;
            for (int i = 0; i < blen[b]; i++)
                sum += qcount[ord[bstart[b] + i]];
            if (pick < 0 || sum > best) {
                best = sum;
                pick = b;
            }
        }
        if (pick < 0)
            break;                      /* nothing left to split */

        int s = bstart[pick], n = blen[pick];
        int rmin = 255, rmax = 0, gmin = 255, gmax = 0, bmin = 255, bmax = 0;
        for (int i = 0; i < n; i++) {
            int c = ord[s + i];
            if (qr[c] < rmin) rmin = qr[c];
            if (qr[c] > rmax) rmax = qr[c];
            if (qg[c] < gmin) gmin = qg[c];
            if (qg[c] > gmax) gmax = qg[c];
            if (qb[c] < bmin) bmin = qb[c];
            if (qb[c] > bmax) bmax = qb[c];
        }
        int rr = rmax - rmin, gr = gmax - gmin, br = bmax - bmin;
        qsort_channel = (rr >= gr && rr >= br) ? 0 : (gr >= br) ? 1 : 2;
        qsort(ord + s, (size_t)n, sizeof *ord, cmp_item);

        /* Split at the median by pixel count, both sides nonempty. */
        uint32_t total = 0;
        for (int i = 0; i < n; i++)
            total += qcount[ord[s + i]];
        uint32_t acc = 0;
        int split = 1;
        for (int i = 0; i < n - 1; i++) {
            acc += qcount[ord[s + i]];
            if (acc * 2 >= total) {
                split = i + 1;
                break;
            }
        }

        blen[pick] = split;
        bstart[nboxes] = s + split;
        blen[nboxes] = n - split;
        nboxes++;
    }

    /* Each box's representative is its pixel-count-weighted average. */
    for (int b = 0; b < nboxes; b++) {
        uint64_t sr = 0, sg = 0, sb = 0, sw = 0;
        for (int i = 0; i < blen[b]; i++) {
            int c = ord[bstart[b] + i];
            uint32_t w = qcount[c];
            sr += (uint64_t)qr[c] * w;
            sg += (uint64_t)qg[c] * w;
            sb += (uint64_t)qb[c] * w;
            sw += w;
        }
        pal_r[b] = (int)((sr + sw / 2) / sw);
        pal_g[b] = (int)((sg + sw / 2) / sw);
        pal_b[b] = (int)((sb + sw / 2) / sw);
    }

    free(ord);
    free(bstart);
    free(blen);
    return nboxes;
}

/* Build an 8-bit palette and per-pixel indices for the image. Fills pal[] (as
 * BGR555), writes w*h indices to idx, and returns the palette size, or -1 on
 * out of memory. Colors become exact entries while they fit in ncolors;
 * otherwise median_cut reduces them and each pixel maps to its nearest entry. */
static int
build_palette_8(const unsigned char *px, int w, int h, int ncolors,
                uint16_t *pal, unsigned char *idx)
{
    static int map[32768];              /* 15-bit color -> palette index */
    for (int i = 0; i < 32768; i++) {
        qcount[i] = 0;
        map[i] = -1;
    }

    /* Tally distinct 15-bit colors. */
    int n = w * h;
    for (int i = 0; i < n; i++) {
        uint16_t c = bgr555(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);
        qcount[c]++;
    }
    int nuniq = 0;
    for (int c = 0; c < 32768; c++) {
        if (qcount[c]) {
            qr[nuniq] = c & 0x1F;
            qg[nuniq] = (c >> 5) & 0x1F;
            qb[nuniq] = (c >> 10) & 0x1F;
            qcount[nuniq] = qcount[c];  /* compact in place; nuniq <= c */
            if (nuniq != c)
                qcount[c] = 0;
            nuniq++;
        }
    }

    int npal;
    if (nuniq <= ncolors) {
        /* Exact: one palette entry per distinct color, lossless. */
        npal = nuniq;
        for (int k = 0; k < nuniq; k++) {
            uint16_t c = (uint16_t)(qr[k] | (qg[k] << 5) | (qb[k] << 10));
            pal[k] = c;
            map[c] = k;
        }
    } else {
        int pal_r[256], pal_g[256], pal_b[256];
        npal = median_cut(nuniq, ncolors, pal_r, pal_g, pal_b);
        for (int k = 0; k < npal; k++)
            pal[k] = (uint16_t)(pal_r[k] | (pal_g[k] << 5) | (pal_b[k] << 10));
    }

    /* Index every pixel. For exact palettes map[] is already set; otherwise
     * resolve each distinct color to its nearest entry once and reuse it. */
    for (int i = 0; i < n; i++) {
        uint16_t c = bgr555(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);
        if (map[c] < 0) {
            int cr = c & 0x1F, cg = (c >> 5) & 0x1F, cb = (c >> 10) & 0x1F;
            int bestk = 0;
            long bestd = -1;
            for (int k = 0; k < npal; k++) {
                int pr = pal[k] & 0x1F, pg = (pal[k] >> 5) & 0x1F, pb = (pal[k] >> 10) & 0x1F;
                long d = (long)(cr - pr) * (cr - pr)
                       + (long)(cg - pg) * (cg - pg)
                       + (long)(cb - pb) * (cb - pb);
                if (bestd < 0 || d < bestd) {
                    bestd = d;
                    bestk = k;
                }
            }
            map[c] = bestk;
        }
        idx[i] = (unsigned char)map[c];
    }
    return npal;
}

static int
emit_c(const char *sym, const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "timconv: cannot write %s\n", path);
        return 1;
    }
    fprintf(f, "/* generated by timconv: TIM texture, do not edit */\n");
    fprintf(f, "const unsigned char %s[] = {\n", sym);
    for (size_t i = 0; i < len; i++)
        fprintf(f, "%u,%s", data[i], (i % 16 == 15) ? "\n" : "");
    fprintf(f, "\n};\n");
    fprintf(f, "const unsigned %s_len = %zu;\n", sym, len);
    fclose(f);
    return 0;
}

static int
write_file(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "timconv: cannot write %s\n", path);
        return 1;
    }
    int ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : 1;
}

/* Exercise the palette builder without the emulator: quantize a smooth gradient
 * (thousands of colors) and check the reconstruction error is small and the
 * palette fits the cap, then check that a low-color image stays exact (zero
 * error). Returns 0 on success. */
static int
selftest(void)
{
    int w = 64, h = 64, n = w * h;
    unsigned char *px = malloc((size_t)n * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char *p = px + (y * w + x) * 3;
            p[0] = (unsigned char)(x * 4);          /* R ramps across */
            p[1] = (unsigned char)(y * 4);          /* G ramps down */
            p[2] = (unsigned char)((x + y) * 2);    /* B on the diagonal */
        }
    }

    uint16_t pal[256];
    unsigned char *idx = malloc((size_t)n);

    /* Quantize the same gradient to a few palette sizes and measure the
     * reconstruction error (5-bit channel RMS). More colors must reduce it, and
     * 256 colors must cover this gradient tightly. */
    double rms[3];
    int caps[3] = {16, 64, 256};
    int npal = 0;
    for (int t = 0; t < 3; t++) {
        npal = build_palette_8(px, w, h, caps[t], pal, idx);
        if (npal > caps[t]) {
            printf("timconv selftest: FAIL palette %d exceeds cap %d\n", npal, caps[t]);
            return 1;
        }
        double sse = 0.0;
        for (int i = 0; i < n; i++) {
            uint16_t c = bgr555(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);
            uint16_t q = pal[idx[i]];
            int dr = (c & 0x1F) - (q & 0x1F);
            int dg = ((c >> 5) & 0x1F) - ((q >> 5) & 0x1F);
            int db = ((c >> 10) & 0x1F) - ((q >> 10) & 0x1F);
            sse += (double)(dr * dr + dg * dg + db * db);
        }
        rms[t] = sqrt(sse / n);          /* in 5-bit channel units, 0..31 */
        printf("timconv selftest: gradient %dx%d -> %d colors, rms %.3f (5-bit)\n",
               w, h, caps[t], rms[t]);
    }
    if (!(rms[0] > rms[1] && rms[1] > rms[2])) {
        printf("timconv selftest: FAIL error did not fall as colors grew\n");
        return 1;
    }
    if (rms[2] > 1.5) {                   /* 256 colors should cover it tightly */
        printf("timconv selftest: FAIL rms %.3f at 256 colors too high\n", rms[2]);
        return 1;
    }

    /* A low-color image must quantize exactly (zero error). */
    for (int i = 0; i < n; i++) {
        unsigned char *p = px + i * 3;
        int q = i % 4;
        p[0] = (q == 0) ? 255 : 0;
        p[1] = (q == 1) ? 255 : 0;
        p[2] = (q == 2) ? 255 : 0;
        if (q == 3) p[0] = p[1] = p[2] = 255;
    }
    npal = build_palette_8(px, w, h, 256, pal, idx);
    int exact = 1;
    for (int i = 0; i < n; i++) {
        uint16_t c = bgr555(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);
        if (pal[idx[i]] != c)
            exact = 0;
    }
    printf("timconv selftest: 4-color image -> %d colors, %s\n",
           npal, exact ? "exact" : "LOSSY");
    free(px);
    free(idx);
    if (!exact || npal != 4) {
        printf("timconv selftest: FAIL low-color image not exact\n");
        return 1;
    }
    printf("timconv selftest: OK\n");
    return 0;
}

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--selftest"))
            return selftest();

    const char *in = NULL, *out = NULL, *sym = NULL;
    int bpp = 8;
    int ncolors = 256;
    int vram_x = 320, vram_y = 0, clut_x = 0, clut_y = 480;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--8")) bpp = 8;
        else if (!strcmp(argv[i], "--16")) bpp = 16;
        else if (!strcmp(argv[i], "--colors") && i + 1 < argc) {
            ncolors = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--vram") && i + 2 < argc) {
            vram_x = atoi(argv[++i]); vram_y = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--clut") && i + 2 < argc) {
            clut_x = atoi(argv[++i]); clut_y = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            sym = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "timconv: unknown option %s\n", argv[i]);
            return 2;
        } else if (!in) in = argv[i];
        else out = argv[i];
    }
    if (!in || !out) {
        fprintf(stderr, "usage: timconv <in.ppm> <out> [--8|--16] "
                        "[--colors N] [--vram X Y] [--clut X Y] [-c SYMBOL]\n");
        return 2;
    }
    if (ncolors < 1 || ncolors > 256) {
        fprintf(stderr, "timconv: --colors must be 1..256\n");
        return 2;
    }

    int w, h;
    unsigned char *px = read_ppm(in, &w, &h);
    if (!px)
        return 1;
    if (w & 1) {
        fprintf(stderr, "timconv: width must be even (two texels per VRAM word)\n");
        free(px);
        return 1;
    }
    if (w > 256 || h > 256) {
        fprintf(stderr, "timconv: texture must fit a 256x256 page\n");
        free(px);
        return 1;
    }
    if (vram_x % 64 != 0 || vram_y % 256 != 0) {
        fprintf(stderr, "timconv: --vram must be a 64x256 page boundary\n");
        free(px);
        return 1;
    }

    /* Upper bound on the TIM size: header + clut + image, each generous. */
    size_t cap = 64 + 256 * 2 + (size_t)w * h * 2;
    unsigned char *tim = malloc(cap);
    size_t n = 0;

    if (bpp == 16) {
        put32(tim, &n, 0x10);                           /* magic */
        put32(tim, &n, 2);                              /* pmode 2 (16-bit) */
        uint16_t iw = (uint16_t)w;                      /* 1 texel per halfword */
        put32(tim, &n, 12 + (uint32_t)iw * h * 2);      /* image block bytes */
        put16(tim, &n, (uint16_t)vram_x);
        put16(tim, &n, (uint16_t)vram_y);
        put16(tim, &n, iw);
        put16(tim, &n, (uint16_t)h);
        for (int i = 0; i < w * h; i++)
            put16(tim, &n, bgr555(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]));
    } else {
        /* Build a palette: exact if the image fits in ncolors, median-cut
         * quantized otherwise. */
        uint16_t pal[256];
        unsigned char *idx = malloc((size_t)w * h);
        if (!idx) {
            free(tim); free(px);
            return 1;
        }
        int npal = build_palette_8(px, w, h, ncolors, pal, idx);

        put32(tim, &n, 0x10);                            /* magic */
        put32(tim, &n, 1 | 0x08);                        /* pmode 1 (8-bit) + CLUT */
        /* CLUT block: always 256 entries wide, one row. */
        put32(tim, &n, 12 + 256 * 2);
        put16(tim, &n, (uint16_t)clut_x);
        put16(tim, &n, (uint16_t)clut_y);
        put16(tim, &n, 256);
        put16(tim, &n, 1);
        for (int k = 0; k < 256; k++)
            put16(tim, &n, (k < npal) ? pal[k] : 0);
        /* Image block: two 8-bit indices per halfword, so width/2 halfwords. */
        uint16_t iw = (uint16_t)(w / 2);
        put32(tim, &n, 12 + (uint32_t)iw * h * 2);
        put16(tim, &n, (uint16_t)vram_x);
        put16(tim, &n, (uint16_t)vram_y);
        put16(tim, &n, iw);
        put16(tim, &n, (uint16_t)h);
        for (int i = 0; i < w * h; i++)
            tim[n++] = idx[i];
        free(idx);
    }
    free(px);

    int rc = sym ? emit_c(sym, out, tim, n) : write_file(out, tim, n);
    free(tim);
    return rc;
}
