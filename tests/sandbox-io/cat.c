/* cat.c : copy a file to stdout through the sandbox file-I/O syscalls.

   It exercises the RV32 sandbox's openat / read / write / close on skj-run: the
   guest opens the compile-time PATH with openat(AT_FDCWD, ...), reads it in
   chunks, and writes each chunk to fd 1.  The freestanding crt passes no argv,
   so the path is baked in with -DPATH.  The syscalls go straight through
   skj-cc-rv's extended inline asm (a7 = number, a0..a3 = args), the same way
   the write(2) asm test does. */

#define AT_FDCWD    (-100)
#define O_RDONLY    0
#define NR_OPENAT   56
#define NR_WRITE    64
#define NR_READ     63
#define NR_CLOSE    57

static long
syscall3(long n, long a0, long a1, long a2)
{
    register long x7 __asm__("a7") = n;
    register long x0 __asm__("a0") = a0;
    register long x1 __asm__("a1") = a1;
    register long x2 __asm__("a2") = a2;

    __asm__ volatile("ecall"
                     : "+r"(x0)
                     : "r"(x7), "r"(x1), "r"(x2)
                     : "memory");
    return x0;
}

static long
syscall4(long n, long a0, long a1, long a2, long a3)
{
    register long x7 __asm__("a7") = n;
    register long x0 __asm__("a0") = a0;
    register long x1 __asm__("a1") = a1;
    register long x2 __asm__("a2") = a2;
    register long x3 __asm__("a3") = a3;

    __asm__ volatile("ecall"
                     : "+r"(x0)
                     : "r"(x7), "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

int
main(void)
{
    char buf[256];
    long fd, n;

    fd = syscall4(NR_OPENAT, AT_FDCWD, (long)PATH, O_RDONLY, 0);
    if (fd < 0)
        return 2;
    for (;;) {
        n = syscall3(NR_READ, fd, (long)buf, (long)sizeof buf);
        if (n <= 0)
            break;
        syscall3(NR_WRITE, 1, (long)buf, n);
    }
    syscall3(NR_CLOSE, fd, 0, 0);
    return 0;
}
