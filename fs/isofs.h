/* isofs: a read-only ISO 9660 (CD-ROM) backend for the shared fs/ layer.
 *
 * The image is read through a single sector accessor, so it comes either from a
 * byte buffer in memory (an embedded .iso or a disc read into RAM) or from a
 * sector-read callback that fetches sectors on demand (a real CD-ROM drive). The
 * reader locates the primary volume descriptor, walks the directory tree by
 * directory records, and reads file extents. All multi-byte fields are read
 * little-endian, so one image is valid on every target. The reader is
 * freestanding-safe. */

#ifndef ISOFS_H
#define ISOFS_H

#include <stddef.h>
#include <stdint.h>

#include "fs.h"

extern const struct fs_backend isofs_backend;

/* Mount an ISO image already in memory. The image must outlive the mount.
 * Returns the mount index or a negative errno. */
int isofs_mount(const char *name, const void *image, size_t len, uint32_t flags);

/* Mount a disc read on demand through a sector-read callback: read_sector(lba,
 * buf) fills a 2048-byte buffer and returns 0, or nonzero on error. This is the
 * memory-realistic path (libpsx/cd.c on the PlayStation) where the disc is not
 * held in RAM. Returns the mount index or a negative errno. */
int isofs_mount_stream(const char *name, int (*read_sector)(uint32_t, void *),
                       uint32_t flags);

/* Register an in-memory image for an already-installed mount at index idx (the
 * launcher path). Returns 0 or a negative errno. The image must outlive the
 * mount. */
intptr_t isofs_adopt(int idx, const void *image, size_t len);

#endif
