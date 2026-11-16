/* C glue for the optional `curl` unit: a thin binding to libcurl's easy
   interface for a simple HTTP (or file://) GET. Built only when ./configure found
   libcurl (HAVE_LIBCURL); compiled with the library's cflags and archived inside
   curl.a, with -lcurl in the curl.link sidecar. See
   doc/languages/kobold/kp-native-units.md.

   libcurl delivers a response body in chunks through a write callback, so the glue
   supplies one that accumulates the chunks into a growing buffer, then returns the
   whole body to Kobold as one string. A URL crosses as a Kobold string handle
   (kp_gptr), decoded with kp_str_bytes, and the body is built with kp_str_new, so
   the glue is compiled -DKP_FREESTANDING to match libkpaot's no-vm signature (see
   "Strings cross as a handle" in the doc). Kobold holds no libcurl handle: Get runs
   a whole transfer and the status of the last one is read back with LastStatus and
   LastOk. */
#include <curl/curl.h>

#include "str.h"        /* kp_gptr, kp_str_bytes / kp_str_new */

#include <stdlib.h>
#include <string.h>

/* A growing byte buffer the write callback appends to. */
struct kpcurl_buf {
    char *data;
    size_t len, cap;
};

static long g_last_status;      /* HTTP code of the last Get, 0 for file:// */
static int  g_last_ok;          /* the last transfer returned CURLE_OK */
static int  g_inited;           /* curl_global_init has run */

static size_t
accumulate(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct kpcurl_buf *b = userdata;
    size_t n = size * nmemb;

    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        char *grown;

        while (c < b->len + n + 1)
            c *= 2;
        grown = realloc(b->data, c);
        if (grown == NULL)
            return 0;           /* out of memory: abort the transfer */
        b->data = grown;
        b->cap = c;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* Fetch a URL and return its body as a new Kobold string; the empty string on a
   transfer failure. LastStatus and LastOk report the outcome. */
kp_gptr
kpcurl_get(kp_gptr url)
{
    CURL *c;
    CURLcode res;
    struct kpcurl_buf b = { NULL, 0, 0 };
    kp_gptr out;

    g_last_status = 0;
    g_last_ok = 0;
    if (!g_inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_inited = 1;
    }
    c = curl_easy_init();
    if (c == NULL)
        return kp_str_new("", 0);
    curl_easy_setopt(c, CURLOPT_URL, kp_str_bytes(url));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, accumulate);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "skjegg-kobold/curl");
    res = curl_easy_perform(c);
    g_last_ok = (res == CURLE_OK);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &g_last_status);
    curl_easy_cleanup(c);
    if (g_last_ok && b.data != NULL)
        out = kp_str_new(b.data, (int)b.len);
    else
        out = kp_str_new("", 0);
    free(b.data);
    return out;
}

int
kpcurl_last_status(void)
{
    return (int)g_last_status;
}

int
kpcurl_last_ok(void)
{
    return g_last_ok;
}
