/* ninefs: the 9P2000 client backend (see ninefs.h).
 *
 * Each mount holds a transport and a small fid allocator.  fid 0 is the
 * attached root (the mount root handle); transient fids for stat/walk and the
 * fids behind open files and directories are drawn from a bitmap.  A path op
 * clones the root with Twalk, works, and clunks.  All exchanges reuse one
 * module scratch buffer: the fs layer is single-threaded with one request
 * outstanding, so a shared buffer keeps per-call stack frames small (a guest
 * stack is modest) and needs no per-mount allocation. */

#include "ninefs.h"
#include "p9_wire.h"
#include "fs_internal.h"

#include <string.h>
#include <errno.h>

#define P9_MAXFID 64                    /* fids per mount: 0 (root) + 1..63    */
#define P9_ROOTFID 0u

struct p9_mount {
    struct p9_transport t;
    uint64_t            fids;           /* bit i set == fid i in use           */
};

static struct p9_mount mounts[FS_MAX_MOUNTS];

/* One shared message buffer pair, reused by every exchange (single request at
 * a time across all mounts). */
static uint8_t txbuf[P9_MSIZE];
static uint8_t rxbuf[P9_MSIZE];

static struct p9_mount *
nm_of(struct fs_mount *m)
{
    return &mounts[(int)(m - fs_mounts)];
}

static int
fid_alloc(struct p9_mount *nm)
{
    int i;
    for (i = 1; i < P9_MAXFID; i++)
        if (!(nm->fids & ((uint64_t)1 << i))) {
            nm->fids |= (uint64_t)1 << i;
            return i;
        }
    return -EMFILE;
}

static void
fid_free(struct p9_mount *nm, int fid)
{
    if (fid > 0 && fid < P9_MAXFID)
        nm->fids &= ~((uint64_t)1 << fid);
}

/* Send one framed T-message and receive the R-message.  Returns the reply type
 * (>=0), with `in` positioned just past the size/type/tag header, or a negative
 * errno.  A Rerror reply maps to -ENOENT (the common case, a missing path);
 * base 9P2000 carries only an error string, not an errno. */
static int
p9_txn(struct p9_transport *t, struct p9_out *o, struct p9_in *in)
{
    uint32_t sz = p9o_finish(o);
    int rlen, type;

    if (sz == 0)
        return -EINVAL;                 /* request overflowed the buffer */
    rlen = t->rpc(t->ctx, txbuf, sz, rxbuf, P9_MSIZE);
    if (rlen < 0)
        return rlen;
    p9i_init(in, rxbuf, (uint32_t)rlen);
    (void)p9i_u32(in);                  /* size  */
    type = p9i_u8(in);                  /* type  */
    (void)p9i_u16(in);                  /* tag   */
    if (!in->ok)
        return -EIO;
    if (type == P9_RERROR)
        return -ENOENT;
    return type;
}

static int
p9_clunk(struct p9_mount *nm, int fid)
{
    struct p9_out o;
    struct p9_in in;
    int type;

    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TCLUNK, 0);
    p9o_u32(&o, (uint32_t)fid);
    type = p9_txn(&nm->t, &o, &in);
    fid_free(nm, fid);
    return type == P9_RCLUNK ? 0 : (type < 0 ? type : -EIO);
}

/* Count the components of `rel` (separators squeezed, "" and "." skipped). */
static int
split_count(const char *rel)
{
    int n = 0;
    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        if (!(p[0] == '.' && (p[1] == '/' || p[1] == 0)))
            n++;
        while (*p && *p != '/') p++;
    }
    return n;
}

/* Fetch the i-th component of `rel` into `out` (cap bytes); returns its length
 * or -1 if out of range. */
static int
split_at(const char *rel, int idx, char *out, int cap)
{
    int n = 0;
    const char *p = rel;
    while (*p) {
        const char *s;
        int len;
        while (*p == '/') p++;
        if (!*p) break;
        s = p;
        while (*p && *p != '/') p++;
        len = (int)(p - s);
        if (len == 1 && s[0] == '.')
            continue;
        if (n == idx) {
            if (len > cap - 1) len = cap - 1;
            memcpy(out, s, (size_t)len);
            out[len] = 0;
            return len;
        }
        n++;
    }
    return -1;
}

/* Clone the root and walk `rel` into a fresh fid.  Returns the new fid, or a
 * negative errno (the new fid is already freed on failure). */
