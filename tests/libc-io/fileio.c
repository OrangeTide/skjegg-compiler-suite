/* fileio.c : exercise the guest libc's real-file I/O and argv the way the
   toolchain does.  argv[1] is an input path, argv[2] an output path.  It slurps
   the input with the fopen / fseek(SEEK_END) / ftell / rewind / fread idiom
   (the same one ir/util.c:slurp uses), writes the bytes back out with fopen /
   fwrite, and prints argc, the paths, and the byte count so a run under skj-run
   and one under qemu-riscv32 can be compared.  Returns 0, or 2 on any error. */

#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
    FILE *f, *o;
    long n;
    char *buf;

    printf("argc=%d\n", argc);
    if (argc < 3) {
        printf("usage: fileio in out\n");
        return 2;
    }
    printf("in=%s out=%s\n", argv[1], argv[2]);

    f = fopen(argv[1], "rb");
    if (f == NULL) {
        printf("cannot open %s\n", argv[1]);
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        printf("cannot size %s\n", argv[1]);
        return 2;
    }
    rewind(f);
    buf = malloc((size_t)n + 1);
    if (buf == NULL)
        return 2;
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        printf("short read\n");
        return 2;
    }
    buf[n] = '\0';
    fclose(f);
    printf("read %ld bytes\n", n);

    o = fopen(argv[2], "w");
    if (o == NULL) {
        printf("cannot open %s\n", argv[2]);
        return 2;
    }
    if (fwrite(buf, 1, (size_t)n, o) != (size_t)n) {
        printf("short write\n");
        return 2;
    }
    fclose(o);
    free(buf);
    printf("wrote %ld bytes\n", n);
    return 0;
}
