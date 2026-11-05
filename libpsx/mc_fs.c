/* mc_fs.c - libpsx memory card save-file manager.
 *
 * The named-save layer over the raw frame I/O (mc.c). A PlayStation card has a
 * directory in its first block: frame 0 is a header, and frames 1 through 15
 * each describe one of the fifteen 8 KB save blocks that follow. A save of up
 * to fifteen blocks is a chain of blocks whose directory entries link together;
 * the first entry carries the file's name and total size.
 *
 * This implements that format: format a card, save a named blob across as many
 * blocks as it needs, load it back, list the saves, and delete one. It follows
 * the documented on-card layout, so saves are self-consistent and a freshly
 * formatted card looks formatted.
 */

#include "psx.h"

#define FRAME        PSX_MC_FRAME_SIZE  /* 128 bytes */
#define BLOCK_FRAMES 64                 /* 64 frames per 8 KB block */
#define BLOCK_SIZE   (BLOCK_FRAMES * FRAME)
#define DIR_BLOCKS   15                 /* save blocks 1..15 */
#define NAME_MAX     20                 /* save name, not counting the NUL */

/* Directory-entry states (bytes 0-3). */
#define ST_FREE   0xA0                  /* formatted, unused */
#define ST_FIRST  0x51                  /* in use, first (or only) block */
#define ST_MIDDLE 0x52                  /* in use, a middle block */
#define ST_LAST   0x53                  /* in use, the last block */

#define LINK_NONE 0xFFFF                /* no next block */

static uint8_t
xorsum(const uint8_t *p, int n)
{
    uint8_t s = 0;
    for (int i = 0; i < n; i++)
        s ^= p[i];
    return s;
}

/* A directory entry occupies the frame whose number equals the block it
 * describes (block b in 1..15 is described by frame b). */
static int
read_dir(int block, uint8_t *frame)
{
    return psx_mc_read_frame(block, frame);
}

static int
write_dir(int block, uint8_t *frame)
{
    frame[FRAME - 1] = xorsum(frame, FRAME - 1);
    return psx_mc_write_frame(block, frame);
}

static void
put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t
get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int
name_eq(const uint8_t *entry, const char *name)
{
    for (int i = 0; i < NAME_MAX; i++) {
        char c = name[i];
        if ((uint8_t)c != entry[0x0A + i])
            return 0;
        if (c == '\0')
            return 1;
    }
    return 1;
}

int
psx_mc_format(void)
{
    uint8_t f[FRAME];
    int i;

    /* Header frame: the "MC" magic, the rest zero. */
    for (i = 0; i < FRAME; i++)
        f[i] = 0;
    f[0] = 'M';
    f[1] = 'C';
    f[FRAME - 1] = xorsum(f, FRAME - 1);
    if (psx_mc_write_frame(0, f) != 0)
        return -1;

    /* Fifteen free directory entries. */
    for (int b = 1; b <= DIR_BLOCKS; b++) {
        for (i = 0; i < FRAME; i++)
            f[i] = 0;
        put32(f + 0x00, ST_FREE);
        put32(f + 0x04, 0);             /* no size */
        f[0x08] = 0xFF;                 /* link: none */
        f[0x09] = 0xFF;
        if (write_dir(b, f) != 0)
            return -2;
    }

    /* The broken-sector and unused frames (16..63) as a formatted card has
     * them: 0xFF filled. */
    for (i = 0; i < FRAME; i++)
        f[i] = 0xFF;
    for (int fr = 16; fr < BLOCK_FRAMES; fr++)
        if (psx_mc_write_frame(fr, f) != 0)
            return -3;

    return 0;
}

/* Find the first-block directory entry of a named save, or -1. */
static int
find_save(const char *name, uint8_t *entry)
{
    for (int b = 1; b <= DIR_BLOCKS; b++) {
        uint8_t f[FRAME];
        if (read_dir(b, f) != 0)
            continue;
        if (get32(f) == ST_FIRST && name_eq(f, name)) {
            if (entry)
                for (int i = 0; i < FRAME; i++)
                    entry[i] = f[i];
            return b;
        }
    }
    return -1;
}

