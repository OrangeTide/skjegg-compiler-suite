#include <stdio.h>
#include <stddef.h>
#include "stdio_impl.h"

/* The buffered-stream core: the three standard streams and the character and
   block I/O over them.  Everything here builds only on the write/read seam the
   crt provides, with no malloc, no open/lseek, and no printf engine, so a
   freestanding runtime that wants buffered console I/O (the Kobold freestanding
   tier) links this object alone.  fopen/fseek/ftell and the printf family live
   in stdio.c, which pulls malloc and the format engine.

   stdout is line buffered, stderr unbuffered, stdin read in BUFSIZ chunks.  The
   crt's normal exit (returning from main) does not flush, so a program that
   leaves output in a buffer should fflush before it returns; line buffering
   means output ending in a newline is already flushed. */

extern int write(int fd, const void *buf, int n);
extern int read(int fd, void *buf, int n);

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
    if (f->flush)                       /* memory stream: grow the caller's buffer */
        return f->flush(f);
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
