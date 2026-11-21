/* WASM file, directory, and stat self-check.  It drives the libc file layer
   (open/close/lseek over stdio and the raw calls, stat, the directory walk, and
   the path operations), which on the WASM target bottoms out at the WASI shim
   runtime/wasi_file.c.  The lane runs it under `wasmtime --dir`, giving it a
   preopened scratch directory; it creates, reads, stats, lists, renames, and
   removes within that directory and returns 42 when every check holds.  It
   cleans up after itself so the scratch directory is empty on exit. */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

int
main(void)
{
    char buf[64];
    int fd, n;
    struct stat st;
    DIR *d;
    struct dirent *de;
    int saw_file = 0, saw_dir = 0;
    FILE *f;

    /* raw open/write/read/lseek */
    fd = open("hello.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 1;
    if (write(fd, "hello world", 11) != 11) return 2;
    close(fd);

    fd = open("hello.txt", O_RDONLY, 0);
    if (fd < 0) return 3;
    n = read(fd, buf, sizeof buf);
    if (n != 11 || memcmp(buf, "hello world", 11) != 0) return 4;
    if (lseek(fd, 6, SEEK_SET) != 6) return 5;
    n = read(fd, buf, sizeof buf);
    if (n != 5 || memcmp(buf, "world", 5) != 0) return 6;
    close(fd);

    /* buffered stdio over a real file */
    f = fopen("num.txt", "w");
    if (!f) return 7;
    fprintf(f, "value=%d\n", 1234);
    fclose(f);

    f = fopen("num.txt", "r");
    if (!f) return 8;
    if (!fgets(buf, (int)sizeof buf, f)) return 9;
    if (strcmp(buf, "value=1234\n") != 0) return 10;
    fclose(f);

    /* stat a file and a directory */
    if (stat("hello.txt", &st) != 0) return 11;
    if (!S_ISREG(st.st_mode) || st.st_size != 11) return 12;

    if (mkdir("sub", 0755) != 0) return 13;
    if (stat("sub", &st) != 0 || !S_ISDIR(st.st_mode)) return 14;

    /* directory walk finds the files we made (and skips . / ..) */
    d = opendir(".");
    if (!d) return 15;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, "hello.txt") == 0) saw_file = 1;
        if (strcmp(de->d_name, "sub") == 0) saw_dir = 1;
    }
    closedir(d);
    if (!saw_file || !saw_dir) return 16;

    /* path operations: rename then remove everything */
    if (rename("hello.txt", "bye.txt") != 0) return 17;
    if (stat("hello.txt", &st) == 0) return 18;      /* old name gone */
    if (stat("bye.txt", &st) != 0) return 19;        /* new name present */

    if (unlink("bye.txt") != 0) return 20;
    if (unlink("num.txt") != 0) return 21;
    if (rmdir("sub") != 0) return 22;

    /* the scratch directory is empty again */
    d = opendir(".");
    if (!d) return 23;
    n = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        n++;
    }
    closedir(d);
    if (n != 0) return 24;

    return 42;
}