int
psx_mc_delete(const char *name)
{
    int b = find_save(name, 0);
    if (b < 0)
        return -1;

    /* Walk the block chain, freeing each directory entry. */
    while (b >= 1 && b <= DIR_BLOCKS) {
        uint8_t f[FRAME];
        if (read_dir(b, f) != 0)
            return -2;
        int link = f[0x08] | (f[0x09] << 8);
        put32(f + 0x00, ST_FREE);
        f[0x08] = 0xFF;
        f[0x09] = 0xFF;
        if (write_dir(b, f) != 0)
            return -2;
        if (link == LINK_NONE)
            break;
        b = link + 1;                   /* link is the 0-based block index */
    }
    return 0;
}

int
psx_mc_save(const char *name, const void *data, int nbytes)
{
    const uint8_t *src = data;
    int need = (nbytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (need < 1)
        need = 1;
    if (need > DIR_BLOCKS)
        return -1;

    /* Overwrite: drop any existing save of the same name first. */
    psx_mc_delete(name);

    /* Gather `need` free blocks. */
    int chosen[DIR_BLOCKS];
    int got = 0;
    for (int b = 1; b <= DIR_BLOCKS && got < need; b++) {
        uint8_t f[FRAME];
        if (read_dir(b, f) != 0)
            return -2;
        if (get32(f) == ST_FREE)
            chosen[got++] = b;
    }
    if (got < need)
        return -3;                      /* not enough free blocks */

    /* Write each block's data, then its directory entry. */
    for (int i = 0; i < need; i++) {
        int block = chosen[i];
        int base = i * BLOCK_SIZE;
        for (int fr = 0; fr < BLOCK_FRAMES; fr++) {
            uint8_t buf[FRAME];
            for (int k = 0; k < FRAME; k++) {
                int off = base + fr * FRAME + k;
                buf[k] = off < nbytes ? src[off] : 0;
            }
            if (psx_mc_write_frame(block * BLOCK_FRAMES + fr, buf) != 0)
                return -4;
        }

        uint8_t e[FRAME];
        for (int k = 0; k < FRAME; k++)
            e[k] = 0;
        uint32_t state = need == 1 ? ST_FIRST
                       : i == 0 ? ST_FIRST
                       : i == need - 1 ? ST_LAST : ST_MIDDLE;
        put32(e + 0x00, state);
        put32(e + 0x04, (uint32_t)nbytes);      /* total size on every entry */
        if (i == need - 1) {
            e[0x08] = 0xFF;
            e[0x09] = 0xFF;
        } else {
            int next = chosen[i + 1] - 1;        /* 0-based block index */
            e[0x08] = (uint8_t)next;
            e[0x09] = (uint8_t)(next >> 8);
        }
        if (i == 0)                              /* name on the first block */
            for (int k = 0; k < NAME_MAX && name[k]; k++)
                e[0x0A + k] = (uint8_t)name[k];
        if (write_dir(block, e) != 0)
            return -4;
    }
    return 0;
}

int
psx_mc_load(const char *name, void *buf, int bufsize)
{
    uint8_t entry[FRAME];
    int b = find_save(name, entry);
    if (b < 0)
        return -1;

    int size = (int)get32(entry + 0x04);
    int want = size < bufsize ? size : bufsize;
    uint8_t *dst = buf;
    int done = 0;

    while (b >= 1 && b <= DIR_BLOCKS && done < want) {
        uint8_t f[FRAME];
        if (read_dir(b, f) != 0)
            return -2;
        int link = f[0x08] | (f[0x09] << 8);
        for (int fr = 0; fr < BLOCK_FRAMES && done < want; fr++) {
            uint8_t buf2[FRAME];
            if (psx_mc_read_frame(b * BLOCK_FRAMES + fr, buf2) != 0)
                return -2;
            for (int k = 0; k < FRAME && done < want; k++)
                dst[done++] = buf2[k];
        }
        if (link == LINK_NONE)
            break;
        b = link + 1;
    }
    return done;
}

int
psx_mc_list(char names[][NAME_MAX + 1], int max)
{
    int n = 0;
    for (int b = 1; b <= DIR_BLOCKS && n < max; b++) {
        uint8_t f[FRAME];
        if (read_dir(b, f) != 0)
            continue;
        if (get32(f) == ST_FIRST) {
            int k;
            for (k = 0; k < NAME_MAX; k++)
                names[n][k] = (char)f[0x0A + k];
            names[n][NAME_MAX] = '\0';
            n++;
        }
    }
    return n;
}