static int
walk_rel(struct p9_mount *nm, const char *rel, int *qtype)
{
    int newfid = fid_alloc(nm);
    int ncomp = split_count(rel);
    int off = 0, first = 1;
    struct p9_qid q;

    q.type = P9_QTDIR;
    if (newfid < 0)
        return newfid;

    do {
        struct p9_out o;
        struct p9_in in;
        int k = ncomp - off;
        int i, nq, type;

        if (k > P9_MAXWELEM) k = P9_MAXWELEM;
        p9o_init(&o, txbuf, sizeof txbuf);
        p9o_hdr(&o, P9_TWALK, 0);
        p9o_u32(&o, first ? P9_ROOTFID : (uint32_t)newfid);
        p9o_u32(&o, (uint32_t)newfid);
        p9o_u16(&o, (uint16_t)k);
        for (i = 0; i < k; i++) {
            char comp[FS_DIRENT_NAMEMAX];
            split_at(rel, off + i, comp, (int)sizeof comp);
            p9o_str(&o, comp);
        }
        type = p9_txn(&nm->t, &o, &in);
        if (type < 0) { fid_free(nm, newfid); return type; }
        if (type != P9_RWALK) { fid_free(nm, newfid); return -EIO; }
        nq = p9i_u16(&in);
        for (i = 0; i < nq; i++)
            p9i_qid(&in, &q);
        if (!in.ok || nq != k) {         /* a short walk never bound newfid */
            fid_free(nm, newfid);
            return -ENOENT;
        }
        off += k;
        first = 0;
    } while (off < ncomp);

    if (qtype)
        *qtype = q.type;
    return newfid;
}

/* The directory part of `rel` (everything before the last component) and its
 * last component.  `dir` holds the parent path (may be ""), returns last len. */
static int
split_last(const char *rel, char *dir, int dcap, char *last, int lcap)
{
    int ncomp = split_count(rel);
    int i, dn = 0;

    if (ncomp == 0) { dir[0] = 0; last[0] = 0; return 0; }
    dir[0] = 0;
    for (i = 0; i < ncomp - 1; i++) {
        char comp[FS_DIRENT_NAMEMAX];
        int cl = split_at(rel, i, comp, (int)sizeof comp);
        if (dn && dn < dcap - 1) dir[dn++] = '/';
        if (cl > dcap - 1 - dn) cl = dcap - 1 - dn;
        memcpy(dir + dn, comp, (size_t)cl);
        dn += cl;
    }
    dir[dn] = 0;
    return split_at(rel, ncomp - 1, last, lcap);
}

static uint8_t
p9_open_mode(int flags)
{
    uint8_t m;
    switch (flags & FS_O_ACCMODE) {
    case FS_O_WRONLY: m = P9_OWRITE; break;
    case FS_O_RDWR:   m = P9_ORDWR;  break;
    default:          m = P9_OREAD;  break;
    }
    if (flags & FS_O_TRUNC)
        m |= P9_OTRUNC;
    return m;
}

/* Topen an already-walked fid.  Returns 0 or a negative errno. */
static int
do_open(struct p9_mount *nm, int fid, uint8_t mode)
{
    struct p9_out o;
    struct p9_in in;
    int type;

    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TOPEN, 0);
    p9o_u32(&o, (uint32_t)fid);
    p9o_u8(&o, mode);
    type = p9_txn(&nm->t, &o, &in);
    return type == P9_ROPEN ? 0 : (type < 0 ? type : -EIO);
}

/* Tcreate a child `name` in the directory `dir` (an unopened fid, which becomes
 * the open new file).  Returns 0 or a negative errno. */
static int
do_create(struct p9_mount *nm, int dirfid, const char *name, uint32_t perm,
          uint8_t mode)
{
    struct p9_out o;
    struct p9_in in;
    int type;

    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TCREATE, 0);
    p9o_u32(&o, (uint32_t)dirfid);
    p9o_str(&o, name);
    p9o_u32(&o, perm);
    p9o_u8(&o, mode);
    type = p9_txn(&nm->t, &o, &in);
    return type == P9_RCREATE ? 0 : (type < 0 ? type : -EACCES);
}

