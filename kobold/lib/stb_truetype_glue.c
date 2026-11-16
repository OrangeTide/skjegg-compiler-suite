/* C glue for the optional `stb_truetype` unit: a thin binding to stb_truetype.h,
   the public-domain single-header TrueType font loader and rasterizer vendored at
   third_party/stb_truetype.h. Compiled with STB_TRUETYPE_IMPLEMENTATION so the one
   definition lives here, and archived inside stb_truetype.a; the -lm the
   rasterizer needs rides in the stb_truetype.link sidecar. See
   doc/languages/kobold/kp-native-units.md.

   Two kinds of handle, both integer ids into bounded tables, since Kobold holds no
   pointers. A font keeps the whole file's bytes alive (stbtt_fontinfo points into
   them) plus the parsed info. A rendered glyph keeps the 8-bit coverage bitmap stb
   rasterized, with its size and the offset of its top-left from the pen origin.

   A path is a Kobold `string`, which crosses as a guest string handle (kp_gptr),
   decoded with kp_str_bytes; the glue is compiled -DKP_FREESTANDING to match
   libkpaot's no-vm signature (see "Strings cross as a handle" in the doc). */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "str.h"        /* kp_gptr, kp_str_bytes: the guest-string read side */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define KPTTF_MAX_FONT  16
#define KPTTF_MAX_GLYPH 64

struct kpttf_font {
    unsigned char *data;        /* the file bytes, kept alive for info's lifetime */
    stbtt_fontinfo info;
    int loaded;                 /* 0 for a free slot */
};

struct kpttf_glyph {
    unsigned char *bitmap;      /* the 8-bit coverage, or NULL for a free slot */
    int w, h, xoff, yoff;
};

static struct kpttf_font  g_font[KPTTF_MAX_FONT];
static struct kpttf_glyph g_glyph[KPTTF_MAX_GLYPH];

static struct kpttf_font *
font_of(int id)
{
    if (id >= 1 && id <= KPTTF_MAX_FONT && g_font[id - 1].loaded)
        return &g_font[id - 1];
    return NULL;
}

static struct kpttf_glyph *
glyph_of(int id)
{
    if (id >= 1 && id <= KPTTF_MAX_GLYPH && g_glyph[id - 1].bitmap != NULL)
        return &g_glyph[id - 1];
    return NULL;
}

/* Read a whole file into a freshly allocated buffer; NULL on any failure. */
static unsigned char *
read_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (buf == NULL) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = n;
    return buf;
}

int
kpttf_load(kp_gptr path)
{
    const char *cpath = kp_str_bytes(path);
    unsigned char *data;
    long len;
    int i, off;

    for (i = 0; i < KPTTF_MAX_FONT; i++)
        if (!g_font[i].loaded)
            break;
    if (i == KPTTF_MAX_FONT)
        return 0;                       /* table full */
    data = read_file(cpath, &len);
    if (data == NULL)
        return 0;
    off = stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0 || !stbtt_InitFont(&g_font[i].info, data, off)) {
        free(data);
        return 0;                       /* not a usable font */
    }
    g_font[i].data = data;
    g_font[i].loaded = 1;
    return i + 1;                       /* 1-based id */
}

void
kpttf_free_font(int id)
{
    struct kpttf_font *fn = font_of(id);

    if (fn) {
        free(fn->data);
        fn->data = NULL;
        fn->loaded = 0;
    }
}

/* The scale that maps font units to a cell `pixel_height` pixels tall. */
static float
scale_for(struct kpttf_font *fn, int pixel_height)
{
    return stbtt_ScaleForPixelHeight(&fn->info, (float)pixel_height);
}

static int
round_i(float v)
{
    return (int)lroundf(v);
}

/* Vertical metrics, scaled to `pixel_height` and rounded to whole pixels: the
   ascent above the baseline (positive), the descent below it (negative), and the
   gap between lines. 0 for an invalid id. */
