/* enc.c : WebAssembly binary encoding primitives (see enc.h). */

#include <stdlib.h>
#include <string.h>

#include "enc.h"

void
wbuf_init(struct wbuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void
wbuf_free(struct wbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void
wbuf_reset(struct wbuf *b)
{
    b->len = 0;
}

static void
wbuf_grow(struct wbuf *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return;
    {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + extra)
            cap *= 2;
        b->data = realloc(b->data, cap);
        if (!b->data) {
            /* enc.c has no access to the toolkit's die(); a failed grow here is
               fatal and unrecoverable, so abort rather than return a short buf */
            abort();
        }
        b->cap = cap;
    }
}

void
wbuf_byte(struct wbuf *b, unsigned char c)
{
    wbuf_grow(b, 1);
    b->data[b->len++] = c;
}

void
wbuf_bytes(struct wbuf *b, const void *p, size_t n)
{
    if (n == 0)
        return;
    wbuf_grow(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void
wbuf_u32(struct wbuf *b, uint32_t v)
{
    wbuf_byte(b, (unsigned char)(v & 0xff));
    wbuf_byte(b, (unsigned char)((v >> 8) & 0xff));
    wbuf_byte(b, (unsigned char)((v >> 16) & 0xff));
    wbuf_byte(b, (unsigned char)((v >> 24) & 0xff));
}

void
wbuf_uleb(struct wbuf *b, uint64_t v)
{
    do {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v != 0)
            byte |= 0x80;
        wbuf_byte(b, byte);
    } while (v != 0);
}

void
wbuf_sleb(struct wbuf *b, int64_t v)
{
    int more = 1;
    while (more) {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;                                    /* arithmetic shift */
        if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))
            more = 0;
        else
            byte |= 0x80;
        wbuf_byte(b, byte);
    }
}

void
wbuf_uleb_fixed(struct wbuf *b, uint64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (i < n - 1)
            byte |= 0x80;
        wbuf_byte(b, byte);
    }
}

void
wbuf_sleb_fixed(struct wbuf *b, int64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;                                    /* arithmetic shift */
        if (i < n - 1)
            byte |= 0x80;
        wbuf_byte(b, byte);
    }
}

void
wbuf_patch_uleb_fixed(struct wbuf *b, size_t off, uint64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (i < n - 1)
            byte |= 0x80;
        b->data[off + i] = byte;
    }
}

void
wbuf_patch_sleb_fixed(struct wbuf *b, size_t off, int64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (i < n - 1)
            byte |= 0x80;
        b->data[off + i] = byte;
    }
}

void
wbuf_name(struct wbuf *b, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    wbuf_uleb(b, n);
    wbuf_bytes(b, s, n);
}

void
wbuf_section(struct wbuf *b, int id, const struct wbuf *payload)
{
    wbuf_byte(b, (unsigned char)id);
    wbuf_uleb(b, payload->len);
    wbuf_bytes(b, payload->data, payload->len);
}

uint64_t
wasm_read_uleb(const unsigned char *p, size_t n, size_t *off)
{
    uint64_t result = 0;
    int shift = 0;
    while (*off < n) {
        unsigned char byte = p[(*off)++];
        result |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            break;
        shift += 7;
    }
    return result;
}

int64_t
wasm_read_sleb(const unsigned char *p, size_t n, size_t *off)
{
    int64_t result = 0;
    int shift = 0;
    unsigned char byte = 0;
    while (*off < n) {
        byte = p[(*off)++];
        result |= (int64_t)(byte & 0x7f) << shift;
        shift += 7;
        if (!(byte & 0x80))
            break;
    }
    if (shift < 64 && (byte & 0x40))
        result |= -((int64_t)1 << shift);           /* sign extend */
    return result;
}
