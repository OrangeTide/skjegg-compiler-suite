/* gdbclient.c : minimal GDB remote serial protocol client */
/* Copyright (c) 2026 Jon Mayo - MIT-0 OR Public Domain */

#include "gdbclient.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define PKT_MAX 65536

/****************************************************************
 * Packet layer
 ****************************************************************/

static int
hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int
write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len) {
        ssize_t n = write(fd, p, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int
read_byte(int fd, char *c)
{
    for (;;) {
        ssize_t n = read(fd, c, 1);

        if (n == 1)
            return 0;
        if (n == 0)
            return -1;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static int
send_packet(gdb_client *g, const char *cmd)
{
    char buf[PKT_MAX];
    unsigned sum = 0;
    size_t i, len = strlen(cmd);
    char ack;

    if (len + 5 > sizeof(buf))
        return -1;
    for (i = 0; i < len; i++)
        sum += (unsigned char)cmd[i];
    snprintf(buf, sizeof(buf), "$%s#%02x", cmd, sum & 0xff);

    if (write_all(g->fd, buf, strlen(buf)))
        return -1;
    if (g->ack_mode) {
        if (read_byte(g->fd, &ack))
            return -1;
        if (ack != '+')
            return -1;
    }
    return 0;
}

/** Receive one packet body. Notification and console-output packets from
 * the stub are skipped so callers only see replies to their own commands. */
static int
recv_packet(gdb_client *g, char *out, size_t size)
{
    char c;
    size_t n;

    for (;;) {
        do {
            if (read_byte(g->fd, &c))
                return -1;
        } while (c != '$');

        n = 0;
        for (;;) {
            if (read_byte(g->fd, &c))
                return -1;
            if (c == '#')
                break;
            if (n + 1 >= size)
                return -1;
            out[n++] = c;
        }
        out[n] = '\0';

        /* Consume the two checksum characters */
        if (read_byte(g->fd, &c) || read_byte(g->fd, &c))
            return -1;

        if (g->ack_mode && write_all(g->fd, "+", 1))
            return -1;

        /* A console-output packet is 'O' followed by hex. "OK" also starts
         * with 'O' but is not hex, so check the payload rather than just
         * the first character. */
        if (out[0] == 'O' && n > 1) {
            size_t k;

            for (k = 1; k < n; k++) {
                if (hexval(out[k]) < 0)
                    break;
            }
            if (k == n)
                continue;
        }
        return 0;
    }
}

static int
transact(gdb_client *g, const char *cmd, char *out, size_t size)
{
    if (send_packet(g, cmd)) {
        fprintf(stderr, "gdb: send failed for '%.40s'\n", cmd);
        return -1;
    }
    if (recv_packet(g, out, size)) {
        fprintf(stderr, "gdb: no reply to '%.40s'\n", cmd);
        return -1;
    }
    return 0;
}

/****************************************************************
 * Target description
 *
 * Register numbers are assigned by walking the features named in
 * target.xml in order. qemu-riscv32 emits five features -- the integer
 * core, the floating-point file, vector registers, virtual registers and
 * the control registers -- so fcsr does not sit at a fixed number and has
 * to be looked up by name.
 ****************************************************************/

/** Fetch an object through qXfer, following continuation replies. */
static int
xfer_read(gdb_client *g, const char *annex, char *out, size_t size)
{
    char cmd[256], reply[PKT_MAX];
    size_t off = 0, len = 0;

    out[0] = '\0';
    for (;;) {
        const char *src;
        size_t i;

        snprintf(cmd, sizeof(cmd), "qXfer:features:read:%s:%zx,3f0", annex,
                 off);
        if (transact(g, cmd, reply, sizeof(reply)))
            return -1;
        if (reply[0] == 'E' || reply[0] == '\0')
            return -1;

        src = reply + 1;
        for (i = 0; src[i]; i++) {
            char c = src[i];

            /* Escaped bytes are sent as '}' followed by the byte xor 0x20 */
            if (c == '}' && src[i + 1]) {
                i++;
                c = (char)(src[i] ^ 0x20);
            }
            if (len + 1 >= size)
                return -1;
            out[len++] = c;
        }
        out[len] = '\0';
        off = len;              /* offsets count unescaped bytes */

        if (reply[0] == 'l')
            break;
    }
    return 0;
}

/** Record one register name at its assigned number. */
static void
note_register(gdb_client *g, const char *name, int bits, int num)
{
    if (strcmp(name, "ft0") == 0) {
        g->fpu_base = num;
        g->flen = bits / 8;
        g->has_fpu = g->flen == 4 || g->flen == 8;
    } else if (strcmp(name, "fflags") == 0) {
        g->fflags_num = num;
    } else if (strcmp(name, "frm") == 0) {
        g->frm_num = num;
    } else if (strcmp(name, "fcsr") == 0) {
        g->fcsr_num = num;
    }
}

/** Walk one feature document, assigning consecutive register numbers. */
static int
scan_feature(gdb_client *g, const char *xml, int *num)
{
    const char *p = xml;

    while ((p = strstr(p, "<reg ")) != NULL) {
        char name[64];
        const char *q;
        int bits = 32;
        size_t n = 0;

        p += 5;
        q = strstr(p, "name=\"");
        if (!q)
            break;
        q += 6;
        while (*q && *q != '"' && n + 1 < sizeof(name))
            name[n++] = *q++;
        name[n] = '\0';

        q = strstr(p, "bitsize=\"");
        if (q && q < strchr(p, '>'))
            bits = atoi(q + 9);

        q = strstr(p, "regnum=\"");
        if (q && q < strchr(p, '>'))
            *num = atoi(q + 8);

        note_register(g, name, bits, *num);
        (*num)++;
    }
    return 0;
}

static int
scan_target_description(gdb_client *g)
{
    static char target[65536];
    static char feature[262144];
    const char *p;
    int num = 0;

    g->fpu_base = g->fflags_num = g->frm_num = g->fcsr_num = -1;

    if (xfer_read(g, "target.xml", target, sizeof(target))) {
        fprintf(stderr, "gdb: remote does not provide a target "
                        "description\n");
        return -1;
    }

    p = target;
    while ((p = strstr(p, "href=\"")) != NULL) {
        char annex[128];
        size_t n = 0;

        p += 6;
        while (*p && *p != '"' && n + 1 < sizeof(annex))
            annex[n++] = *p++;
        annex[n] = '\0';

        if (xfer_read(g, annex, feature, sizeof(feature))) {
            fprintf(stderr, "gdb: cannot read feature '%s'\n", annex);
            return -1;
        }
        scan_feature(g, feature, &num);
    }

    if (g->fcsr_num < 0) {
        fprintf(stderr, "gdb: target description has no fcsr register\n");
        return -1;
    }
    return 0;
}

/****************************************************************
 * Connection setup
 ****************************************************************/

static int
connect_retry(int port, int attempts)
{
    struct sockaddr_in sa;
    int fd, i, one = 1;
    struct timespec ts = { 0, 20 * 1000 * 1000 };

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (i = 0; i < attempts; i++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            struct timeval tv = { 15, 0 };

            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            /* A stub that stops answering should fail the run rather than
             * block it forever. */
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return fd;
        }
        close(fd);
        nanosleep(&ts, NULL);
    }
    return -1;
}

/** Bring a connected stub up to the point where it can be stepped. */
static int
handshake(gdb_client *g)
{
    char reply[PKT_MAX];

    /* Turn off packet acknowledgement when the stub supports it: it
     * roughly halves the syscall traffic, which matters when stepping
     * millions of instructions. */
    if (transact(g, "QStartNoAckMode", reply, sizeof(reply)) == 0 &&
        strcmp(reply, "OK") == 0)
        g->ack_mode = 0;

    transact(g, "qSupported:xmlRegisters=riscv", reply, sizeof(reply));

    /* The bulk register block holds x0-x31 and pc. */
    if (transact(g, "g", reply, sizeof(reply))) {
        fprintf(stderr, "gdb: no reply to register read\n");
        return -1;
    }
    if (strlen(reply) / 2 < 33 * 4) {
        fprintf(stderr, "gdb: short register block of %zu bytes\n",
                strlen(reply) / 2);
        return -1;
    }
    if (scan_target_description(g))
        return -1;
    if (!g->has_fpu) {
        fprintf(stderr, "gdb: remote exposes no floating-point registers\n");
        return -1;
    }
    return 0;
}

int
gdb_launch_argv(gdb_client *g, char *const argv[], int port)
{
    pid_t pid;

    memset(g, 0, sizeof(*g));
    signal(SIGPIPE, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    g->child = pid;
    g->ack_mode = 1;
    g->fd = connect_retry(port, 200);
    if (g->fd < 0) {
        fprintf(stderr, "gdb: cannot connect to %s on port %d\n", argv[0],
                port);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    if (handshake(g)) {
        gdb_close(g);
        return -1;
    }
    return 0;
}

int
gdb_set_break(gdb_client *g, uint32_t addr, int kind)
{
    char cmd[64], reply[256];

    snprintf(cmd, sizeof(cmd), "Z0,%x,%x", addr, kind);
    if (transact(g, cmd, reply, sizeof(reply)))
        return -1;
    return strcmp(reply, "OK") == 0 ? 0 : -1;
}

int
gdb_continue(gdb_client *g, int *exit_code)
{
    char reply[PKT_MAX];

    if (transact(g, "c", reply, sizeof(reply)))
        return -1;
    switch (reply[0]) {
    case 'T':
    case 'S':
        return 1;
    case 'W':
        if (exit_code)
            *exit_code = (hexval(reply[1]) << 4) | hexval(reply[2]);
        return 0;
    default:
        fprintf(stderr, "gdb: unexpected stop reply '%s'\n", reply);
        return -1;
    }
}

int
gdb_launch(gdb_client *g, const char *qemu, const char *elf, int port)
{
    char portbuf[16];
    pid_t pid;

    memset(g, 0, sizeof(*g));
    snprintf(portbuf, sizeof(portbuf), "%d", port);

    /* A stub that goes away mid-conversation should surface as a write
     * error, not as a signal that kills the comparison run. */
    signal(SIGPIPE, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* The guest writes to stdout directly; discard it so the
         * comparison report stays readable. */
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execlp(qemu, qemu, "-g", portbuf, elf, (char *)NULL);
        fprintf(stderr, "exec %s: %s\n", qemu, strerror(errno));
        _exit(127);
    }

    g->child = pid;
    g->ack_mode = 1;
    g->fd = connect_retry(port, 200);
    if (g->fd < 0) {
        fprintf(stderr, "gdb: cannot connect to %s on port %d\n", qemu, port);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    if (handshake(g)) {
        gdb_close(g);
        return -1;
    }
    return 0;
}

/****************************************************************
 * Register and memory access
 ****************************************************************/

/** Decode a little-endian hex value of n bytes starting at *p. */
static uint64_t
take_hex(const char **p, int n)
{
    uint64_t v = 0;
    int i, hi, lo;

    for (i = 0; i < n; i++) {
        hi = hexval((*p)[0]);
        lo = hexval((*p)[1]);
        if (hi < 0 || lo < 0)
            return v;
        v |= (uint64_t)((hi << 4) | lo) << (8 * i);
        *p += 2;
    }
    return v;
}

int
gdb_read_state(gdb_client *g, gdb_state *st)
{
    char reply[PKT_MAX];
    const char *p = reply;
    int i;

    if (transact(g, "g", reply, sizeof(reply)))
        return -1;

    for (i = 0; i < 32; i++)
        st->x[i] = (uint32_t)take_hex(&p, 4);
    st->pc = (uint32_t)take_hex(&p, 4);
    return 0;
}

/** Read one register by its gdb register number. */
static int
read_reg(gdb_client *g, int num, int bytes, uint64_t *out)
{
    char cmd[16], reply[256];
    const char *p = reply;

    snprintf(cmd, sizeof(cmd), "p%x", num);
    if (transact(g, cmd, reply, sizeof(reply)))
        return -1;
    if (reply[0] == 'E' || strlen(reply) < (size_t)bytes * 2)
        return -1;
    *out = take_hex(&p, bytes);
    return 0;
}

int
gdb_read_fpu(gdb_client *g, gdb_state *st)
{
    uint64_t v;
    int i;

    if (!g->has_fpu)
        return -1;

    for (i = 0; i < 32; i++) {
        if (read_reg(g, g->fpu_base + i, g->flen, &st->f[i]))
            return -1;
    }
    if (read_reg(g, g->fcsr_num, 4, &v))
        return -1;
    st->fcsr = (uint32_t)v;
    st->fflags = st->fcsr & 0x1f;
    st->frm = (st->fcsr >> 5) & 7;
    return 0;
}

int
gdb_step(gdb_client *g, int *exit_code)
{
    char reply[PKT_MAX];

    if (transact(g, "s", reply, sizeof(reply)))
        return -1;

    switch (reply[0]) {
    case 'T':
    case 'S': {
        int sig = (hexval(reply[1]) << 4) | hexval(reply[2]);

        /* A completed single step reports SIGTRAP. Anything else means the
         * guest took a real signal, which for this workload means the
         * reference rejected an instruction the emulator accepted. */
        if (sig != 5) {
            fprintf(stderr, "gdb: guest stopped with signal %d\n", sig);
            return -2;
        }
        return 1;
    }
    case 'W':
        if (exit_code)
            *exit_code = (hexval(reply[1]) << 4) | hexval(reply[2]);
        return 0;
    case 'X':
        if (exit_code)
            *exit_code = -1;
        return 0;
    default:
        fprintf(stderr, "gdb: unexpected stop reply '%s'\n", reply);
        return -1;
    }
}

int
gdb_read_mem(gdb_client *g, uint32_t addr, uint8_t *buf, uint32_t len)
{
    char cmd[64], reply[PKT_MAX];
    uint32_t done = 0;

    while (done < len) {
        uint32_t chunk = len - done;
        const char *p;
        uint32_t i;

        if (chunk > 1024)
            chunk = 1024;
        snprintf(cmd, sizeof(cmd), "m%x,%x", addr + done, chunk);
        if (transact(g, cmd, reply, sizeof(reply)))
            return -1;
        if (reply[0] == 'E' || strlen(reply) < chunk * 2)
            return -1;
        p = reply;
        for (i = 0; i < chunk; i++) {
            int hi = hexval(p[0]), lo = hexval(p[1]);

            if (hi < 0 || lo < 0)
                return -1;
            buf[done + i] = (uint8_t)((hi << 4) | lo);
            p += 2;
        }
        done += chunk;
    }
    return 0;
}

int
gdb_read_freg(gdb_client *g, int n, uint64_t *out)
{
    return read_reg(g, g->fpu_base + n, g->flen, out);
}

int
gdb_read_fcsr(gdb_client *g, uint32_t *out)
{
    uint64_t v;

    if (read_reg(g, g->fcsr_num, 4, &v))
        return -1;
    *out = (uint32_t)v;
    return 0;
}

/****************************************************************
 * Writing remote state
 ****************************************************************/

static void
put_hex(char **p, uint64_t v, int bytes)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < bytes; i++) {
        unsigned byte = (unsigned)((v >> (8 * i)) & 0xff);

        *(*p)++ = digits[byte >> 4];
        *(*p)++ = digits[byte & 0xf];
    }
}

int
gdb_write_state(gdb_client *g, const gdb_state *st)
{
    char cmd[16 + 33 * 8 + 1], reply[256];
    char *p = cmd;
    int i;

    *p++ = 'G';
    for (i = 0; i < 32; i++)
        put_hex(&p, st->x[i], 4);
    put_hex(&p, st->pc, 4);
    *p = '\0';

    if (transact(g, cmd, reply, sizeof(reply)))
        return -1;
    return strcmp(reply, "OK") == 0 ? 0 : -1;
}

static int
write_reg(gdb_client *g, int num, uint64_t val, int bytes)
{
    char cmd[64], reply[256];
    char *p = cmd;

    p += snprintf(cmd, sizeof(cmd), "P%x=", num);
    put_hex(&p, val, bytes);
    *p = '\0';

    if (transact(g, cmd, reply, sizeof(reply)))
        return -1;
    return strcmp(reply, "OK") == 0 ? 0 : -1;
}

int
gdb_write_fpu(gdb_client *g, const gdb_state *st)
{
    int i;

    for (i = 0; i < 32; i++) {
        uint64_t v = st->f[i];

        /* A single-precision value in a wider register must carry an
         * all-ones upper half, or the remote reads it back as a NaN. */
        if (g->flen == 8)
            v = 0xffffffff00000000ull | (uint32_t)v;
        if (write_reg(g, g->fpu_base + i, v, g->flen))
            return -1;
    }
    return write_reg(g, g->fcsr_num, st->fcsr & 0xff, 4);
}

int
gdb_write_mem(gdb_client *g, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    char cmd[3 * 1024], reply[256];
    uint32_t done = 0;

    while (done < len) {
        uint32_t chunk = len - done, i;
        char *p;

        if (chunk > 1024)
            chunk = 1024;
        p = cmd + snprintf(cmd, sizeof(cmd), "M%x,%x:", addr + done, chunk);
        for (i = 0; i < chunk; i++)
            put_hex(&p, buf[done + i], 1);
        *p = '\0';

        if (transact(g, cmd, reply, sizeof(reply)))
            return -1;
        if (strcmp(reply, "OK") != 0) {
            fprintf(stderr, "gdb: write of %u bytes at %08x refused: '%s'\n",
                    chunk, addr + done, reply);
            return -1;
        }
        done += chunk;
    }
    return 0;
}

void
gdb_close(gdb_client *g)
{
    if (g->fd >= 0) {
        send_packet(g, "k");
        close(g->fd);
        g->fd = -1;
    }
    if (g->child > 0) {
        kill(g->child, SIGKILL);
        waitpid(g->child, NULL, 0);
        g->child = 0;
    }
}
