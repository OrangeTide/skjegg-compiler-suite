/* enc.h : WebAssembly binary encoding primitives.

   Shared by the backend's relocatable-object sink and by skj-ld-wasm. A wbuf is
   a growable byte buffer; the rest are the WASM binary building blocks (LEB128,
   little-endian words, length-prefixed names, section framing) plus LEB readers
   for the linker. */

#ifndef WASM_ENC_H
#define WASM_ENC_H

#include <stddef.h>
#include <stdint.h>

struct wbuf {
    unsigned char *data;
    size_t         len;
    size_t         cap;
};

void wbuf_init(struct wbuf *b);
void wbuf_free(struct wbuf *b);
void wbuf_reset(struct wbuf *b);

void wbuf_byte(struct wbuf *b, unsigned char c);
void wbuf_bytes(struct wbuf *b, const void *p, size_t n);
void wbuf_u32(struct wbuf *b, uint32_t v);          /* raw little-endian, 4 bytes */

void wbuf_uleb(struct wbuf *b, uint64_t v);
void wbuf_sleb(struct wbuf *b, int64_t v);

/* a ULEB/SLEB padded to exactly `n` bytes: a relocatable placeholder the linker
   can overwrite in place without changing the code size */
void wbuf_uleb_fixed(struct wbuf *b, uint64_t v, int n);
void wbuf_sleb_fixed(struct wbuf *b, int64_t v, int n);

/* patch a fixed-width LEB already written at byte offset `off` */
void wbuf_patch_uleb_fixed(struct wbuf *b, size_t off, uint64_t v, int n);
void wbuf_patch_sleb_fixed(struct wbuf *b, size_t off, int64_t v, int n);

void wbuf_name(struct wbuf *b, const char *s);      /* uleb(len) then the bytes */

/* append a framed section to b: the id byte, uleb(len(payload)), then payload */
void wbuf_section(struct wbuf *b, int id, const struct wbuf *payload);

/* LEB readers: advance *off past the value in p[0..n) and return it */
uint64_t wasm_read_uleb(const unsigned char *p, size_t n, size_t *off);
int64_t  wasm_read_sleb(const unsigned char *p, size_t n, size_t *off);

#endif /* WASM_ENC_H */
