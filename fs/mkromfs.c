/* skj-mkromfs: pack a directory tree into a romfs image (see romfs.h). */

#include "romfs.h"

#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
    void *img;
    size_t len;
    FILE *f;
    int rc;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <dir> <out.img>\n", argv[0]);
        return 2;
    }
    rc = romfs_build_dir(argv[1], &img, &len);
    if (rc < 0) {
        fprintf(stderr, "%s: cannot pack '%s' (error %d)\n", argv[0], argv[1], -rc);
        return 1;
    }
    f = fopen(argv[2], "wb");
    if (!f) {
        perror(argv[2]);
        free(img);
        return 1;
    }
    if (fwrite(img, 1, len, f) != len) {
        perror(argv[2]);
        fclose(f);
        free(img);
        return 1;
    }
    fclose(f);
    free(img);
    fprintf(stderr, "%s: wrote %s (%zu bytes)\n", argv[0], argv[2], len);
    return 0;
}
