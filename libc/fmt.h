#ifndef _LIBC_FMT_H
#define _LIBC_FMT_H

/* The shared conversion engine behind snprintf and the printf family.  It
   renders into an abstract sink, so one formatter serves a caller buffer
   (snprintf) and a stream (fprintf) without duplicating the conversions.
   `put` receives one character at a time and is responsible for counting it in
   `count`; the engine returns `count`, the number of characters a conversion
   produced regardless of whether the sink stored them (C99 snprintf rule). */

#include <stddef.h>
#include <stdarg.h>

struct __sink {
    void (*put)(struct __sink *s, char c);
    size_t count;
};

int __vformat(struct __sink *s, const char *fmt, va_list ap);

#endif
