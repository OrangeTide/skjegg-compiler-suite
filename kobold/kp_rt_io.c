/* kp_rt_io.c : the hosted Kobold runtime surface (see kp_rt_io.h).

   One source for the file, directory, file-string, and debug/test entry points
   the stdlib units call by name, shared by the two hosted tiers that used to
   carry byte-identical copies: the AOT start file (aot_rt.c) and the in-process
   JIT driver (jit_main.c). It is compiled twice. For the JIT it is built
   host-native and linked into skj-kp-jit, and the driver binds each guest
   symbol name to the linked function. For an AOT program it is built with the
   target toolchain and the guest calls each function directly. The leaf
   primitives (open/read/write/stat/opendir/readdir) therefore resolve to the
   host libc in the JIT and to the guest libc (which lowers to the sandbox
   syscall seam) in an AOT build, which is the whole of the per-tier difference.

   The freestanding tier (io_free.c) is deliberately not folded in here: it
   routes the same names through the fs-interpose wrappers, allocates its read
   buffer from the guest heap, and walks directories over raw getdents64 with no
   opendir/DIR* or malloc, so its bodies differ rather than merely resolving
   different leaf calls. */

#if !defined(_DARWIN_C_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "kp_rt_io.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

/* From kobold/kp_host.c (JIT) or its AOT twin: whole-file read/write. */
char *kp_read_file(const char *path, size_t *lenp);
int kp_write_file(const char *path, const char *data, size_t len);

kp_gptr ReadFileStr(kp_gptr pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    char *buf;
    size_t n = 0;
    kp_gptr s;

    if (plen < 0 || plen >= (int)sizeof(path))
        return kp_str_new("", 0);
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    buf = kp_read_file(path, &n);
    s = kp_str_new(buf ? buf : "", (int)n);
    free(buf);
    return s;
}

/* WriteFileStr(path, data): write a guest string to a host file named by another
   guest string, returning 1 on success and 0 on failure.  The front end's `-o`
   uses it to write the KPM1 module without a shell redirect. */
int WriteFileStr(kp_gptr pathstr, kp_gptr datastr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);

    if (plen < 0 || plen >= (int)sizeof(path))
        return 0;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    return kp_write_file(path, kp_str_bytes(datastr), (size_t)kp_str_len(datastr));
}

/* The line/stream file primitives the `files` unit builds on: open a handle,
   read one chunk, write bytes, close.  Stateless (the unit holds the fd and does
   its own line buffering). */
#define KPF_CHUNK 4096

int kpf_open_read(kp_gptr pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    if (plen < 0 || plen >= (int)sizeof(path))
        return -1;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    return open(path, O_RDONLY);
}

int kpf_open_write(kp_gptr pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    if (plen < 0 || plen >= (int)sizeof(path))
        return -1;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

kp_gptr kpf_read(int fd)
{
    char tmp[KPF_CHUNK];
    ssize_t n = (fd < 0) ? -1 : read(fd, tmp, sizeof tmp);
    if (n <= 0)
        return kp_str_new("", 0);
    return kp_str_new(tmp, (int)n);
}

int kpf_write(int fd, kp_gptr datastr)
{
    if (fd < 0)
        return -1;
    return (int)write(fd, kp_str_bytes(datastr), (size_t)kp_str_len(datastr));
}

void kpf_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* The path metadata and operations the `files` unit exposes (exists, size, a
   directory test, delete/rename/mkdir/rmdir) are tier-independent and live in
   kobold/kp_rt_meta.c, shared with the freestanding tier too. */

/* Directory reads for the `dirs` unit, over the host opendir/readdir.  A Kobold
   handle is an index into a small table of open DIR*; kpd_next returns one
   entry name at a time (or the empty string at end). */
#define KPD_MAX 16
static DIR *kpd_tab[KPD_MAX];

int kpd_open(kp_gptr pathstr)
{
    char path[1024];
    int plen = kp_str_len(pathstr);
    int i;
    if (plen < 0 || plen >= (int)sizeof(path))
        return -1;
    memcpy(path, kp_str_bytes(pathstr), (size_t)plen);
    path[plen] = '\0';
    for (i = 0; i < KPD_MAX; i++)
        if (!kpd_tab[i])
            break;
    if (i == KPD_MAX)
        return -1;
    kpd_tab[i] = opendir(path);
    return kpd_tab[i] ? i : -1;
}

kp_gptr kpd_next(int h)
{
    struct dirent *e;
    if (h < 0 || h >= KPD_MAX || !kpd_tab[h])
        return kp_str_new("", 0);
    e = readdir(kpd_tab[h]);
    if (!e)
        return kp_str_new("", 0);
    return kp_str_new(e->d_name, (int)strlen(e->d_name));
}

void kpd_close(int h)
{
    if (h >= 0 && h < KPD_MAX && kpd_tab[h]) {
        closedir(kpd_tab[h]);
        kpd_tab[h] = 0;
    }
}
