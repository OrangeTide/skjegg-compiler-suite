/* enc_test.c : round-trip self-test for the WASM encoding primitives.

   Host-only, no cross toolchain.  Encodes a spread of values with the ULEB and
   SLEB writers (variable and fixed width) and reads them back, checking each
   round-trips and that the fixed-width form uses exactly the requested size.
   Prints a one-line result and exits non-zero on the first failure. */

#include <stdio.h>
#include <stdint.h>

#include "enc.h"

static int failures;

static void
check_uleb(uint64_t v)
{
    struct wbuf b;
    size_t off = 0;
    uint64_t got;
    wbuf_init(&b);
    wbuf_uleb(&b, v);
    got = wasm_read_uleb(b.data, b.len, &off);
    if (got != v || off != b.len) {
        fprintf(stderr, "uleb %llu: got %llu, consumed %zu of %zu\n",
                (unsigned long long)v, (unsigned long long)got, off, b.len);
        failures++;
    }
    wbuf_free(&b);
}

static void
check_sleb(int64_t v)
{
    struct wbuf b;
    size_t off = 0;
    int64_t got;
    wbuf_init(&b);
    wbuf_sleb(&b, v);
    got = wasm_read_sleb(b.data, b.len, &off);
    if (got != v || off != b.len) {
        fprintf(stderr, "sleb %lld: got %lld, consumed %zu of %zu\n",
                (long long)v, (long long)got, off, b.len);
        failures++;
    }
    wbuf_free(&b);
}

static void
check_uleb_fixed(uint64_t v, int n)
{
    struct wbuf b;
    size_t off = 0;
    uint64_t got;
    wbuf_init(&b);
    wbuf_uleb_fixed(&b, v, n);
    if (b.len != (size_t)n) {
        fprintf(stderr, "uleb_fixed %llu/%d: wrote %zu bytes\n",
                (unsigned long long)v, n, b.len);
        failures++;
    }
    got = wasm_read_uleb(b.data, b.len, &off);
    if (got != v) {
        fprintf(stderr, "uleb_fixed %llu/%d: read back %llu\n",
                (unsigned long long)v, n, (unsigned long long)got);
        failures++;
    }
    /* patch to a new value, same width, and re-read */
    wbuf_patch_uleb_fixed(&b, 0, v ^ 0x2a, n);
    off = 0;
    got = wasm_read_uleb(b.data, b.len, &off);
    if (got != (v ^ 0x2a)) {
        fprintf(stderr, "uleb_fixed patch %llu/%d: read %llu\n",
                (unsigned long long)(v ^ 0x2a), n, (unsigned long long)got);
        failures++;
    }
    wbuf_free(&b);
}

static void
check_section(void)
{
    struct wbuf payload, out;
    size_t off = 0;
    uint64_t len;
    wbuf_init(&payload);
    wbuf_init(&out);
    wbuf_name(&payload, "linking");
    wbuf_byte(&payload, 0x02);
    wbuf_section(&out, 0, &payload);               /* custom section id 0 */
    if (out.data[0] != 0) { fprintf(stderr, "section id\n"); failures++; }
    off = 1;
    len = wasm_read_uleb(out.data, out.len, &off);
    if (len != payload.len || off + len != out.len) {
        fprintf(stderr, "section frame: len %llu payload %zu total %zu off %zu\n",
                (unsigned long long)len, payload.len, out.len, off);
        failures++;
    }
    wbuf_free(&payload);
    wbuf_free(&out);
}

int
main(void)
{
    uint64_t uv[] = { 0, 1, 63, 64, 127, 128, 255, 16383, 16384,
                      123456789, 0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFull };
    int64_t  sv[] = { 0, 1, -1, 63, -64, 64, -65, 127, -128, 128,
                      -12345, 2147483647, -2147483648LL, -1234567890123LL };
    size_t i;

    for (i = 0; i < sizeof(uv) / sizeof(uv[0]); i++)
        check_uleb(uv[i]);
    for (i = 0; i < sizeof(sv) / sizeof(sv[0]); i++)
        check_sleb(sv[i]);
    for (i = 0; i < sizeof(uv) / sizeof(uv[0]) && uv[i] <= 0xFFFFFFFFu; i++)
        check_uleb_fixed(uv[i], 5);
    check_section();

    if (failures == 0)
        printf("wasm-enc: all round-trip checks passed\n");
    else
        printf("wasm-enc: %d failure(s)\n", failures);
    return failures != 0;
}
