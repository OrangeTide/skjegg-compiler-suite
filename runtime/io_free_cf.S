/* io_free_cf.S : the ColdFire syscall seam for the Kobold freestanding guest.
 *
 * The wrappers are the same ones the C crt uses, so they live in syscall_cf.S
 * and both files .include it; the Kobold console C (io_free.c) and libc's
 * stdio_core.c build on write / read, io_halt exits through _exit, and the
 * ColdFire file-I/O surface (ReadFileStr / WriteFileStr) uses open / close. */

    .text
    .include "syscall_cf.S"
