/* isotest.c - host self-test for the ISO 9660 backend (milestone 7).
 *
 * Reads an ISO image on the host through the shared fs/ layer (fs.c + isofs.c),
 * the same reader the PlayStation uses, and checks that files read back. The
 * image is built by genisoimage from a seed directory, so this verifies the
 * reader against a real ISO 9660 filesystem. Host-only, no emulator.
 *
 * It exercises both forms of the backend: a RAM mount (a pointer into the image)
 * and a streaming mount (a sector-read callback, as the CD-ROM driver provides
 * on the console). The streaming side reads a multi-kilobyte file and compares
 * it byte for byte with the RAM read, so the one-sector cache and reads that
 * span sector boundaries are covered.
 *
 * Usage: isotest <image.iso>   (exits 0 on success)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "isofs.h"

#define ISO_SECTOR 2048

/* The image, held for the streaming callback to read sectors from. */
static const unsigned char *g_img;
static long g_len;

/* A sector-read callback over the in-memory image: the same shape the CD-ROM
 * driver (libpsx/cd.c psx_cd_read_sector) implements, so the streaming path is
 * exercised by the same code on the host. */
static int
ram_read_sector(uint32_t lba, void *buf)
{
    long off = (long)lba * ISO_SECTOR;
    if (off + ISO_SECTOR > g_len)
        return -1;
    memcpy(buf, g_img + off, ISO_SECTOR);
    return 0;
}

/* Read a whole file through an fs path into a freshly malloc'd buffer. Returns
 * the length, or -1. */
static long
read_all(const char *path, unsigned char **out)
{
    int fd = fs_open(path, FS_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    size_t cap = 4096, len = 0;
    unsigned char *b = malloc(cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            b = realloc(b, cap);
        }
        int r = fs_read(fd, b + len, (int)(cap - len));
        if (r < 0) { free(b); fs_close(fd); return -1; }
        if (r == 0) break;
        len += (size_t)r;
    }
    fs_close(fd);
    *out = b;
    return (long)len;
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: isotest <image.iso>\n");
        return 9;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "isotest: cannot open %s\n", argv[1]);
        return 9;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "isotest: short read\n");
        return 9;
    }
    fclose(f);
    g_img = buf;
    g_len = n;

    fs_init();
    if (isofs_mount("cd", buf, (size_t)n, 0) < 0) {
        printf("isotest: RAM mount failed\n");
        return 1;
    }

    char rb[64];
    int fd = fs_open("cd:greet.txt", FS_O_RDONLY, 0);
    if (fd < 0) {
        printf("isotest: open greet.txt failed (%d)\n", fd);
        return 2;
    }
    int r = fs_read(fd, rb, sizeof rb);
    fs_close(fd);
    if (r != 21) {
        printf("isotest: greet.txt read %d bytes, want 21\n", r);
        return 3;
    }
    if (fs_open("cd:absent.txt", FS_O_RDONLY, 0) >= 0) {
        printf("isotest: a missing path opened\n");
        return 4;
    }

    /* Streaming mount: the same filesystem reached through a sector callback. */
    if (isofs_mount_stream("cds", ram_read_sector, 0) < 0) {
        printf("isotest: streaming mount failed\n");
        return 5;
    }
    char sb[64];
    fd = fs_open("cds:greet.txt", FS_O_RDONLY, 0);
    if (fd < 0) {
        printf("isotest: streaming open greet.txt failed (%d)\n", fd);
        return 6;
    }
    int sr = fs_read(fd, sb, sizeof sb);
    fs_close(fd);
    if (sr != 21 || memcmp(sb, rb, 21) != 0) {
        printf("isotest: streaming greet.txt mismatch (%d bytes)\n", sr);
        return 7;
    }

    /* A multi-sector file: read it both ways and compare byte for byte, so the
     * streaming path's cache and sector-spanning reads are verified. */
    unsigned char *ram_big, *cd_big;
    long ram_len = read_all("cd:big.txt", &ram_big);
    long cd_len = read_all("cds:big.txt", &cd_big);
    if (ram_len <= ISO_SECTOR * 2) {
        printf("isotest: big.txt too small to span sectors (%ld)\n", ram_len);
        return 8;
    }
    if (ram_len != cd_len || memcmp(ram_big, cd_big, (size_t)ram_len) != 0) {
        printf("isotest: streaming big.txt != RAM (%ld vs %ld)\n", ram_len, cd_len);
        return 10;
    }
    free(ram_big);
    free(cd_big);

    printf("isotest: ISO 9660 RAM + streaming OK (greet \"%.*s\", big %ld bytes)\n",
           r, rb, ram_len);
    return 0;
}
