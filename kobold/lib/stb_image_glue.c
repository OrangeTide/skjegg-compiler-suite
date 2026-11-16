/* C glue for the optional `stb_image` unit: a thin binding to stb_image.h, the
   public-domain single-header image loader vendored at third_party/stb_image.h.
   Compiled with STB_IMAGE_IMPLEMENTATION so the one definition lives here, and
   archived inside stb_image.a; the -lm link flag rides in the stb_image.link
   sidecar (stb's decoders use the math library). See
   doc/languages/kobold/kp-native-units.md.

   A decoded image is a heap buffer of pixels that stb returns as a pointer.
   Kobold holds no pointers, so an image is a small integer id into a bounded
   table that keeps the buffer and its dimensions; Load returns the id and the
   accessors read through it.

   A Kobold `string` crosses the FFI as a guest string handle (kp_gptr), not a C
   `char *`: the glue turns it into a NUL-terminated path with kp_str_bytes, the
   read side the runtime already exposes (str.h). The handle runtime resolves from
   libkpaot.a, so the glue is compiled with -DKP_FREESTANDING to match that build's
   no-vm signature. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "str.h"        /* kp_gptr, kp_str_bytes: the guest-string read side */

#define KPSTBI_MAX_IMAGE 32

struct kpstbi_image {
    stbi_uc *data;      /* the decoded pixels, or NULL for a free slot */
    int w, h, ch;       /* width, height, and stored channels per pixel */
};

static struct kpstbi_image g_image[KPSTBI_MAX_IMAGE];

static struct kpstbi_image *
image_of(int id)
{
    if (id >= 1 && id <= KPSTBI_MAX_IMAGE && g_image[id - 1].data != NULL)
        return &g_image[id - 1];
    return NULL;
}

/* Decode the file at `path` into a new table slot, requesting `want` channels
   (0 keeps the file's own channel count).  Returns a 1-based id, or 0 on a full
   table or a decode failure. */
static int
load_want(const char *path, int want)
{
    int i, w, h, file_ch;
    stbi_uc *data;

    for (i = 0; i < KPSTBI_MAX_IMAGE; i++)
        if (g_image[i].data == NULL)
            break;
    if (i == KPSTBI_MAX_IMAGE)
        return 0;                       /* table full */
    data = stbi_load(path, &w, &h, &file_ch, want);
    if (data == NULL)
        return 0;                       /* decode failed */
    g_image[i].data = data;
    g_image[i].w = w;
    g_image[i].h = h;
    g_image[i].ch = want ? want : file_ch;   /* the stride stb actually wrote */
    return i + 1;
}

int
kpstbi_load(kp_gptr path)
{
    return load_want(kp_str_bytes(path), 0);
}

int
kpstbi_load_rgba(kp_gptr path)
{
    return load_want(kp_str_bytes(path), 4);
}

int
kpstbi_width(int id)
{
    struct kpstbi_image *im = image_of(id);

    return im ? im->w : 0;
}

int
kpstbi_height(int id)
{
    struct kpstbi_image *im = image_of(id);

    return im ? im->h : 0;
}

int
kpstbi_channels(int id)
{
    struct kpstbi_image *im = image_of(id);

    return im ? im->ch : 0;
}

/* One channel byte (0..255) at pixel (x, y); 0 for an invalid id, an out-of-range
   coordinate, or a channel beyond the stored count. */
int
kpstbi_pixel(int id, int x, int y, int c)
{
    struct kpstbi_image *im = image_of(id);

    if (im == NULL || x < 0 || x >= im->w || y < 0 || y >= im->h ||
        c < 0 || c >= im->ch)
        return 0;
    return im->data[(y * im->w + x) * im->ch + c];
}

void
kpstbi_free(int id)
{
    struct kpstbi_image *im = image_of(id);

    if (im) {
        stbi_image_free(im->data);
        im->data = NULL;
    }
}