static intptr_t
nine_open(struct fs_mount *m, intptr_t base, const char *rel, int flags, int mode)
{
    struct p9_mount *nm = nm_of(m);
    int fid, qtype = P9_QTFILE, rc;
    (void)base; (void)mode;

    if (flags & FS_O_DIRECTORY) {
        fid = walk_rel(nm, rel, &qtype);
        if (fid < 0)
            return fid;
        if (!(qtype & P9_QTDIR)) { p9_clunk(nm, fid); return -ENOTDIR; }
        rc = do_open(nm, fid, P9_OREAD);
        if (rc < 0) { p9_clunk(nm, fid); return rc; }
        return fid;
    }

    fid = walk_rel(nm, rel, &qtype);
    if (fid >= 0) {                      /* the file exists */
        if (qtype & P9_QTDIR) { p9_clunk(nm, fid); return -EISDIR; }
        rc = do_open(nm, fid, p9_open_mode(flags));
        if (rc < 0) { p9_clunk(nm, fid); return rc; }
        return fid;
    }
    if (fid != -ENOENT || !(flags & FS_O_CREAT))
        return fid;

    /* Create: walk to the parent directory, then Tcreate the last component. */
    {
        char dir[256], last[FS_DIRENT_NAMEMAX];
        int pfid;
        if (split_last(rel, dir, (int)sizeof dir, last, (int)sizeof last) <= 0)
            return -EINVAL;
        pfid = walk_rel(nm, dir, &qtype);
        if (pfid < 0)
            return pfid;
        rc = do_create(nm, pfid, last, 0644, p9_open_mode(flags));
        if (rc < 0) { p9_clunk(nm, pfid); return rc; }
        return pfid;                     /* pfid is now the open new file */
    }
}

static int
nine_read(struct fs_mount *m, intptr_t h, void *buf, int n, int64_t *pos)
{
    struct p9_mount *nm = nm_of(m);
    struct p9_out o;
    struct p9_in in;
    int type;
    uint32_t cnt;

    if (n <= 0)
        return 0;
    if ((uint32_t)n > P9_MSIZE - 32)
        n = P9_MSIZE - 32;               /* leave room for the Rread header */
    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TREAD, 0);
    p9o_u32(&o, (uint32_t)h);
    p9o_u64(&o, (uint64_t)*pos);
    p9o_u32(&o, (uint32_t)n);
    type = p9_txn(&nm->t, &o, &in);
    if (type < 0)
        return type;
    if (type != P9_RREAD)
        return -EIO;
    cnt = p9i_u32(&in);
    if (!in.ok || in.p + cnt > in.len)
        return -EIO;
    if (cnt > (uint32_t)n)
        cnt = (uint32_t)n;
    memcpy(buf, in.b + in.p, cnt);
    *pos += cnt;
    return (int)cnt;
}

static int
nine_write(struct fs_mount *m, intptr_t h, const void *buf, int n, int64_t *pos)
{
    struct p9_mount *nm = nm_of(m);
    struct p9_out o;
    struct p9_in in;
    int type;
    uint32_t cnt;

    if (n <= 0)
        return 0;
    if ((uint32_t)n > P9_MSIZE - 32)
        n = P9_MSIZE - 32;
    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TWRITE, 0);
    p9o_u32(&o, (uint32_t)h);
    p9o_u64(&o, (uint64_t)*pos);
    p9o_u32(&o, (uint32_t)n);
    p9o_bytes(&o, buf, (uint32_t)n);
    type = p9_txn(&nm->t, &o, &in);
    if (type < 0)
        return type;
    if (type != P9_RWRITE)
        return -EIO;
    cnt = p9i_u32(&in);
    if (!in.ok)
        return -EIO;
    *pos += cnt;
    return (int)cnt;
}

static int64_t
nine_lseek(struct fs_mount *m, intptr_t h, int64_t off, int whence, int64_t *pos)
{
    int64_t np;
    (void)m; (void)h;
    switch (whence) {
    case FS_SEEK_SET: np = off; break;
    case FS_SEEK_CUR: np = *pos + off; break;
    default: return -EINVAL;              /* SEEK_END needs a server round-trip */
    }
    if (np < 0)
        return -EINVAL;
    *pos = np;
    return np;
}

/* Parse one 9P2000 stat entry at `in`'s cursor: fill name/mode/length and
 * advance past the whole record.  Returns 0 or -EIO. */
