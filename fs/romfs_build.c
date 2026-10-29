/* romfs builder (host only): pack a real directory tree into a romfs image.
 *
 * Used by the skj-mkromfs tool and the fs unit test.  It walks the directory
 * with the host dirent/stat API, collects a node table, a string table and a
 * data blob, then serializes them little-endian to match the reader. */

#define _GNU_SOURCE
#include "romfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

struct bnode {
    char    *name;
    int      type;
    int      parent;
    uint32_t data_off;      /* into the data blob */
    uint32_t size;
};

struct builder {
    struct bnode *nodes;
    int           nnodes, ncap;
    char         *strs;     /* not used during build; serialized at the end */
    unsigned char *data;
    size_t        data_len, data_cap;
    int           err;
};

static int
add_node(struct builder *b, const char *name, int type, int parent)
{
    if (b->nnodes >= b->ncap) {
        int nc = b->ncap ? b->ncap * 2 : 16;
        struct bnode *nn = realloc(b->nodes, (size_t)nc * sizeof *nn);
        if (!nn) { b->err = -ENOMEM; return -1; }
        b->nodes = nn;
        b->ncap = nc;
    }
    b->nodes[b->nnodes].name = strdup(name);
    b->nodes[b->nnodes].type = type;
    b->nodes[b->nnodes].parent = parent;
    b->nodes[b->nnodes].data_off = 0;
    b->nodes[b->nnodes].size = 0;
    return b->nnodes++;
}

static int
append_file(struct builder *b, const char *path, uint32_t *off, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    size_t cap, len = 0;
    if (!f)
        return -errno;
    *off = (uint32_t)b->data_len;
    cap = b->data_cap;
    for (;;) {
        size_t got;
        if (b->data_len + 65536 > cap) {
            unsigned char *nd;
            cap = cap ? cap * 2 : 65536;
            while (b->data_len + 65536 > cap)
                cap *= 2;
            nd = realloc(b->data, cap);
            if (!nd) { fclose(f); return -ENOMEM; }
            b->data = nd;
            b->data_cap = cap;
        }
        got = fread(b->data + b->data_len, 1, 65536, f);
        b->data_len += got;
        len += got;
        if (got < 65536)
            break;
    }
    fclose(f);
    *size = (uint32_t)len;
    return 0;
}

/* Recursively add the contents of host directory `path` under node `parent`. */
static int
walk_dir(struct builder *b, const char *path, int parent)
{
    DIR *d = opendir(path);
    struct dirent *e;
    if (!d)
        return -errno;
    while ((e = readdir(d))) {
        char child[4096];
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        if (stat(child, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            int idx = add_node(b, e->d_name, ROMFS_T_DIR, parent);
            if (idx < 0) { closedir(d); return b->err; }
            {
                int rc = walk_dir(b, child, idx);
                if (rc < 0) { closedir(d); return rc; }
            }
        } else if (S_ISREG(st.st_mode)) {
            uint32_t off = 0, size = 0;
            int idx, rc = append_file(b, child, &off, &size);
            if (rc < 0) { closedir(d); return rc; }
            idx = add_node(b, e->d_name, ROMFS_T_FILE, parent);
            if (idx < 0) { closedir(d); return b->err; }
            b->nodes[idx].data_off = off;
            b->nodes[idx].size = size;
        }
        /* anything else (symlink, device, ...) is skipped */
    }
    closedir(d);
    return 0;
}

static void
put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

int
romfs_build_dir(const char *dir, void **out, size_t *out_len)
{
    struct builder b;
    uint32_t nodes_off, strs_off, data_off, strs_len = 0;
    uint32_t *name_off;
    unsigned char *img;
    size_t total, pos;
    int i, rc;

    memset(&b, 0, sizeof b);
    if (add_node(&b, "", ROMFS_T_DIR, -1) < 0)   /* node 0: the root */
        return -ENOMEM;
    rc = walk_dir(&b, dir, 0);
    if (rc < 0)
        goto fail;

    /* Lay out the string table: each name NUL-terminated, offset recorded. */
    name_off = calloc((size_t)b.nnodes, sizeof *name_off);
    if (!name_off) { rc = -ENOMEM; goto fail; }
    for (i = 0; i < b.nnodes; i++) {
        name_off[i] = strs_len;
        strs_len += (uint32_t)strlen(b.nodes[i].name) + 1;
    }

    nodes_off = ROMFS_HDR_SIZE;
    strs_off = nodes_off + (uint32_t)b.nnodes * ROMFS_NODE_SIZE;
    data_off = strs_off + strs_len;
    total = (size_t)data_off + b.data_len;

    img = calloc(1, total);
    if (!img) { free(name_off); rc = -ENOMEM; goto fail; }

    memcpy(img, ROMFS_MAGIC, 8);
    put_u32(img + 8, ROMFS_VERSION);
    put_u32(img + 12, (uint32_t)b.nnodes);
    put_u32(img + 16, nodes_off);
    put_u32(img + 20, strs_off);
    put_u32(img + 24, data_off);
    put_u32(img + 28, (uint32_t)total);

    for (i = 0; i < b.nnodes; i++) {
        unsigned char *np = img + nodes_off + (size_t)i * ROMFS_NODE_SIZE;
        put_u32(np + 0, name_off[i]);
        put_u32(np + 4, (uint32_t)b.nodes[i].type);
        put_u32(np + 8, (uint32_t)b.nodes[i].parent);
        put_u32(np + 12, b.nodes[i].data_off);
        put_u32(np + 16, b.nodes[i].size);
    }
    pos = strs_off;
    for (i = 0; i < b.nnodes; i++) {
        size_t l = strlen(b.nodes[i].name) + 1;
        memcpy(img + pos, b.nodes[i].name, l);
        pos += l;
    }
    if (b.data_len)
        memcpy(img + data_off, b.data, b.data_len);

    free(name_off);
    *out = img;
    *out_len = total;
    rc = 0;

fail:
    for (i = 0; i < b.nnodes; i++)
        free(b.nodes[i].name);
    free(b.nodes);
    free(b.data);
    return rc;
}
