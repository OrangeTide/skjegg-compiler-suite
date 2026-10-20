/* dirio.c : exercise the guest libc's opendir/readdir/closedir (served by
   getdents64 through skj-run's GSYS_GETDENTS64 handler).  argv[1] is a scratch
   directory the harness has filled with known files.  Prints an
   order-independent summary (entry count and total name length, "." and ".."
   skipped), so a run under skj-run and one under qemu can be compared.  Returns
   0, or 2 on a usage/opendir error. */

#include <stdio.h>
#include <string.h>
#include <dirent.h>

int
main(int argc, char **argv)
{
    DIR *dir;
    struct dirent *e;
    int n = 0, total = 0;

    if (argc < 2) {
        printf("usage: dirio scratchdir\n");
        return 2;
    }
    dir = opendir(argv[1]);
    if (!dir) {
        printf("opendir failed\n");
        return 2;
    }
    while ((e = readdir(dir)) != 0) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        n++;
        total += (int)strlen(e->d_name);
    }
    closedir(dir);
    printf("entries=%d\n", n);
    printf("namelen=%d\n", total);
    return 0;
}