static int
parse_stat(struct p9_in *in, char *name, int ncap, uint32_t *mode, uint64_t *len)
{
    uint32_t stsz = p9i_u16(in);         /* bytes following this size field */
    uint32_t end = in->p + stsz;
    struct p9_qid q;

    if (!in->ok || end > in->len)
        return -EIO;
    (void)p9i_u16(in);                   /* type   */
    (void)p9i_u32(in);                   /* dev    */
    p9i_qid(in, &q);                     /* qid    */
    *mode = p9i_u32(in);                 /* mode   */
    (void)p9i_u32(in);                   /* atime  */
    (void)p9i_u32(in);                   /* mtime  */
    *len = p9i_u64(in);                  /* length */
    p9i_str(in, name, (uint32_t)ncap);   /* name   */
    in->p = end;                         /* skip uid/gid/muid */
    return in->ok ? 0 : -EIO;
}

static int
nine_readdir(struct fs_mount *m, intptr_t h, int64_t *cursor, struct fs_dirent *de)
{
    struct p9_mount *nm = nm_of(m);
    struct p9_out o;
    struct p9_in in;
    int type, rc;
    uint32_t cnt, mode;
    uint64_t flen;
    uint32_t want = 512;

    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TREAD, 0);
    p9o_u32(&o, (uint32_t)h);
    p9o_u64(&o, (uint64_t)*cursor);
    p9o_u32(&o, want);
    type = p9_txn(&nm->t, &o, &in);
    if (type < 0)
        return type;
    if (type != P9_RREAD)
        return -EIO;
    cnt = p9i_u32(&in);
    if (!in.ok || cnt == 0 || in.p + cnt > in.len)
        return 0;                        /* end of directory */
    {
        uint32_t startp = in.p;
        rc = parse_stat(&in, de->name, FS_DIRENT_NAMEMAX, &mode, &flen);
        if (rc < 0)
            return rc;
        de->type = (mode & P9_DMDIR) ? FS_DT_DIR : FS_DT_REG;
        de->ino = 0;
        de->pad[0] = de->pad[1] = de->pad[2] = 0;
        *cursor += (int64_t)(in.p - startp);
    }
    return 1;
}

static int
nine_close(struct fs_mount *m, intptr_t h, int is_dir)
{
    (void)is_dir;
    return p9_clunk(nm_of(m), (int)h);
}

/* Walk `rel`, Tstat it, clunk, and hand the decoded fields back.  A missing
 * path returns -ENOENT (from the walk). */
static int
stat_rel(struct p9_mount *nm, const char *rel, uint32_t *mode, uint64_t *len)
{
    struct p9_out o;
    struct p9_in in;
    int fid = walk_rel(nm, rel, 0);
    int type, rc;
    char name[FS_DIRENT_NAMEMAX];

    if (fid < 0)
        return fid;
    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TSTAT, 0);
    p9o_u32(&o, (uint32_t)fid);
    type = p9_txn(&nm->t, &o, &in);
    if (type != P9_RSTAT) {
        p9_clunk(nm, fid);
        return type < 0 ? type : -EIO;
    }
    (void)p9i_u16(&in);                  /* outer stat count */
    rc = parse_stat(&in, name, (int)sizeof name, mode, len);
    p9_clunk(nm, fid);
    return rc;
}

static int
nine_exists(struct fs_mount *m, intptr_t base, const char *rel)
{
    uint32_t mode;
    uint64_t len;
    (void)base;
    return stat_rel(nm_of(m), rel, &mode, &len) == 0 ? 1 : 0;
}

static int64_t
nine_size(struct fs_mount *m, intptr_t base, const char *rel)
{
    uint32_t mode;
    uint64_t len;
    int rc = stat_rel(nm_of(m), rel, &mode, &len);
    (void)base;
    if (rc < 0)
        return rc;
    return (mode & P9_DMDIR) ? 0 : (int64_t)len;
}

static int
nine_isdir(struct fs_mount *m, intptr_t base, const char *rel)
{
    uint32_t mode;
    uint64_t len;
    (void)base;
    if (stat_rel(nm_of(m), rel, &mode, &len) < 0)
        return 0;
    return (mode & P9_DMDIR) ? 1 : 0;
}

static void
fill_stat(uint32_t mode, uint64_t len, struct fs_stat *st)
{
    int isdir = (mode & P9_DMDIR) != 0;
    st->dev = 0x3970;                    /* '9p': a constant 9pfs device id */
    st->ino = 0;
    st->size = isdir ? 0 : (int64_t)len;
    st->mode = (isdir ? FS_S_IFDIR : FS_S_IFREG) | (mode & 0777u);
    st->nlink = 1;
}

