/* Unit test for the fs/ mount-table + namespace layer, driven directly from C
 * (no language runtime).  Builds a two-mount table by hand the way a launcher
 * would (ramfs scratch at index 0, a hostfs mount at index 1 over a temp dir)
 * and exercises resolution, the DOS current-mount + per-mount-cwd state, the
 * ramfs backend, and hostfs confinement. */

#define _GNU_SOURCE
#include "fs.h"
#include "ramfs.h"
#include "hostfs.h"
#include "devfs.h"
#include "romfs.h"
#include "ninefs.h"
#include "p9_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures;

/* A transport that counts doorbell rings (rpc calls) and delegates to the real
 * server, so the 9P batching is a tested property: a stat or an open must cost
 * one ring, not the three or two it was before the composites were batched. */
static struct p9_transport p9_real;
static int                  p9_rings;

static int
counting_rpc(void *ctx, const uint8_t *tx, uint32_t txlen, uint8_t *rx,
             uint32_t rxcap)
{
    (void)ctx;
    p9_rings++;
    return p9_real.rpc(p9_real.ctx, tx, txlen, rx, rxcap);
}

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
    int rc, t, h, d, dfd, saw_a, saw_b, count;

    if (!hostdir) {
        perror("mkdtemp");
        return 2;
    }

    /* Launcher-style table setup: scratch ramfs at index 0, host at index 1. */
    t = ramfs_mount("tmp", 0);
    h = hostfs_mount("host", hostdir, 0);
    d = devfs_mount("dev", 0);
    CHECK(t == 0, "ramfs scratch is mount index 0");
    CHECK(h == 1, "hostfs is mount index 1");
    CHECK(d == 2, "devfs is mount index 2");

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

    /* ROOT: is the case-insensitive alias for the default root (index 0, here
     * the "tmp" scratch mount): it reaches the same file whatever case, and a
     * name that is not a real mount nor the alias still fails. */
    CHECK(fs_exists("ROOT:hello") == 1, "ROOT:hello exists via alias");
    CHECK(fs_exists("root:hello") == 1, "root:hello exists (case-insensitive)");
    CHECK(fs_exists("Root:hello") == 1, "Root:hello exists (case-insensitive)");
    CHECK(fs_mount_find("ROOT") == 0, "find ROOT -> 0 (default root alias)");
    CHECK(fs_mount_find("root") == 0, "find root -> 0 (case-insensitive)");
    CHECK(fs_exists("nope:hello") < 0, "nope: is not a mount nor the alias");

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

    /* devfs: the fixed character devices. */
    {
        int fd, n;
        struct fs_stat st;
        char zbuf[8];

        CHECK(fs_isdir("dev:") == 1, "dev: root is a directory");
        CHECK(fs_exists("dev:null") == 1, "dev:null exists");
        CHECK(fs_exists("dev:zero") == 1, "dev:zero exists");
        CHECK(fs_exists("dev:full") == 1, "dev:full exists");
        CHECK(fs_exists("dev:nope") == 0, "dev:nope does not exist");

        /* null: writes vanish, reads hit EOF. */
        fd = fs_open("dev:null", FS_O_WRONLY, 0);
        CHECK(fd >= 0, "open dev:null");
        CHECK(fs_write(fd, "discard me", 10) == 10, "write to dev:null is swallowed");
        fs_close(fd);
        fd = fs_open("dev:null", FS_O_RDONLY, 0);
        CHECK(fs_read(fd, buf, sizeof buf) == 0, "read dev:null is EOF");
        fs_close(fd);

        /* zero: an endless run of zero bytes. */
        fd = fs_open("dev:zero", FS_O_RDONLY, 0);
        memset(zbuf, 0xAA, sizeof zbuf);
        n = fs_read(fd, zbuf, sizeof zbuf);
        CHECK(n == (int)sizeof zbuf, "read dev:zero fills the buffer");
        CHECK(zbuf[0] == 0 && zbuf[7] == 0, "dev:zero yields zero bytes");
        fs_close(fd);

        /* full: reads as zero, writes fail with ENOSPC. */
        fd = fs_open("dev:full", FS_O_RDWR, 0);
        CHECK(fs_read(fd, zbuf, 4) == 4, "read dev:full yields zeros");
        CHECK(fs_write(fd, "x", 1) < 0, "write to dev:full fails (ENOSPC)");
        fs_close(fd);

        /* a device is a regular file; the mount is immutable. */
        CHECK(fs_stat("dev:zero", &st) == 0 && (st.mode & FS_S_IFMT) == FS_S_IFREG,
              "dev:zero stats as a regular file");
        CHECK(fs_mkdir("dev:sub") < 0, "mkdir on devfs is rejected");
        CHECK(fs_unlink("dev:null") < 0, "unlink on devfs is rejected");

        /* the directory enumerates exactly the three devices. */
        dfd = fs_open("dev:", FS_O_DIRECTORY, 0);
        CHECK(dfd >= 0, "open dev: as a directory");
        count = 0;
        {
            struct fs_dirent de;
            while (fs_read(dfd, &de, (int)sizeof de) == (int)sizeof de)
                count++;
        }
        CHECK(count == 3, "devfs lists three devices");
        fs_close(dfd);
    }

    /* romfs: pack a temp directory tree into an image, mount it read-only. */
    {
        char romtmpl[] = "/tmp/skjromsrcXXXXXX";
        char *romsrc = mkdtemp(romtmpl);
        char sub[128];
        void *img = 0;
        size_t imglen = 0;
        int rm, fd;
        FILE *wf;

        CHECK(romsrc != 0, "romfs: made a source dir");
        if (romsrc) {
            snprintf(sub, sizeof sub, "%s/sub", romsrc);
            mkdir(sub, 0755);
            snprintf(buf, sizeof buf, "%s/greet", romsrc);
            wf = fopen(buf, "wb"); fputs("hello romfs", wf); fclose(wf);
            snprintf(buf, sizeof buf, "%s/sub/inner", romsrc);
            wf = fopen(buf, "wb"); fputs("nested", wf); fclose(wf);

            rc = romfs_build_dir(romsrc, &img, &imglen);
            CHECK(rc == 0 && img && imglen > 0, "romfs: built an image");
            rm = romfs_mount("rom", img, imglen, 0);
            CHECK(rm == 3, "romfs is mount index 3");

            CHECK(fs_exists("rom:greet") == 1, "rom:greet exists");
            CHECK(fs_isdir("rom:sub") == 1, "rom:sub is a directory");
            CHECK(fs_exists("rom:sub/inner") == 1, "rom:sub/inner exists");
            CHECK(read_file("rom:greet", buf, sizeof buf) == 11
                  && strcmp(buf, "hello romfs") == 0, "read rom:greet back");
            CHECK(read_file("rom:sub/inner", buf, sizeof buf) == 6
                  && strcmp(buf, "nested") == 0, "read rom:sub/inner back");
            CHECK(fs_size("rom:greet") == 11, "rom:greet size is 11");

            /* read-only: no write open, no create, no mutation. */
            fd = fs_open("rom:greet", FS_O_WRONLY, 0);
            CHECK(fd < 0, "write-open rom:greet is rejected (EROFS)");
            CHECK(fs_open("rom:new", FS_O_WRONLY | FS_O_CREAT, 0644) < 0,
                  "create on romfs is rejected");
            CHECK(fs_mkdir("rom:d2") < 0, "mkdir on romfs is rejected");
            CHECK(fs_unlink("rom:greet") < 0, "unlink on romfs is rejected");

            /* the root directory lists greet and sub. */
            dfd = fs_open("rom:", FS_O_DIRECTORY, 0);
            CHECK(dfd >= 0, "open rom: as a directory");
            saw_a = saw_b = count = 0;
            {
                struct fs_dirent de;
                while (fs_read(dfd, &de, (int)sizeof de) == (int)sizeof de) {
                    if (strcmp(de.name, "greet") == 0) saw_a = 1;
                    if (strcmp(de.name, "sub") == 0) saw_b = 1;
                    count++;
                }
            }
            CHECK(saw_a && saw_b && count == 2, "romfs root lists greet and sub");
            fs_close(dfd);
            free(img);

            snprintf(buf, sizeof buf, "%s/sub/inner", romsrc); unlink(buf);
            snprintf(buf, sizeof buf, "%s/greet", romsrc); unlink(buf);
            rmdir(sub);
            rmdir(romsrc);
        }
    }

    /* 9pfs: an in-process 9P2000 server over a temp host dir, reached by the
     * ninefs client through the server transport. */
    {
        char nptmpl[] = "/tmp/skjp9srvXXXXXX";
        char *nproot = mkdtemp(nptmpl);
        struct p9_transport tr;
        int np, nfd;

        CHECK(nproot != 0, "9pfs: made a server root dir");
        if (nproot && p9_server_open(nproot, &p9_real) == 0) {
            tr.rpc = counting_rpc;       /* count rings, delegate to p9_real */
            tr.ctx = 0;
            np = ninefs_mount("9p", &tr, 0);
            CHECK(np == 4, "9pfs is mount index 4 (version+attach ok)");

            /* Create and read back through the client. */
            CHECK(write_file("9p:hello.txt", "hi 9p") == 5, "9pfs: wrote hello.txt");
            CHECK(fs_exists("9p:hello.txt") == 1, "9p:hello.txt exists");
            CHECK(read_file("9p:hello.txt", buf, sizeof buf) == 5
                  && strcmp(buf, "hi 9p") == 0, "9pfs: read hello.txt back");
            CHECK(fs_size("9p:hello.txt") == 5, "9p:hello.txt size is 5");

            /* A subdirectory and a nested file. */
            CHECK(fs_mkdir("9p:docs") == 0, "9pfs: mkdir docs");
            CHECK(fs_isdir("9p:docs") == 1, "9p:docs is a directory");
            CHECK(write_file("9p:docs/note", "inner") == 5, "9pfs: wrote docs/note");
            CHECK(read_file("9p:docs/note", buf, sizeof buf) == 5
                  && strcmp(buf, "inner") == 0, "9pfs: read docs/note back");

            /* Stat reports a regular file with the right mode class. */
            {
                struct fs_stat st;
                CHECK(fs_stat("9p:hello.txt", &st) == 0
                      && (st.mode & FS_S_IFMT) == FS_S_IFREG && st.size == 5,
                      "9pfs: stat hello.txt is a 5-byte regular file");
            }

            /* The batching is a tested property: a stat (Twalk+Tstat+Tclunk)
             * and an open (Twalk+Topen) each cost exactly one doorbell ring. */
            {
                struct fs_stat st;
                int ofd;
                p9_rings = 0;
                fs_stat("9p:hello.txt", &st);
                CHECK(p9_rings == 1, "9pfs: stat is one ring (walk+stat+clunk)");
                p9_rings = 0;
                ofd = fs_open("9p:hello.txt", FS_O_RDONLY, 0);
                CHECK(p9_rings == 1, "9pfs: open is one ring (walk+open)");
                if (ofd >= 0) fs_close(ofd);
            }

            /* The root directory lists hello.txt and docs. */
            nfd = fs_open("9p:", FS_O_DIRECTORY, 0);
            CHECK(nfd >= 0, "9pfs: open root as a directory");
            saw_a = saw_b = count = 0;
            {
                struct fs_dirent de;
                while (fs_read(nfd, &de, (int)sizeof de) == (int)sizeof de) {
                    if (strcmp(de.name, "hello.txt") == 0) saw_a = 1;
                    if (strcmp(de.name, "docs") == 0) saw_b = 1;
                    count++;
                }
            }
            CHECK(saw_a && saw_b && count == 2, "9pfs root lists hello.txt and docs");
            fs_close(nfd);

            /* Rename within a directory, then remove. */
            CHECK(fs_rename("9p:hello.txt", "9p:greeting") == 0, "9pfs: rename hello.txt");
            CHECK(fs_exists("9p:hello.txt") == 0, "9p:hello.txt is gone after rename");
            CHECK(fs_exists("9p:greeting") == 1, "9p:greeting exists after rename");
            CHECK(fs_unlink("9p:greeting") == 0, "9pfs: unlink greeting");
            CHECK(fs_exists("9p:greeting") == 0, "9p:greeting is gone after unlink");
            CHECK(fs_unlink("9p:docs/note") == 0, "9pfs: unlink docs/note");
            CHECK(fs_rmdir("9p:docs") == 0, "9pfs: rmdir docs");

            /* A truncated T-message must not act on zeroed fields.  Feed the
             * server a header-only Tclunk and Tversion (every body field is
             * past the end, so the parse cursor latches not-ok): the reply must
             * be Rerror, not an Rclunk that clunked fid 0 or an Rversion that
             * reset the session. */
            {
                struct p9_server *bad = p9_server_create(nproot);
                uint8_t rx[64];
                int n;
                uint8_t tclunk[7] = { 7, 0, 0, 0, P9_TCLUNK, 0, 0 };
                uint8_t tver[7]   = { 7, 0, 0, 0, P9_TVERSION, 0, 0 };
                CHECK(bad != 0, "9pfs: made a raw server for malformed frames");
                if (bad) {
                    n = p9_server_handle(bad, tclunk, sizeof tclunk, rx, sizeof rx);
                    CHECK(n >= 5 && rx[4] == P9_RERROR,
                          "9pfs: truncated Tclunk is rejected, not acted on");
                    n = p9_server_handle(bad, tver, sizeof tver, rx, sizeof rx);
                    CHECK(n >= 5 && rx[4] == P9_RERROR,
                          "9pfs: truncated Tversion is rejected, not acted on");
                    p9_server_destroy(bad);
                }
            }

            p9_server_close(&p9_real);
            rmdir(nproot);
        }
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
