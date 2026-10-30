/* skj_dbg.c : skj-dbg, a small source-level debugger for RV32 skjegg guests.

   It launches `skj-run --gdb PORT program.elf`, connects to the stub over the
   GDB remote protocol, and drives it with a tiny command set (break, continue,
   step, registers, memory, backtrace, quit).  Addresses are symbolized through
   the shared DWARF reader (dwarf/dwread.c): a breakpoint names a function, a
   stop prints file:line, and `bt` walks the frame-pointer chain with the same
   dw_backtrace the host fault path uses, reading guest memory over the wire.

   The RSP client is a slim relative of tests/rv32/gdbclient.c, trimmed to the
   stub's integer-only target (that client requires an FPU for the lockstep
   oracle, which this target does not expose). */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE                /* expose INADDR_LOOPBACK, which strict
                                           _POSIX_C_SOURCE hides on Darwin */
#else
#define _POSIX_C_SOURCE 200809L         /* fork, execvp, kill, getpid, sockets */
#endif

#include "dwread.h"
#include "arena.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define PKT_MAX     8192
#define NGPR        32

static const char *const abi_name[NGPR] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

struct dbg {
    int fd;
    pid_t child;
    struct dw_reader r;
    struct arena arena;
    int have_reader;
};

/****************************************************************
 * RSP transport
 ****************************************************************/

static int
hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char
hexdig(int v)
{
    return "0123456789abcdef"[v & 0xf];
}

static int
write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int
read_byte(int fd, unsigned char *c)
{
    for (;;) {
        ssize_t n = read(fd, c, 1);
        if (n == 1) return 0;
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        return -1;
    }
}

static int
send_packet(struct dbg *d, const char *cmd)
{
    char buf[PKT_MAX + 8];
    unsigned sum = 0;
    size_t i, len = strlen(cmd);
    unsigned char ack;

    if (len + 5 > sizeof buf)
        return -1;
    buf[0] = '$';
    for (i = 0; i < len; i++) {
        buf[1 + i] = cmd[i];
        sum += (unsigned char)cmd[i];
    }
    buf[1 + len] = '#';
    buf[2 + len] = hexdig((sum >> 4) & 0xf);
    buf[3 + len] = hexdig(sum & 0xf);
    if (write_all(d->fd, buf, len + 4))
        return -1;
    if (read_byte(d->fd, &ack))         /* the stub keeps ack mode on */
        return -1;
    return 0;
}

/* Receive one packet payload (NUL-terminated).  Console-output 'O' packets are
   printed and skipped.  Returns the length or -1 on error. */
static int
recv_packet(struct dbg *d, char *out, size_t cap)
{
    unsigned char c;
    size_t n;

    for (;;) {
        do {
            if (read_byte(d->fd, &c))
                return -1;
        } while (c != '$');
        n = 0;
        for (;;) {
            if (read_byte(d->fd, &c))
                return -1;
            if (c == '#')
                break;
            if (n + 1 >= cap)
                return -1;
            out[n++] = (char)c;
        }
        out[n] = '\0';
        if (read_byte(d->fd, &c) || read_byte(d->fd, &c))       /* checksum */
            return -1;
        if (write_all(d->fd, "+", 1))
            return -1;
        /* A console-output packet is 'O' followed by an even run of hex.
           "OK" also starts with 'O' but is not all hex, so inspect the whole
           payload before deciding, or a plain "OK" reply is swallowed. */
        if (out[0] == 'O' && n > 1 && (n & 1) == 1) {
            size_t k;
            int all_hex = 1;
            for (k = 1; k < n; k++)
                if (hexval(out[k]) < 0) {
                    all_hex = 0;
                    break;
                }
            if (all_hex) {
                for (k = 1; k + 1 < n; k += 2)
                    putchar((hexval(out[k]) << 4) | hexval(out[k + 1]));
                continue;
            }
        }
        return (int)n;
    }
}

static int
transact(struct dbg *d, const char *cmd, char *out, size_t cap)
{
    if (send_packet(d, cmd))
        return -1;
    return recv_packet(d, out, cap);
}

