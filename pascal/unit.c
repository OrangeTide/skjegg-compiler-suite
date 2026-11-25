/* unit.c : Pascal unit (uses/unit) resolution

   A compiled unit is a GNU `ar` archive `<name>.a` carrying its object
   member(s) beside a reserved text member `.ppm`.  The `.ppm` member holds the
   unit's interface-section declaration source, prefixed by a one-line header:

       (* skj-pascal-unit 1 <name> <lp64|ilp32> <be|le> *)

   A consumer re-parses the declaration source (see parse_resolve_uses), so the
   target ABI the unit was compiled for must match the consumer's, since record
   layouts are recomputed from the re-parsed source.  The header carries that
   ABI and this loader refuses a mismatch. */

#include "pascal.h"
#include "arena.h"
#include "target.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read a whole file into a malloc'd buffer, or NULL if it cannot be opened.
   The length (without any added terminator) is returned through *outlen. */
static char *
slurp_file(const char *path, long *outlen)
{
    FILE *f;
    long n;
    char *buf;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        die("out of memory reading %s", path);
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        die("read failed on %s", path);
    }
    fclose(f);
    buf[n] = '\0';
    *outlen = n;
    return buf;
}

/* Find the reserved member `want` in an ar archive image and return its payload
   (arena-allocated, NUL-terminated).  A dot-named member is not in the symbol
   index, so it is found by an exact-name header walk: the index '/' and the
   long-name table '//' both begin with '/', so their name fields read empty and
   never match a wanted name.  Returns NULL if the image is not an archive or has
   no such member. */
static char *
ar_member(struct arena *a, const char *buf, long len, const char *want,
          long *outlen)
{
    size_t off, wl = strlen(want);

    if (len < 8 || memcmp(buf, "!<arch>\n", 8) != 0)
        return NULL;
    off = 8;
    while (off + 60 <= (size_t)len) {
        const char *h = buf + off;
        char nm[17];
        int k;
        size_t j, sz = 0;

        for (k = 0; k < 16 && h[k] != '/' && h[k] != ' '; k++)
            nm[k] = h[k];
        nm[k] = '\0';
        for (j = 48; j < 58 && h[j] >= '0' && h[j] <= '9'; j++)
            sz = sz * 10 + (size_t)(h[j] - '0');
        if ((size_t)k == wl && memcmp(nm, want, wl) == 0) {
            char *out;
            if (off + 60 + sz > (size_t)len)
                sz = (size_t)len - off - 60;
            out = arena_alloc(a, sz + 1);
            memcpy(out, buf + off + 60, sz);
            out[sz] = '\0';
            if (outlen)
                *outlen = (long)sz;
            return out;
        }
        off += 60 + sz + (sz & 1);
    }
    return NULL;
}

/* Parse the `.ppm` header line, validate magic/version/arch, collect the
   interface-dependency unit names from an optional `uses:A,B,...` field into
   deps[], and return a pointer to the start of the declaration body (just past
   the header's newline).  The header is:

       (* skj-pascal-unit 1 <name> <arch> [uses:A,B,...] *)

   Dies on a malformed header or an arch mismatch. */
static char *
parse_header(struct arena *a, const char *unit, char *ppm,
             char **deps, int maxdeps, int *ndeps)
{
    char magic[32], uname[128], arch[32];
    char *nl, *u;
    int ver;

    *ndeps = 0;
    if (sscanf(ppm, "(* %31s %d %127s %31s",
               magic, &ver, uname, arch) != 4 ||
        strcmp(magic, "skj-pascal-unit") != 0)
        die("unit '%s': not a skj-pascal-unit interface member", unit);
    if (ver != 1)
        die("unit '%s': interface format version %d is not supported", unit, ver);
    if (strcmp(arch, SKJ_KPM_ARCH) != 0)
        die("unit '%s': built for %s, this target is %s",
            unit, arch, SKJ_KPM_ARCH);

    nl = strchr(ppm, '\n');
    u = strstr(ppm, "uses:");
    if (u && (!nl || u < nl)) {
        char *q = u + 5;
        while (*q && *q != '*' && *q != '\n' && !isspace((unsigned char)*q)) {
            char name[128];
            int k = 0;
            while (*q && *q != ',' && *q != '*' &&
                   !isspace((unsigned char)*q) && k < 127)
                name[k++] = *q++;
            name[k] = '\0';
            if (k > 0 && *ndeps < maxdeps)
                deps[(*ndeps)++] = arena_strdup(a, name);
            if (*q == ',')
                q++;
            else
                break;
        }
    }
    return nl ? nl + 1 : ppm + strlen(ppm);
}

/* Join dir + base + ".a" into dst (dir may be empty or lack a trailing slash). */
static void
join_unit_path(char *dst, size_t cap, const char *dir, const char *base)
{
    if (dir && dir[0]) {
        size_t dl = strlen(dir);
        int slash = (dl > 0 && dir[dl - 1] == '/') ? 0 : 1;
        snprintf(dst, cap, "%s%s%s.a", dir, slash ? "/" : "", base);
    } else {
        snprintf(dst, cap, "%s.a", base);
    }
}

/* Try one directory: read <dir>/<rawname>.a then <dir>/<lname>.a.  Returns the
   whole archive image (malloc'd) and sets *which to the basename that matched,
   or NULL if neither opens. */
static char *
try_dir(const char *dir, const char *lname, const char *rawname, long *outlen)
{
    char path[1024];
    char *buf;
    const char *cands[2];
    int i;

    cands[0] = rawname;
    cands[1] = lname;
    for (i = 0; i < 2; i++) {
        if (!cands[i])
            continue;
        if (i == 1 && rawname && strcmp(rawname, lname) == 0)
            continue;
        join_unit_path(path, sizeof(path), dir, cands[i]);
        buf = slurp_file(path, outlen);
        if (buf)
            return buf;
    }
    return NULL;
}

char *
unit_load_interface(struct arena *a, const char *lname, const char *rawname,
                    const char *srcdir, const char **updirs, int nupdirs,
                    char **deps, int maxdeps, int *ndeps)
{
    char *buf = NULL;
    long len = 0;
    char *ppm, *body;
    int d;

    /* source dir, then each -U dir, then "." */
    buf = try_dir(srcdir ? srcdir : "", lname, rawname, &len);
    for (d = 0; !buf && d < nupdirs; d++)
        buf = try_dir(updirs[d], lname, rawname, &len);
    if (!buf)
        buf = try_dir(".", lname, rawname, &len);
    if (!buf)
        die("cannot find unit '%s' (looked for %s.a on the unit path)",
            lname, rawname ? rawname : lname);

    ppm = ar_member(a, buf, len, ".ppm", NULL);
    free(buf);
    if (!ppm)
        die("unit '%s': archive has no .ppm interface member", lname);

    body = parse_header(a, lname, ppm, deps, maxdeps, ndeps);
    return arena_strdup(a, body);
}