int
kpttf_ascent(int id, int pixel_height)
{
    struct kpttf_font *fn = font_of(id);
    int a, d, g;

    if (fn == NULL)
        return 0;
    stbtt_GetFontVMetrics(&fn->info, &a, &d, &g);
    return round_i((float)a * scale_for(fn, pixel_height));
}

int
kpttf_descent(int id, int pixel_height)
{
    struct kpttf_font *fn = font_of(id);
    int a, d, g;

    if (fn == NULL)
        return 0;
    stbtt_GetFontVMetrics(&fn->info, &a, &d, &g);
    return round_i((float)d * scale_for(fn, pixel_height));
}

int
kpttf_line_gap(int id, int pixel_height)
{
    struct kpttf_font *fn = font_of(id);
    int a, d, g;

    if (fn == NULL)
        return 0;
    stbtt_GetFontVMetrics(&fn->info, &a, &d, &g);
    return round_i((float)g * scale_for(fn, pixel_height));
}

/* The horizontal advance of a code point, scaled to `pixel_height` and rounded.
   0 for an invalid id. */
int
kpttf_advance(int id, int cp, int pixel_height)
{
    struct kpttf_font *fn = font_of(id);
    int adv, lsb;

    if (fn == NULL)
        return 0;
    stbtt_GetCodepointHMetrics(&fn->info, cp, &adv, &lsb);
    return round_i((float)adv * scale_for(fn, pixel_height));
}

/* Rasterize a code point at `pixel_height` into a new glyph handle; 0 on a full
   table or a code point with no outline (an empty bitmap, e.g. the space). */
int
kpttf_render(int id, int cp, int pixel_height)
{
    struct kpttf_font *fn = font_of(id);
    unsigned char *bm;
    float scale;
    int i, w, h, xoff, yoff;

    if (fn == NULL)
        return 0;
    for (i = 0; i < KPTTF_MAX_GLYPH; i++)
        if (g_glyph[i].bitmap == NULL)
            break;
    if (i == KPTTF_MAX_GLYPH)
        return 0;
    scale = scale_for(fn, pixel_height);
    bm = stbtt_GetCodepointBitmap(&fn->info, 0, scale, cp, &w, &h, &xoff, &yoff);
    if (bm == NULL || w <= 0 || h <= 0) {
        stbtt_FreeBitmap(bm, NULL);     /* NULL is a safe no-op */
        return 0;
    }
    g_glyph[i].bitmap = bm;
    g_glyph[i].w = w;
    g_glyph[i].h = h;
    g_glyph[i].xoff = xoff;
    g_glyph[i].yoff = yoff;
    return i + 1;
}

int
kpttf_glyph_width(int gid)
{
    struct kpttf_glyph *gl = glyph_of(gid);

    return gl ? gl->w : 0;
}

int
kpttf_glyph_height(int gid)
{
    struct kpttf_glyph *gl = glyph_of(gid);

    return gl ? gl->h : 0;
}

int
kpttf_glyph_xoff(int gid)
{
    struct kpttf_glyph *gl = glyph_of(gid);

    return gl ? gl->xoff : 0;
}

int
kpttf_glyph_yoff(int gid)
{
    struct kpttf_glyph *gl = glyph_of(gid);

    return gl ? gl->yoff : 0;
}

/* The coverage (0..255) at pixel (x, y) of the glyph bitmap; 0 for an invalid id
   or an out-of-range coordinate. */
int
kpttf_glyph_coverage(int gid, int x, int y)
{
    struct kpttf_glyph *gl = glyph_of(gid);

    if (gl == NULL || x < 0 || x >= gl->w || y < 0 || y >= gl->h)
        return 0;
    return gl->bitmap[y * gl->w + x];
}

void
kpttf_free_glyph(int gid)
{
    struct kpttf_glyph *gl = glyph_of(gid);

    if (gl) {
        stbtt_FreeBitmap(gl->bitmap, NULL);
        gl->bitmap = NULL;
    }
}
