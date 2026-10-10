#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF    (-1)
#define BUFSIZ 1024

/* buffering modes for setvbuf-style fields (setvbuf itself is not yet here) */
#define _IOFBF 0        /* fully buffered */
#define _IOLBF 1        /* line buffered  */
#define _IONBF 2        /* unbuffered     */

/* A stream.  The buffer is embedded rather than pointed at, so the three
   standard streams need no static pointer initializer (which skj-cc does not
   yet emit).  Fields are private; use the functions. */
typedef struct __FILE {
    int fd;
    int flags;                  /* read/write/eof/error bits */
    int mode;                   /* _IOFBF / _IOLBF / _IONBF */
    int pos;                    /* bytes pending (write) or next index (read) */
    int len;                    /* valid bytes in buf (read) */
    unsigned char buf[BUFSIZ];
} FILE;

extern FILE __libc_stdin;
extern FILE __libc_stdout;
extern FILE __libc_stderr;

#define stdin  (&__libc_stdin)
#define stdout (&__libc_stdout)
#define stderr (&__libc_stderr)

int   fflush(FILE *stream);

int   fputc(int c, FILE *stream);
int   putc(int c, FILE *stream);
int   putchar(int c);
int   fputs(const char *s, FILE *stream);
int   puts(const char *s);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

int   fgetc(FILE *stream);
int   getc(FILE *stream);
int   getchar(void);
char *fgets(char *s, int size, FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);

int   feof(FILE *stream);
int   ferror(FILE *stream);
void  clearerr(FILE *stream);

int   printf(const char *fmt, ...);
int   fprintf(FILE *stream, const char *fmt, ...);
int   vprintf(const char *fmt, va_list ap);
int   vfprintf(FILE *stream, const char *fmt, va_list ap);

int   snprintf(char *buf, size_t size, const char *fmt, ...);
int   vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int   sprintf(char *buf, const char *fmt, ...);
int   vsprintf(char *buf, const char *fmt, va_list ap);

int   sscanf(const char *str, const char *fmt, ...);
int   vsscanf(const char *str, const char *fmt, va_list ap);

void  perror(const char *s);

#endif
