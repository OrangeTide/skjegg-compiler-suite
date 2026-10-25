/* kp_io.h : the Kobold console I/O seam (the POSIX-fd model).

   The front end lowers write/writeln/read/readln to these io_* symbols with an fd
   stream argument: fd 1 is standard output, fd 2 standard error, and input is
   stdin (fd 0).  kp_io.c is the single implementation shared by the hosted
   runtime (kp_host.c) and the freestanding runtime (io_free.c).  Only
   io_write_real is defined per tier, because the %g formatter differs (the host
   libc's vs numfmt.c's, byte-identical output); its bytes still reach the fd with
   a plain write, like every other console write here. */
#ifndef KP_IO_H
#define KP_IO_H

#include "str.h"

#include <stdint.h>

void    io_write_str(int fd, kp_gptr s);
void    io_write_int(int fd, int v);
void    io_write_char(int fd, int ch);
void    io_write_real(int fd, double x);   /* per tier: kp_host.c / io_free.c */
void    io_flush(int fd);
void    io_halt(int code);

int64_t io_read_int(void);
double  io_read_real(void);
int     io_read_char(void);
void    io_read_ln(void);
kp_gptr io_read_str(void);
int     io_eof(void);

#endif