/****************************************************************
 * Register and memory access over the wire
 ****************************************************************/

/* Read all 33 registers (x0-x31, pc) via the g packet. */
static int
read_regs(struct dbg *d, uint32_t regs[NGPR + 1])
{
    char reply[PKT_MAX];
    int i;

    if (transact(d, "g", reply, sizeof reply) < 0)
        return -1;
    if (strlen(reply) < (size_t)(NGPR + 1) * 8)
        return -1;
    for (i = 0; i <= NGPR; i++) {
        uint32_t v = 0;
        int b;
        for (b = 0; b < 4; b++) {
            int hi = hexval(reply[i * 8 + b * 2]);
            int lo = hexval(reply[i * 8 + b * 2 + 1]);
            if (hi < 0 || lo < 0)
                return -1;
            v |= (uint32_t)((hi << 4) | lo) << (8 * b);
        }
        regs[i] = v;
    }
    return 0;
}

/* Read `len` bytes of guest memory at `addr` via m.  Returns 0 on success. */
static int
read_mem(struct dbg *d, uint32_t addr, void *dst, uint32_t len)
{
    char cmd[32], reply[PKT_MAX];
    uint8_t *p = dst;
    uint32_t i;

    snprintf(cmd, sizeof cmd, "m%x,%x", addr, len);
    if (transact(d, cmd, reply, sizeof reply) < 0)
        return -1;
    if (reply[0] == 'E' || strlen(reply) < (size_t)len * 2)
        return -1;
    for (i = 0; i < len; i++) {
        int hi = hexval(reply[i * 2]), lo = hexval(reply[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        p[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* dw_backtrace read callback: a 4-byte guest read over the wire. */
static int
bt_read(void *ctx, uint32_t addr, void *dst, uint32_t len)
{
    return read_mem((struct dbg *)ctx, addr, dst, len) == 0;
}

/****************************************************************
 * Symbolized reporting
 ****************************************************************/

static void
print_location(struct dbg *d, uint32_t pc)
{
    const char *fn = NULL, *file = NULL;
    int line = 0;

    if (d->have_reader && dw_lookup_func(&d->r, pc, &fn) && fn)
        printf("%s", fn);
    else
        printf("0x%08x", pc);
    if (d->have_reader && dw_lookup_line(&d->r, pc, &file, &line) && line > 0)
        printf(" at %s:%d", file ? file : "??", line);
}

/* Parse a stop reply ("Sxx", "Txx..."), or an exit ("Wxx"/"Xxx").  Prints the
   state and returns 1 if the guest is still stopped (debuggable), 0 if it has
   exited. */
static int
report_stop(struct dbg *d, const char *reply)
{
    if (reply[0] == 'W' || reply[0] == 'X') {
        long code = strtol(reply + 1, NULL, 16);
        if (reply[0] == 'W')
            printf("[guest exited with code %ld]\n", code);
        else
            printf("[guest killed by signal %ld]\n", code);
        return 0;
    }
    if (reply[0] == 'S' || reply[0] == 'T') {
        uint32_t regs[NGPR + 1];
        int sig = (int)strtol(reply + 1, NULL, 16);
        if (sig == 11)
            printf("SIGSEGV, segmentation fault in ");
        else if (sig == 2)
            printf("interrupted in ");
        else
            printf("stopped in ");
        if (read_regs(d, regs) == 0) {
            print_location(d, regs[NGPR]);
            printf("\n");
        } else {
            printf("?\n");
        }
        return 1;
    }
    printf("stopped (%s)\n", reply);
    return 1;
}

/****************************************************************
 * Commands
 ****************************************************************/

static void
cmd_backtrace(struct dbg *d)
{
    uint32_t regs[NGPR + 1], pcs[32];
    char buf[2048];
    int n;

    if (read_regs(d, regs) < 0) {
        printf("cannot read registers\n");
        return;
    }
    /* regs[8] is s0/fp. */
    n = dw_backtrace(regs[NGPR], regs[8], bt_read, d, pcs, 32);
    dw_backtrace_format(d->have_reader ? &d->r : NULL, pcs, n, buf, sizeof buf);
    fputs(buf, stdout);
}

static void
cmd_regs(struct dbg *d)
{
    uint32_t regs[NGPR + 1];
    int i;

    if (read_regs(d, regs) < 0) {
        printf("cannot read registers\n");
        return;
    }
    for (i = 0; i < NGPR; i++)
        printf("%-4s 0x%08x%s", abi_name[i], regs[i],
               (i % 4 == 3) ? "\n" : "  ");
    printf("pc   0x%08x\n", regs[NGPR]);
}

static void
cmd_examine(struct dbg *d, const char *arg)
{
    uint32_t addr, len = 16, i;
    uint8_t buf[256];

    addr = (uint32_t)strtoul(arg, NULL, 0);
    if (len > sizeof buf)
        len = sizeof buf;
    if (read_mem(d, addr, buf, len) < 0) {
        printf("cannot read 0x%08x\n", addr);
        return;
    }
    for (i = 0; i < len; i++)
        printf("%02x%s", buf[i], (i % 16 == 15) ? "\n" : " ");
    if (len % 16)
        printf("\n");
}

/* Resolve a break target: *0xADDR / decimal address, or a function name. */
static int
resolve_addr(struct dbg *d, const char *s, uint32_t *out)
{
    int i;

    while (*s == ' ')
        s++;
    if (*s == '*')
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        *out = (uint32_t)strtoul(s, NULL, 16);
        return 0;
    }
    if (*s >= '0' && *s <= '9') {
        *out = (uint32_t)strtoul(s, NULL, 0);
        return 0;
    }
    if (d->have_reader) {
        for (i = 0; i < d->r.nfuncs; i++)
            if (strcmp(d->r.funcs[i].name, s) == 0) {
                *out = d->r.funcs[i].addr;
                return 0;
            }
    }
    return -1;
}

static void
cmd_break(struct dbg *d, const char *arg)
{
    char cmd[64], reply[256];
    uint32_t addr;

    if (resolve_addr(d, arg, &addr) < 0) {
        printf("no symbol '%s'\n", arg);
        return;
    }
    snprintf(cmd, sizeof cmd, "Z0,%x,4", addr);
    if (transact(d, cmd, reply, sizeof reply) < 0 || strcmp(reply, "OK") != 0) {
        printf("could not set breakpoint at 0x%08x\n", addr);
        return;
    }
    printf("breakpoint at 0x%08x: ", addr);
    print_location(d, addr);
    printf("\n");
}

/* Returns 1 to keep the session, 0 to end it. */
static int
run_command(struct dbg *d, char *line)
{
    char reply[PKT_MAX];
    char *cmd, *arg;

    while (*line == ' ')
        line++;
    cmd = line;
    arg = line;
    while (*arg && *arg != ' ')
        arg++;
    if (*arg) {
        *arg++ = '\0';
        while (*arg == ' ')
            arg++;
    }
    if (*cmd == '\0')
        return 1;

    if (!strcmp(cmd, "b") || !strcmp(cmd, "break")) {
        cmd_break(d, arg);
    } else if (!strcmp(cmd, "c") || !strcmp(cmd, "continue")) {
        if (transact(d, "c", reply, sizeof reply) < 0)
            return 0;
        return report_stop(d, reply);
    } else if (!strcmp(cmd, "s") || !strcmp(cmd, "si") || !strcmp(cmd, "stepi")) {
        if (transact(d, "s", reply, sizeof reply) < 0)
            return 0;
        return report_stop(d, reply);
    } else if (!strcmp(cmd, "bt") || !strcmp(cmd, "backtrace")) {
        cmd_backtrace(d);
    } else if (!strcmp(cmd, "regs") || !strcmp(cmd, "info")) {
        cmd_regs(d);
    } else if (!strcmp(cmd, "x")) {
        cmd_examine(d, arg);
    } else if (!strcmp(cmd, "q") || !strcmp(cmd, "quit")) {
        send_packet(d, "k");
        return 0;
    } else {
        printf("commands: break FUNC|ADDR, continue, stepi, backtrace, "
               "regs, x ADDR, quit\n");
    }
    return 1;
}

/****************************************************************
 * Launch and connection
 ****************************************************************/

static int
connect_retry(int port, int attempts)
{
    struct sockaddr_in sa;
    int fd, i, one = 1;
    struct timespec ts = { 0, 20 * 1000 * 1000 };

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (i = 0; i < attempts; i++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            return fd;
        }
        close(fd);
        nanosleep(&ts, NULL);
    }
    return -1;
}

static uint8_t *
read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        *out_len = (size_t)n;
    return buf;
}

static void
usage(FILE *out)
{
    fprintf(out, "usage: skj-dbg [--run skj-run] [--port N] [-ex CMD]... "
                 "program.elf [args...]\n");
}

int
main(int argc, char **argv)
{
    const char *run = "skj-run";
    const char *elf = NULL;
    int port = 0;
    int i, prog_i = 0;
    char *ex[64];
    int nex = 0;
    struct dbg d;
    char portbuf[16];
    char *rargv[64];
    int rn = 0;
    pid_t pid;
    uint8_t *elf_img;
    size_t elf_len = 0;
    char reply[PKT_MAX];

    util_set_progname("skj-dbg");

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run") && i + 1 < argc) {
            run = argv[++i];
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-ex") && i + 1 < argc) {
            if (nex < (int)(sizeof ex / sizeof ex[0]))
                ex[nex++] = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            prog_i = i;
            elf = argv[i];
            break;
        }
    }
    if (!elf) {
        usage(stderr);
        return 2;
    }
    if (port == 0)
        port = 20000 + (int)(getpid() % 20000);

    /* Build the reader for symbolization (not fatal if it fails: raw
     * addresses still work). */
    memset(&d, 0, sizeof d);
    elf_img = read_file(elf, &elf_len);
    arena_init(&d.arena);
    if (elf_img && dw_read_elf(&d.arena, &d.r, elf_img, elf_len) == 0)
        d.have_reader = 1;

    /* Spawn skj-run --gdb PORT program.elf [args...]. */
    snprintf(portbuf, sizeof portbuf, "%d", port);
    rargv[rn++] = (char *)run;
    rargv[rn++] = (char *)"--gdb";
    rargv[rn++] = portbuf;
    for (i = prog_i; i < argc && rn < (int)(sizeof rargv / sizeof rargv[0]) - 1; i++)
        rargv[rn++] = argv[i];
    rargv[rn] = NULL;

    signal(SIGPIPE, SIG_IGN);
    pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        execvp(rargv[0], rargv);
        fprintf(stderr, "skj-dbg: exec %s: %s\n", rargv[0], strerror(errno));
        _exit(127);
    }
    d.child = pid;
    d.fd = connect_retry(port, 200);
    if (d.fd < 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        die("cannot connect to %s on port %d", run, port);
    }

    transact(&d, "qSupported", reply, sizeof reply);    /* advertise ourselves */
    if (transact(&d, "?", reply, sizeof reply) >= 0) {
        printf("skj-dbg: ");
        report_stop(&d, reply);
    }

    for (i = 0; i < nex; i++) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", ex[i]);
        if (!run_command(&d, buf))
            goto done;
    }
    /* Interactive mode only when no -ex scripts were given. */
    if (nex == 0) {
        char line[256];
        printf("(skj-dbg) ");
        fflush(stdout);
        while (fgets(line, sizeof line, stdin)) {
            size_t len = strlen(line);
            if (len && line[len - 1] == '\n')
                line[len - 1] = '\0';
            if (!run_command(&d, line))
                break;
            printf("(skj-dbg) ");
            fflush(stdout);
        }
    }

done:
    close(d.fd);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    arena_free(&d.arena);
    free(elf_img);
    return 0;
}
