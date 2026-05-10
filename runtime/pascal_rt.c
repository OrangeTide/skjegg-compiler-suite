/* pascal_rt.c : runtime helpers for Compact Pascal (cross-compiled) */

extern int read(int fd, char *buf, int n);
extern int write(int fd, const char *buf, int n);

void
__pascal_write_int(int v)
{
    char buf[12];
    int i, neg;
    unsigned int u;

    if (v == 0) {
        write(1, "0", 1);
        return;
    }

    neg = 0;
    if (v < 0) {
        neg = 1;
        u = (unsigned int)(-(v + 1)) + 1;
    } else {
        u = (unsigned int)v;
    }

    i = sizeof(buf);
    while (u > 0) {
        buf[--i] = '0' + (u % 10);
        u /= 10;
    }
    if (neg)
        buf[--i] = '-';
    write(1, buf + i, sizeof(buf) - i);
}

void
__pascal_write_char(int v)
{
    char c = (char)v;
    write(1, &c, 1);
}

void
__pascal_write_str(char *s)
{
    int len = (unsigned char)s[0];

    if (len > 0)
        write(1, s + 1, len);
}

void
__pascal_str_assign(char *dst, char *src, int maxlen)
{
    int srclen = (unsigned char)src[0];
    int copylen = srclen < maxlen ? srclen : maxlen;
    int i;

    dst[0] = (char)copylen;
    for (i = 1; i <= copylen; i++)
        dst[i] = src[i];
}