static int
nine_stat(struct fs_mount *m, intptr_t base, const char *rel, struct fs_stat *st)
{
    uint32_t mode;
    uint64_t len;
    int rc = stat_rel(nm_of(m), rel, &mode, &len);
    (void)base;
    if (rc < 0)
        return rc;
    fill_stat(mode, len, st);
    return 0;
}

static int
nine_fstat(struct fs_mount *m, intptr_t h, int is_dir, struct fs_stat *st)
{
    struct p9_mount *nm = nm_of(m);
    struct p9_out o;
    struct p9_in in;
    int type, rc;
    uint32_t mode;
    uint64_t len;
    char name[FS_DIRENT_NAMEMAX];
    (void)is_dir;

    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TSTAT, 0);
    p9o_u32(&o, (uint32_t)h);
    type = p9_txn(&nm->t, &o, &in);
    if (type != P9_RSTAT)
        return type < 0 ? type : -EIO;
    (void)p9i_u16(&in);
    rc = parse_stat(&in, name, (int)sizeof name, &mode, &len);
    if (rc < 0)
        return rc;
    fill_stat(mode, len, st);
    return 0;
}

/* Tremove works on both files and directories; it removes and clunks the fid
 * (even on error), so the fid is freed here without a separate Tclunk. */
static int
remove_rel(struct p9_mount *nm, const char *rel)
{
    struct p9_out o;
    struct p9_in in;
    int fid = walk_rel(nm, rel, 0);
    int type;

    if (fid < 0)
        return fid;
    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TREMOVE, 0);
    p9o_u32(&o, (uint32_t)fid);
    type = p9_txn(&nm->t, &o, &in);
    fid_free(nm, fid);
    return type == P9_RREMOVE ? 0 : (type < 0 ? type : -EACCES);
}

static int
nine_unlink(struct fs_mount *m, intptr_t base, const char *rel)
{
    (void)base;
    return remove_rel(nm_of(m), rel);
}

static int
nine_rmdir(struct fs_mount *m, intptr_t base, const char *rel)
{
    (void)base;
    return remove_rel(nm_of(m), rel);
}

static int
nine_mkdir(struct fs_mount *m, intptr_t base, const char *rel)
{
    struct p9_mount *nm = nm_of(m);
    char dir[256], last[FS_DIRENT_NAMEMAX];
    int pfid, rc;
    (void)base;

    if (split_last(rel, dir, (int)sizeof dir, last, (int)sizeof last) <= 0)
        return -EINVAL;
    pfid = walk_rel(nm, dir, 0);
    if (pfid < 0)
        return pfid;
    rc = do_create(nm, pfid, last, P9_DMDIR | 0755u, P9_OREAD);
    p9_clunk(nm, pfid);                  /* clunk the open new directory */
    return rc;
}

/* Rename within one directory: 9P2000 does it by changing the name field with
 * Twstat.  fs.c guarantees `from` and `to` share the same base; require the
 * same parent directory too (a cross-directory move needs 9P2000.u). */
