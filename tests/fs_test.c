/* Unit test for the fs/ mount-table + namespace layer, driven directly from C
 * (no language runtime).  Builds a two-mount table by hand the way a launcher
 * would (ramfs scratch at index 0, a hostfs mount at index 1 over a temp dir)
 * and exercises resolution, the DOS current-mount + per-mount-cwd state, the
 * ramfs backend, and hostfs confinement. */

#define _GNU_SOURCE
#include "fs.h"
#include "ramfs.h"
#include "hostfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("ok   - %s\n", msg);                                       \
        } else {                                                              \
            printf("FAIL - %s  (%s:%d)\n", msg, __FILE__, __LINE__);          \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static int
write_file(const char *path, const char *data)
{
    int fd = fs_open(path, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC, 0644);
    int n;
    if (fd < 0)
        return fd;
    n = fs_write(fd, data, (int)strlen(data));
    fs_close(fd);
    return n;
}

static int
read_file(const char *path, char *buf, int cap)
{
    int fd = fs_open(path, FS_O_RDONLY, 0);
    int n;
    if (fd < 0)
        return fd;
    n = fs_read(fd, buf, cap - 1);
    fs_close(fd);
    if (n >= 0)
        buf[n] = 0;
    return n;
}

int
main(void)
{
    char tmpl[] = "/tmp/fs_test.XXXXXX";
    char *hostdir = mkdtemp(tmpl);
    char buf[128], cwd[128];
    int rc, t, h, dfd, saw_a, saw_b, count;

    if (!hostdir) {
        perror("mkdtemp");
        return 2;
    }

    /* Launcher-style table setup: scratch ramfs at index 0, host at index 1. */
    t = ramfs_mount("tmp", 0);
    h = hostfs_mount("host", hostdir, 0);
    CHECK(t == 0, "ramfs scratch is mount index 0");
    CHECK(h == 1, "hostfs is mount index 1");

    /* Lookup and the default current mount. */
    CHECK(fs_mount_find("tmp") == 0, "find tmp -> 0");
    CHECK(fs_mount_find("host") == 1, "find host -> 1");
    CHECK(fs_mount_find("nope") == -1, "find nope -> -1");
    CHECK(fs_mount_current() == 0, "default current mount is scratch (0)");

    /* ramfs via a bare path (resolves against the current mount, scratch). */
    CHECK(write_file("hello", "world") == 5, "write bare hello");
    CHECK(fs_exists("hello") == 1, "hello exists");
    CHECK(fs_size("hello") == 5, "hello size is 5");
    CHECK(read_file("hello", buf, sizeof buf) == 5 && strcmp(buf, "world") == 0,
          "read hello back");

    /* name: selector reaches the same scratch mount explicitly. */
    CHECK(fs_exists("tmp:hello") == 1, "tmp:hello exists via selector");

    /* fs_stat / fs_lseek / fs_fstat on the ramfs file. */
    {
        struct fs_stat sp, sf;
        int fd;
        CHECK(fs_stat("hello", &sp) == 0 &&
              (sp.mode & FS_S_IFMT) == FS_S_IFREG && sp.size == 5,
              "fs_stat hello: regular file, size 5");
        fd = fs_open("hello", FS_O_RDONLY, 0);
        CHECK(fd >= FS_FD_BASE, "open hello for seek");
        CHECK(fs_lseek(fd, 2, FS_SEEK_SET) == 2, "lseek hello to 2");
        CHECK(fs_read(fd, buf, sizeof buf - 1) == 3 &&
              (buf[3] = 0, strcmp(buf, "rld") == 0), "read from offset 2 gives rld");
        CHECK(fs_lseek(fd, 0, FS_SEEK_END) == 5, "lseek SEEK_END gives size 5");
        CHECK(fs_fstat(fd, &sf) == 0 && sf.ino == sp.ino && sf.dev == sp.dev,
              "fs_fstat matches fs_stat (dev,ino)");
        fs_close(fd);
    }

    /* mkdir + directory walk. */
    CHECK(fs_mkdir("d") == 0, "mkdir d");
    CHECK(fs_isdir("d") == 1, "d is a directory");
    CHECK(write_file("d/a", "A") == 1, "write d/a");
    CHECK(write_file("d/b", "BB") == 2, "write d/b");
    dfd = fs_open("d", FS_O_DIRECTORY | FS_O_RDONLY, 0);
    CHECK(dfd >= FS_FD_BASE, "open dir d");
    saw_a = saw_b = count = 0;
    {
        struct fs_dirent de;
        while (fs_read(dfd, &de, sizeof de) == (int)sizeof de) {
            if (strcmp(de.name, "a") == 0) saw_a = 1;
            if (strcmp(de.name, "b") == 0) saw_b = 1;
            count++;
        }
    }
    fs_close(dfd);
    CHECK(saw_a && saw_b && count == 2, "dir d lists exactly a and b");

    /* DOS per-mount cwd: chdir into d, then bare paths resolve there while an
     * absolute path still reaches the mount root. */
    CHECK(fs_chdir("d") == 0, "chdir d");
    CHECK(fs_getcwd(0, cwd, sizeof cwd) > 0 && strcmp(cwd, "/d") == 0,
          "getcwd(scratch) == /d");
    CHECK(write_file("x", "1") == 1, "write bare x (into cwd d)");
    CHECK(fs_exists("x") == 1, "x exists relative to cwd");
    CHECK(fs_exists("/d/x") == 1, "same file is /d/x from the root");
    CHECK(fs_exists("/hello") == 1, "absolute /hello still reaches the root");
    CHECK(fs_exists("hello") == 0, "bare hello no longer resolves from cwd d");

    /* rename within the scratch mount (both ends root-absolute, same base). */
    CHECK(fs_rename("/hello", "/hello2") == 0, "rename /hello -> /hello2");
    CHECK(fs_exists("/hello2") == 1 && fs_exists("/hello") == 0, "rename moved it");

    /* Switch to the host mount; its cwd is independent of scratch's. */
    CHECK(fs_mount_set(1) == 0, "set current mount to host");
    CHECK(fs_mount_current() == 1, "current mount is host");
    CHECK(write_file("hfile", "hi") == 2, "write bare hfile on host");
    CHECK(fs_exists("hfile") == 1, "hfile exists via fs");
    {
        char real[256];
        struct stat st;
        snprintf(real, sizeof real, "%s/hfile", hostdir);
        CHECK(stat(real, &st) == 0 && st.st_size == 2,
              "hfile is really on the host fs");
    }

    /* fs_stat / fs_fstat reach the same host object by two routes (dev,ino). */
    {
        struct fs_stat sp, sf;
        int fd;
        CHECK(fs_stat("hfile", &sp) == 0 &&
              (sp.mode & FS_S_IFMT) == FS_S_IFREG && sp.size == 2,
              "fs_stat hfile on host: regular, size 2");
        fd = fs_open("hfile", FS_O_RDONLY, 0);
        CHECK(fd >= FS_FD_BASE, "open hfile on host");
        CHECK(fs_fstat(fd, &sf) == 0 && sf.ino == sp.ino && sf.dev == sp.dev,
              "host fs_fstat matches fs_stat (dev,ino)");
        fs_close(fd);
    }

    /* hostfs confinement: a .. escape is refused. */
    rc = fs_open("host:../escape", FS_O_RDONLY, 0);
    CHECK(rc < 0, "host:../escape is denied");
    CHECK(fs_exists("host:../etc") < 0, "host:../etc stat is denied");

    /* Scratch cwd survived the mount switch (DOS per-drive cwd). */
    CHECK(fs_getcwd(0, cwd, sizeof cwd) > 0 && strcmp(cwd, "/d") == 0,
          "scratch cwd still /d after switching mounts");

    /* Bad index rejected. */
    CHECK(fs_mount_set(99) < 0, "set to a bad index fails");

    /* Clean up the host temp tree. */
    {
        char p[256];
        snprintf(p, sizeof p, "%s/hfile", hostdir);
        unlink(p);
        rmdir(hostdir);
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
