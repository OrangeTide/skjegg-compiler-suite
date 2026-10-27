/* hostfs: a real host directory tree, confined to the granted root.  A
 * development/admin backend, avoided on production servers. */

#ifndef FS_HOSTFS_H
#define FS_HOSTFS_H

#include "fs.h"

extern const struct fs_backend hostfs_backend;

/* Open `hostdir` on the real host and install it as a mount named `name` with
 * `flags`.  All access through the mount is confined beneath that directory.
 * Returns the new mount index or a negative errno. */
int hostfs_mount(const char *name, const char *hostdir, uint32_t flags);

#endif /* FS_HOSTFS_H */