int
__pascal_str_compare(char *a, char *b)
{
    int alen = (unsigned char)a[0];
    int blen = (unsigned char)b[0];
    int minlen = alen < blen ? alen : blen;
    int i;

    for (i = 1; i <= minlen; i++) {
        if ((unsigned char)a[i] != (unsigned char)b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return alen - blen;
}

void
__pascal_str_concat(char *dst, char *a, char *b, int maxlen)
{
    int alen = (unsigned char)a[0];
    int blen = (unsigned char)b[0];
    int total = alen + blen;
    int i;

    if (total > maxlen)
        total = maxlen;
    dst[0] = (char)total;
    for (i = 1; i <= alen && i <= total; i++)
        dst[i] = a[i];
    for (int j = 1; j <= blen && i <= total; j++, i++)
        dst[i] = b[j];
}

void
__pascal_str_copy(char *dst, char *src, int index, int count, int maxlen)
{
    int srclen = (unsigned char)src[0];
    int start = index - 1;
    int avail, copylen, i;

    if (start < 0)
        start = 0;
    if (start >= srclen) {
        dst[0] = 0;
        return;
    }
    avail = srclen - start;
    copylen = count < avail ? count : avail;
    if (copylen > maxlen)
        copylen = maxlen;
    dst[0] = (char)copylen;
    for (i = 0; i < copylen; i++)
        dst[i + 1] = src[start + i + 1];
}

int
__pascal_str_pos(char *needle, char *haystack)
{
    int nlen = (unsigned char)needle[0];
    int hlen = (unsigned char)haystack[0];
    int i, j;

    if (nlen == 0)
        return 0;
    for (i = 1; i <= hlen - nlen + 1; i++) {
        for (j = 0; j < nlen; j++) {
            if (haystack[i + j] != needle[j + 1])
                break;
        }
        if (j == nlen)
            return i;
    }
    return 0;
}

void
__pascal_str_delete(char *s, int index, int count)
{
    int slen = (unsigned char)s[0];
    int start = index - 1;
    int tail, i;

    if (start < 0 || start >= slen) return;
    if (start + count > slen)
        count = slen - start;
    tail = slen - start - count;
    for (i = 0; i < tail; i++)
        s[start + i + 1] = s[start + count + i + 1];
    s[0] = (char)(slen - count);
}

void
__pascal_str_insert(char *src, char *dst, int index, int maxlen)
{
    int srclen = (unsigned char)src[0];
    int dstlen = (unsigned char)dst[0];
    int pos = index - 1;
    int newlen, tail, i;

    if (pos < 0) pos = 0;
    if (pos > dstlen) pos = dstlen;
    newlen = dstlen + srclen;
    if (newlen > maxlen)
        newlen = maxlen;
    tail = dstlen - pos;
    if (pos + srclen + tail > newlen)
        tail = newlen - pos - srclen;
    if (tail < 0) tail = 0;
    for (i = tail - 1; i >= 0; i--)
        dst[pos + srclen + i + 1] = dst[pos + i + 1];
    for (i = 0; i < srclen && pos + i < newlen; i++)
        dst[pos + i + 1] = src[i + 1];
    dst[0] = (char)newlen;
}

void
__pascal_str_from_int(int v, char *dst, int maxlen)
{
    char buf[12];
    int i, neg, len;
    unsigned int u;

    if (v == 0) {
        buf[0] = '0';
        len = 1;
    } else {
        neg = 0;
        if (v < 0) {
            neg = 1;
            u = (unsigned int)(-(v + 1)) + 1;
        } else {
            u = (unsigned int)v;
        }
        i = sizeof(buf);
        while (u > 0) {
            buf[--i] = '0' + (u % 10);
            u /= 10;
        }
        if (neg)
            buf[--i] = '-';
        len = (int)sizeof(buf) - i;
        for (int j = 0; j < len; j++)
            buf[j] = buf[i + j];
    }
    if (len > maxlen)
        len = maxlen;
    dst[0] = (char)len;
    for (i = 0; i < len; i++)
        dst[i + 1] = buf[i];
}

static char rd_buf[256];
static int rd_pos;
static int rd_len;
static int rd_eof;

static int
rd_getc(void)
{
    if (rd_eof)
        return -1;
    if (rd_pos >= rd_len) {
        rd_len = read(0, rd_buf, sizeof(rd_buf));
        rd_pos = 0;
        if (rd_len <= 0) {
            rd_eof = 1;
            return -1;
        }
    }
    return (unsigned char)rd_buf[rd_pos++];
}

void
__pascal_read_char(int *dst)
{
    int c = rd_getc();

    *dst = (c >= 0) ? c : 0;
}

int
__pascal_eof(void)
{
    return rd_eof;
}

void
__pascal_read_int(int *dst)
{
    int c, neg;
    unsigned int v;

    /* skip whitespace (not newlines for read vs readln distinction) */
    for (;;) {
        c = rd_getc();
        if (c < 0) {
            *dst = 0;
            return;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
    }

    neg = 0;
    if (c == '-') {
        neg = 1;
        c = rd_getc();
    } else if (c == '+') {
        c = rd_getc();
    }

    v = 0;
    while (c >= '0' && c <= '9') {
        v = v * 10 + (unsigned int)(c - '0');
        c = rd_getc();
    }

    /* push back the non-digit character */
    if (c >= 0)
        rd_pos--;

    *dst = neg ? -(int)v : (int)v;
}

void
__pascal_read_str(char *dst, int maxlen)
{
    int c, len;

    len = 0;
    for (;;) {
        c = rd_getc();
        if (c < 0 || c == '\n')
            break;
        if (len < maxlen)
            dst[1 + len++] = (char)c;
    }
    dst[0] = (char)len;
}

void
__pascal_readln(void)
{
    int c;

    for (;;) {
        c = rd_getc();
        if (c < 0 || c == '\n')
            return;
    }
}

/****************************************************************
 * Set operations (256-bit sets, 32 bytes)
 ****************************************************************/

void
__pascal_set_clear(char *s)
{
    for (int i = 0; i < 32; i++)
        s[i] = 0;
}

void
__pascal_set_add(char *s, int elem)
{
    if (elem >= 0 && elem <= 255)
        s[elem >> 3] |= (1 << (elem & 7));
}

void
__pascal_set_addrange(char *s, int lo, int hi)
{
    for (int i = lo; i <= hi; i++)
        if (i >= 0 && i <= 255)
            s[i >> 3] |= (1 << (i & 7));
}

void
__pascal_set_union(char *dst, char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        dst[i] = a[i] | b[i];
}

void
__pascal_set_intersect(char *dst, char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        dst[i] = a[i] & b[i];
}

void
__pascal_set_diff(char *dst, char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        dst[i] = a[i] & ~b[i];
}

int
__pascal_set_in(int elem, char *s)
{
    if (elem < 0 || elem > 255)
        return 0;
    return (s[elem >> 3] >> (elem & 7)) & 1;
}

int
__pascal_set_eq(char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int
__pascal_set_ne(char *a, char *b)
{
    return !__pascal_set_eq(a, b);
}

int
__pascal_set_subset(char *a, char *b)
{
    for (int i = 0; i < 32; i++)
        if (a[i] & ~b[i])
            return 0;
    return 1;
}

void
__pascal_set_copy(char *dst, char *src)
{
    for (int i = 0; i < 32; i++)
        dst[i] = src[i];
}

void
__pascal_val(char *s, int *result, int *code)
{
    int len = (unsigned char)s[0];
    int i = 1;
    int neg = 0;
    int val = 0;

    while (i <= len && s[i] == ' ')
        i++;
    if (i <= len && s[i] == '-') {
        neg = 1;
        i++;
    } else if (i <= len && s[i] == '+') {
        i++;
    }
    if (i > len || s[i] < '0' || s[i] > '9') {
        *result = 0;
        *code = i;
        return;
    }
    while (i <= len && s[i] >= '0' && s[i] <= '9') {
        val = val * 10 + (s[i] - '0');
        i++;
    }
    if (i <= len) {
        *result = 0;
        *code = i;
        return;
    }
    *result = neg ? -val : val;
    *code = 0;
}

void *
memset(void *s, int c, unsigned int n)
{
    unsigned char *p = s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void *
memmove(void *dst, const void *src, unsigned int n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}
