#ifndef _ERRNO_H
#define _ERRNO_H

/* A single global errno (the library is single threaded, so no per-thread
   indirection).  The three C-standard codes come first; the rest are the
   common POSIX names a hosted program expects, with stable local numbers. */
extern int errno;

#define EDOM    1       /* argument outside a function's domain */
#define ERANGE  2       /* result not representable */
#define EILSEQ  3       /* illegal byte sequence */
#define EINVAL  4       /* invalid argument */
#define ENOMEM  5       /* out of memory */
#define ENOENT  6       /* no such file or directory */
#define EBADF   7       /* bad file descriptor */
#define EIO     8       /* input/output error */
#define EACCES  9       /* permission denied */
#define ENOSPC  10      /* no space left on device */

#endif
