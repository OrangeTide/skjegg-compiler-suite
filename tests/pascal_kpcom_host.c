/* Host import shims for kpcom built by skj-pc with KOBOLD defined.  With
   KOBOLD its file I/O is `external cdecl`, so the acceptance test links these
   host functions in place of the Kobold runtime's own.  A Pascal string is a
   managed descriptor (refcount, kind, len, cap, bytes); NULL is the empty
   string.  The directory helpers are stubbed: the acceptance test emits one
   module at a time and never scans a directory. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSTR_HDR 16

void *__pascal_str_new(const char *b, int len);

static int pstr_len(const void *s)
{
    if (!s) return 0;
    return *(const int *)((const char *)s + 8);
}

static const char *pstr_bytes(const void *s)
{
    if (!s) return "";
    return (const char *)s + PSTR_HDR;
}

void *ReadFileStr(const void *path)
{
    char p[4096];
    int n = pstr_len(path);
    FILE *f;
    long sz;
    char *buf;
    void *r;
    if (n >= (int)sizeof(p)) n = sizeof(p) - 1;
    memcpy(p, pstr_bytes(path), n);
    p[n] = '\0';
    f = fopen(p, "rb");
    if (!f) return __pascal_str_new("", 0);
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(sz > 0 ? sz : 1);
    if (sz > 0) sz = fread(buf, 1, sz, f);
    fclose(f);
    r = __pascal_str_new(buf, (int)sz);
    free(buf);
    return r;
}

int WriteFileStr(const void *path, const void *data)
{
    char p[4096];
    int n = pstr_len(path);
    FILE *f;
    size_t w;
    if (n >= (int)sizeof(p)) n = sizeof(p) - 1;
    memcpy(p, pstr_bytes(path), n);
    p[n] = '\0';
    f = fopen(p, "wb");
    if (!f) return 0;
    w = fwrite(pstr_bytes(data), 1, pstr_len(data), f);
    fclose(f);
    return (int)w == pstr_len(data);
}

int kpd_open(const void *path) { (void)path; return -1; }
void *kpd_next(int handle) { (void)handle; return __pascal_str_new("", 0); }
void kpd_close(int handle) { (void)handle; }
