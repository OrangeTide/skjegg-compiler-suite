/* ps: list the sandbox's processes, read from the proc: pseudo filesystem.
 *
 * proc: is a host-served mount (skj-run generates its content from the
 * scheduler's process table).  Its root lists one directory per live process,
 * named by pid, plus a "self" alias; each <pid>/status file carries key:value
 * metadata.  ps reads the values straight out of that text, so it needs no
 * number formatting of its own. */

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>

/* Copy the value of "Key:" out of the status text into out (NUL-terminated),
 * skipping the ':' and any tabs/spaces, stopping at the newline.  Writes "?"
 * when the key is absent. */
static void
getval(const char *buf, const char *key, char *out, int cap)
{
    const char *p = strstr(buf, key);
    int n = 0;

    if (p) {
        p += strlen(key);
        while (*p == ':' || *p == '\t' || *p == ' ')
            p++;
        while (*p && *p != '\n' && n < cap - 1)
            out[n++] = *p++;
    } else if (cap > 1) {
        out[n++] = '?';
    }
    out[n] = 0;
}

static void
puts2(int fd, const char *s)
{
    write(fd, s, (int)strlen(s));
}

/* Print one column value padded to `width` with trailing spaces. */
static void
col(const char *s, int width)
{
    int len = (int)strlen(s);
    static const char pad[] = "            ";

    write(1, s, len);
    if (len < width)
        write(1, pad, width - len);
}

static void
row(const char *pid, const char *state, const char *parent, const char *insns)
{
    col(pid, 6);
    col(state, 10);
    col(parent, 8);
    puts2(1, insns);
    write(1, "\n", 1);
}

int
main(void)
{
    DIR *d = opendir("proc:");
    struct dirent *e;

    if (!d) {
        puts2(2, "ps: cannot open proc:\n");
        return 1;
    }
    row("PID", "STATE", "PARENT", "INSNS");
    while ((e = readdir(d)) != NULL) {
        char path[64], buf[512];
        char pid[16], state[16], parent[16], insns[32];
        int fd, n;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
            strcmp(e->d_name, "self") == 0)
            continue;

        path[0] = 0;
        strcat(path, "proc:");
        strcat(path, e->d_name);
        strcat(path, "/status");
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;                   /* raced with an exit; skip it */
        n = read(fd, buf, (int)sizeof buf - 1);
        close(fd);
        if (n < 0)
            n = 0;
        buf[n] = 0;

        getval(buf, "Pid", pid, sizeof pid);
        getval(buf, "State", state, sizeof state);
        getval(buf, "Parent", parent, sizeof parent);
        getval(buf, "Insns", insns, sizeof insns);
        row(pid, state, parent, insns);
    }
    closedir(d);
    return 0;
}
