#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include "fmt.h"

/* Buffered streams over the write and read seam (the crt's write/read).  Each
   FILE embeds its buffer (see stdio.h).  stdout is line buffered, stderr
   unbuffered, stdin read in BUFSIZ chunks.  fopen of a real file is deferred to
   a host that offers a file power (doc/tools/libc.md Phase C), so only the three
   standard streams exist.

   Note: the crt's normal exit (returning from main) does not flush, so a
   program that leaves output in a buffer should fflush before it returns; a
   flushing exit path is a later phase.  Line buffering means output ending in
   a newline is already flushed. */

extern int write(int fd, const void *buf, int n);
extern int read(int fd, void *buf, int n);

#define F_READ  1
#define F_WRITE 2
#define F_EOF   4
#define F_ERR   8

FILE __libc_stdin  = { 0, F_READ,  _IOLBF, 0, 0 };
FILE __libc_stdout = { 1, F_WRITE, _IOLBF, 0, 0 };
FILE __libc_stderr = { 2, F_WRITE, _IONBF, 0, 0 };

int
fflush(FILE *f)
{
    int off;

    if (f == NULL) {                    /* flush every stream we know of */
        int r = 0;
        if (fflush(stdout) == EOF)
            r = EOF;
        if (fflush(stderr) == EOF)
            r = EOF;
        return r;
    }
    if (!(f->flags & F_WRITE) || f->pos == 0)
        return 0;
    for (off = 0; off < f->pos; ) {
        int w = write(f->fd, f->buf + off, f->pos - off);
        if (w <= 0) {
            f->flags |= F_ERR;
            return EOF;
        }
        off += w;
    }
    f->pos = 0;
    return 0;
}

int
fputc(int c, FILE *f)
{
    if (!(f->flags & F_WRITE)) {
        f->flags |= F_ERR;
        return EOF;
    }
    f->buf[f->pos++] = (unsigned char)c;
    if (f->pos == BUFSIZ || f->mode == _IONBF ||
        (f->mode == _IOLBF && c == '\n'))
        if (fflush(f) == EOF)
            return EOF;
    return (unsigned char)c;
}

int
putc(int c, FILE *f)
{
    return fputc(c, f);
}

int
putchar(int c)
{
    return fputc(c, stdout);
}

int
fputs(const char *s, FILE *f)
{
    while (*s)
        if (fputc((unsigned char)*s++, f) == EOF)
            return EOF;
    return 0;
}

int
puts(const char *s)
{
    if (fputs(s, stdout) == EOF)
        return EOF;
    if (fputc('\n', stdout) == EOF)
        return EOF;
    return 0;
}

size_t
fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    const unsigned char *p = ptr;
    size_t total = size * nmemb;
    size_t i;

    if (size == 0 || nmemb == 0)
        return 0;
    for (i = 0; i < total; i++)
        if (fputc(p[i], f) == EOF)
            break;
    return i / size;                    /* whole items written */
}

/* Refill an input stream's buffer.  Returns 0, or EOF at end of input. */
static int
refill(FILE *f)
{
    int n = read(f->fd, f->buf, BUFSIZ);
    if (n <= 0) {
        f->flags |= (n == 0) ? F_EOF : F_ERR;
        f->pos = f->len = 0;
        return EOF;
    }
    f->len = n;
    f->pos = 0;
    return 0;
}

int
fgetc(FILE *f)
{
    if (!(f->flags & F_READ)) {
        f->flags |= F_ERR;
        return EOF;
    }
    if (f->pos >= f->len)
        if (refill(f) == EOF)
            return EOF;
    return f->buf[f->pos++];
}

int
getc(FILE *f)
{
    return fgetc(f);
}

int
getchar(void)
{
    return fgetc(stdin);
}

char *
fgets(char *s, int size, FILE *f)
{
    int i = 0;

    if (size <= 0)
        return NULL;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;                    /* end of input with nothing read */
    s[i] = '\0';
    return s;
}

size_t
fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    unsigned char *p = ptr;
    size_t total = size * nmemb;
    size_t i;

    if (size == 0 || nmemb == 0)
        return 0;
    for (i = 0; i < total; i++) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        p[i] = (unsigned char)c;
    }
    return i / size;                    /* whole items read */
}

int
feof(FILE *f)
{
    return (f->flags & F_EOF) != 0;
}

int
ferror(FILE *f)
{
    return (f->flags & F_ERR) != 0;
}

void
clearerr(FILE *f)
{
    f->flags &= ~(F_EOF | F_ERR);
}

/* The stream sink behind the printf family: each character goes to the FILE,
   and every one is counted for the return value.  The base __sink is first, so
   the engine's pointer casts here. */
struct file_sink {
    struct __sink base;
    FILE *f;
};

static void
file_put(struct __sink *s, char c)
{
    struct file_sink *fs = (struct file_sink *)s;
    fputc((unsigned char)c, fs->f);
    s->count++;
}

int
vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct file_sink fs;
    fs.base.put = file_put;
    fs.base.count = 0;
    fs.f = f;
    __vformat(&fs.base, fmt, ap);
    return (int)fs.base.count;
}

int
fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int
vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int
printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}
