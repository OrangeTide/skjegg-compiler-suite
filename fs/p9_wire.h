/* 9P2000 wire codec: little-endian build and parse cursors, shared by the
 * ninefs client and the host test server.  Header-only and freestanding-safe
 * (no libc beyond string.h); every field is assembled byte by byte so the
 * encoding is identical on any host byte order. */

#ifndef P9_WIRE_H
#define P9_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ninefs.h"      /* struct p9_qid, the message-type and flag constants */

/* Build cursor.  `ok` latches 0 on overflow; a caller checks it once at finish
 * rather than after every field. */
struct p9_out {
    uint8_t *b;
    uint32_t cap;
    uint32_t n;
    int      ok;
};

static inline void
p9o_init(struct p9_out *o, uint8_t *b, uint32_t cap)
{
    o->b = b; o->cap = cap; o->n = 0; o->ok = 1;
}

static inline void
p9o_u8(struct p9_out *o, uint8_t v)
{
    if (o->n + 1 > o->cap) { o->ok = 0; return; }
    o->b[o->n++] = v;
}

static inline void
p9o_u16(struct p9_out *o, uint16_t v)
{
    if (o->n + 2 > o->cap) { o->ok = 0; return; }
    o->b[o->n++] = (uint8_t)(v & 0xff);
    o->b[o->n++] = (uint8_t)(v >> 8);
}

static inline void
p9o_u32(struct p9_out *o, uint32_t v)
{
    if (o->n + 4 > o->cap) { o->ok = 0; return; }
    o->b[o->n++] = (uint8_t)(v & 0xff);
    o->b[o->n++] = (uint8_t)((v >> 8) & 0xff);
    o->b[o->n++] = (uint8_t)((v >> 16) & 0xff);
    o->b[o->n++] = (uint8_t)((v >> 24) & 0xff);
}

static inline void
p9o_u64(struct p9_out *o, uint64_t v)
{
    p9o_u32(o, (uint32_t)(v & 0xffffffffu));
    p9o_u32(o, (uint32_t)(v >> 32));
}

static inline void
p9o_bytes(struct p9_out *o, const void *p, uint32_t len)
{
    if (o->n + len > o->cap) { o->ok = 0; return; }
    memcpy(o->b + o->n, p, len);
    o->n += len;
}

/* A 9P string: a 2-byte length then the bytes, no NUL on the wire. */
static inline void
p9o_str(struct p9_out *o, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    if (len > 0xffff) { o->ok = 0; return; }
    p9o_u16(o, (uint16_t)len);
    p9o_bytes(o, s, (uint32_t)len);
}

static inline void
p9o_qid(struct p9_out *o, const struct p9_qid *q)
{
    p9o_u8(o, q->type);
    p9o_u32(o, q->version);
    p9o_u64(o, q->path);
}

/* Write the message header with a placeholder size, to be patched at finish. */
static inline void
p9o_hdr(struct p9_out *o, uint8_t type, uint16_t tag)
{
    p9o_u32(o, 0);                  /* size placeholder */
    p9o_u8(o, type);
    p9o_u16(o, tag);
}

/* Patch size[4] at the front and return the framed length, or 0 on overflow. */
static inline uint32_t
p9o_finish(struct p9_out *o)
{
    uint32_t n;
    if (!o->ok)
        return 0;
    n = o->n;
    o->b[0] = (uint8_t)(n & 0xff);
    o->b[1] = (uint8_t)((n >> 8) & 0xff);
    o->b[2] = (uint8_t)((n >> 16) & 0xff);
    o->b[3] = (uint8_t)((n >> 24) & 0xff);
    return n;
}

/* Parse cursor.  Every read checks bounds and latches `ok` to 0 past the end;
 * a short read then yields zeroes, so a caller validates `ok` before trusting
 * the decoded values. */
struct p9_in {
    const uint8_t *b;
    uint32_t       len;
    uint32_t       p;
    int            ok;
};

static inline void
p9i_init(struct p9_in *in, const uint8_t *b, uint32_t len)
{
    in->b = b; in->len = len; in->p = 0; in->ok = 1;
}

static inline uint8_t
p9i_u8(struct p9_in *in)
{
    if (in->p + 1 > in->len) { in->ok = 0; return 0; }
    return in->b[in->p++];
}

static inline uint16_t
p9i_u16(struct p9_in *in)
{
    uint16_t v;
    if (in->p + 2 > in->len) { in->ok = 0; return 0; }
    v = (uint16_t)in->b[in->p] | ((uint16_t)in->b[in->p + 1] << 8);
    in->p += 2;
    return v;
}

static inline uint32_t
p9i_u32(struct p9_in *in)
{
    uint32_t v;
    if (in->p + 4 > in->len) { in->ok = 0; return 0; }
    v = (uint32_t)in->b[in->p] | ((uint32_t)in->b[in->p + 1] << 8)
      | ((uint32_t)in->b[in->p + 2] << 16) | ((uint32_t)in->b[in->p + 3] << 24);
    in->p += 4;
    return v;
}

static inline uint64_t
p9i_u64(struct p9_in *in)
{
    uint64_t lo = p9i_u32(in);
    uint64_t hi = p9i_u32(in);
    return lo | (hi << 32);
}

/* Copy a 9P string into `dst` (NUL-terminated, truncated to cap) and return its
 * on-wire byte length.  dst may be NULL to skip the field. */
static inline uint32_t
p9i_str(struct p9_in *in, char *dst, uint32_t cap)
{
    uint32_t len = p9i_u16(in);
    uint32_t i;
    if (in->p + len > in->len) { in->ok = 0; return 0; }
    for (i = 0; i < len; i++) {
        if (dst && i + 1 < cap)
            dst[i] = (char)in->b[in->p + i];
    }
    if (dst && cap > 0)
        dst[len < cap ? len : cap - 1] = 0;
    in->p += len;
    return len;
}

static inline void
p9i_qid(struct p9_in *in, struct p9_qid *q)
{
    q->type = p9i_u8(in);
    q->version = p9i_u32(in);
    q->path = p9i_u64(in);
}

static inline void
p9i_skip(struct p9_in *in, uint32_t n)
{
    if (in->p + n > in->len) { in->ok = 0; return; }
    in->p += n;
}

#endif /* P9_WIRE_H */
