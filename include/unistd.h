#ifndef _UNISTD_H
#define _UNISTD_H

/* The POSIX file-removal calls.  ISO C's remove()/rename() live in <stdio.h>;
   these are the POSIX spellings a self-hosting build expects.  libc/fileops.c
   wraps the crt's *at syscall wrappers with AT_FDCWD. */
int unlink(const char *path);
int rmdir(const char *path);

#endif /* _UNISTD_H */