static int
nine_rename(struct fs_mount *m, intptr_t base, const char *from, const char *to)
{
    struct p9_mount *nm = nm_of(m);
    char fdir[256], flast[FS_DIRENT_NAMEMAX];
    char tdir[256], tlast[FS_DIRENT_NAMEMAX];
    struct p9_out o;
    struct p9_in in;
    int fid, type;
    uint32_t stsz, pos;
    (void)base;

    split_last(from, fdir, (int)sizeof fdir, flast, (int)sizeof flast);
    if (split_last(to, tdir, (int)sizeof tdir, tlast, (int)sizeof tlast) <= 0)
        return -EINVAL;
    if (strcmp(fdir, tdir) != 0)
        return -EXDEV;                   /* different directory: unsupported */

    fid = walk_rel(nm, from, 0);
    if (fid < 0)
        return fid;

    /* Twstat: fid[4] n[2] stat.  A "don't touch" stat sets every fixed field to
     * its empty sentinel (all-ones) and leaves only `name`.  The stat's own
     * size[2] counts the bytes after it; n[2] counts the stat plus that size. */
    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TWSTAT, 0);
    p9o_u32(&o, (uint32_t)fid);
    pos = o.n;
    p9o_u16(&o, 0);                      /* n: patched below   */
    p9o_u16(&o, 0);                      /* stat size: patched */
    p9o_u16(&o, 0xffff);                 /* type   (no change) */
    p9o_u32(&o, 0xffffffffu);            /* dev    (no change) */
    p9o_u8(&o, 0xff);                    /* qid.type           */
    p9o_u32(&o, 0xffffffffu);            /* qid.version        */
    p9o_u64(&o, 0xffffffffffffffffull);  /* qid.path           */
    p9o_u32(&o, 0xffffffffu);            /* mode   (no change) */
    p9o_u32(&o, 0xffffffffu);            /* atime  (no change) */
    p9o_u32(&o, 0xffffffffu);            /* mtime  (no change) */
    p9o_u64(&o, 0xffffffffffffffffull);  /* length (no change) */
    p9o_str(&o, tlast);                  /* name: the rename   */
    p9o_str(&o, "");                     /* uid    */
    p9o_str(&o, "");                     /* gid    */
    p9o_str(&o, "");                     /* muid   */
    if (!o.ok) { p9_clunk(nm, fid); return -EINVAL; }
    stsz = o.n - (pos + 2);              /* bytes after n[2] = the whole stat  */
    o.b[pos]     = (uint8_t)((o.n - pos - 2) & 0xff);
    o.b[pos + 1] = (uint8_t)(((o.n - pos - 2) >> 8) & 0xff);
    o.b[pos + 2] = (uint8_t)((stsz - 2) & 0xff);     /* stat size: bytes after it */
    o.b[pos + 3] = (uint8_t)(((stsz - 2) >> 8) & 0xff);
    type = p9_txn(&nm->t, &o, &in);
    p9_clunk(nm, fid);
    return type == P9_RWSTAT ? 0 : (type < 0 ? type : -EACCES);
}

const struct fs_backend ninefs_backend = {
    FS_KIND_9P, "9pfs",
    nine_open, nine_read, nine_write, nine_lseek, nine_readdir, nine_close,
    nine_exists, nine_size, nine_isdir, nine_stat, nine_fstat,
    nine_unlink, nine_rename, nine_mkdir, nine_rmdir,
};

/* Tversion then Tattach (root -> fid 0, no authentication) over `t`.  Returns 0
 * or a negative errno; leaves the session ready with fid 0 bound to the root. */
static int
p9_handshake(const struct p9_transport *t)
{
    struct p9_transport local = *t;
    struct p9_out o;
    struct p9_in in;
    char ver[16];
    int type;
    uint32_t msize;

    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TVERSION, P9_NOTAG);
    p9o_u32(&o, P9_MSIZE);
    p9o_str(&o, P9_VERSION);
    type = p9_txn(&local, &o, &in);
    if (type < 0)
        return type;
    if (type != P9_RVERSION)
        return -EPROTO;
    msize = p9i_u32(&in);
    p9i_str(&in, ver, sizeof ver);
    if (!in.ok || msize < 256 || strcmp(ver, P9_VERSION) != 0)
        return -EPROTO;

    p9o_init(&o, txbuf, sizeof txbuf);
    p9o_hdr(&o, P9_TATTACH, 0);
    p9o_u32(&o, P9_ROOTFID);
    p9o_u32(&o, P9_NOFID);
    p9o_str(&o, "skj");                  /* uname  */
    p9o_str(&o, "");                     /* aname  */
    type = p9_txn(&local, &o, &in);
    if (type < 0)
        return type;
    if (type != P9_RATTACH)
        return -EPROTO;
    return 0;
}

/* Record the transport for a mount at index `m`: fid 0 is the attached root. */
static void
bind_mount(int m, const struct p9_transport *t)
{
    mounts[m].t = *t;
    mounts[m].fids = (uint64_t)1;        /* fid 0 (root) in use */
}

int
ninefs_mount(const char *name, const struct p9_transport *t, uint32_t flags)
{
    int m, rc = p9_handshake(t);

    if (rc < 0)
        return rc;
    m = fs_install_mount(name, FS_KIND_9P, (intptr_t)P9_ROOTFID, flags);
    if (m < 0)
        return m;
    bind_mount(m, t);
    return m;
}

intptr_t
ninefs_adopt(int idx, const struct p9_transport *t)
{
    int rc = p9_handshake(t);

    if (rc < 0)
        return rc;
    bind_mount(idx, t);
    return (intptr_t)P9_ROOTFID;         /* the attached root fid */
}
