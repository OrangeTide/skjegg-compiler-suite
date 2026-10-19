/* rv_gdb.c : the RV32 GDB remote-serial-protocol server (see rv_gdb.h). */

#include "rv_gdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define PKT_MAX     8192            /* packet payload buffer (PacketSize below) */
#define BP_MAX      64              /* host-side software breakpoints */
#define NGPR        32
#define NREG        (NGPR + 1)      /* x0-x31 plus pc */
#define IRQ_STRIDE  4096            /* steps between interrupt-byte polls */

/* The target description gdb reads over qXfer, so a plain `gdb-multiarch
 * target remote` learns the architecture and register layout with no manual
 * `set architecture`.  33 registers: x0-x31 then pc, each 32 bits. */
static const char target_xml[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "  <architecture>riscv:rv32</architecture>\n"
    "  <feature name=\"org.gnu.gdb.riscv.cpu\">\n"
    "    <reg name=\"zero\" bitsize=\"32\" regnum=\"0\"/>\n"
    "    <reg name=\"ra\" bitsize=\"32\"/>\n"
    "    <reg name=\"sp\" bitsize=\"32\"/>\n"
    "    <reg name=\"gp\" bitsize=\"32\"/>\n"
    "    <reg name=\"tp\" bitsize=\"32\"/>\n"
    "    <reg name=\"t0\" bitsize=\"32\"/>\n"
    "    <reg name=\"t1\" bitsize=\"32\"/>\n"
    "    <reg name=\"t2\" bitsize=\"32\"/>\n"
    "    <reg name=\"fp\" bitsize=\"32\"/>\n"
    "    <reg name=\"s1\" bitsize=\"32\"/>\n"
    "    <reg name=\"a0\" bitsize=\"32\"/>\n"
    "    <reg name=\"a1\" bitsize=\"32\"/>\n"
    "    <reg name=\"a2\" bitsize=\"32\"/>\n"
    "    <reg name=\"a3\" bitsize=\"32\"/>\n"
    "    <reg name=\"a4\" bitsize=\"32\"/>\n"
    "    <reg name=\"a5\" bitsize=\"32\"/>\n"
    "    <reg name=\"a6\" bitsize=\"32\"/>\n"
    "    <reg name=\"a7\" bitsize=\"32\"/>\n"
    "    <reg name=\"s2\" bitsize=\"32\"/>\n"
    "    <reg name=\"s3\" bitsize=\"32\"/>\n"
    "    <reg name=\"s4\" bitsize=\"32\"/>\n"
    "    <reg name=\"s5\" bitsize=\"32\"/>\n"
    "    <reg name=\"s6\" bitsize=\"32\"/>\n"
    "    <reg name=\"s7\" bitsize=\"32\"/>\n"
    "    <reg name=\"s8\" bitsize=\"32\"/>\n"
    "    <reg name=\"s9\" bitsize=\"32\"/>\n"
    "    <reg name=\"s10\" bitsize=\"32\"/>\n"
    "    <reg name=\"s11\" bitsize=\"32\"/>\n"
    "    <reg name=\"t3\" bitsize=\"32\"/>\n"
    "    <reg name=\"t4\" bitsize=\"32\"/>\n"
    "    <reg name=\"t5\" bitsize=\"32\"/>\n"
    "    <reg name=\"t6\" bitsize=\"32\"/>\n"
    "    <reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\" regnum=\"32\"/>\n"
    "  </feature>\n"
    "</target>\n";

struct gdb {
    int fd;                 /* the accepted connection */
    int ack_mode;           /* +/- acknowledgements still in effect */
    rv_cpu *cpu;
    guest *g;
    uint32_t bp[BP_MAX];
    int nbp;
};

/****************************************************************
 * Hex and packet transport
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

/* Read one byte, blocking.  Returns 0 and the byte, or -1 on EOF/error. */
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

/* Append the $...#cc frame for `payload` and send it, honoring ack mode. */
static int
send_packet(struct gdb *s, const char *payload)
{
    char buf[PKT_MAX + 8];
    unsigned sum = 0;
    size_t i, len = strlen(payload);

    if (len + 5 > sizeof buf)
        return -1;
    buf[0] = '$';
    for (i = 0; i < len; i++) {
        buf[1 + i] = payload[i];
        sum += (unsigned char)payload[i];
    }
    buf[1 + len] = '#';
    buf[2 + len] = hexdig((sum >> 4) & 0xf);
    buf[3 + len] = hexdig(sum & 0xf);
    for (;;) {
        unsigned char ack;
        if (write_all(s->fd, buf, len + 4))
            return -1;
        if (!s->ack_mode)
            return 0;
        if (read_byte(s->fd, &ack))
            return -1;
        if (ack == '+')
            return 0;
        if (ack == '-')
            continue;               /* resend on a NAK */
        return 0;                   /* unexpected: treat as delivered */
    }
}

/* Receive one packet payload into `out` (NUL-terminated).  Returns the length,
   -1 on EOF/error, or -2 when a raw Ctrl-C (0x03) interrupt byte arrived in
   place of a packet.  Leading `+`/`-` acks are skipped. */
static int
recv_packet(struct gdb *s, char *out, size_t cap)
{
    unsigned char c;
    unsigned sum, want;
    size_t n;

    for (;;) {
        if (read_byte(s->fd, &c))
            return -1;
        if (c == 0x03)
            return -2;              /* interrupt request */
        if (c == '$')
            break;
        /* otherwise a stray ack or noise: ignore */
    }
    n = 0;
    sum = 0;
    for (;;) {
        if (read_byte(s->fd, &c))
            return -1;
        if (c == '#')
            break;
        if (n + 1 >= cap)
            return -1;
        out[n++] = (char)c;
        sum += c;
    }
    out[n] = '\0';
    {
        unsigned char h, l;
        if (read_byte(s->fd, &h) || read_byte(s->fd, &l))
            return -1;
        want = (unsigned)((hexval(h) << 4) | hexval(l));
    }
    if (s->ack_mode) {
        char a = (want == (sum & 0xff)) ? '+' : '-';
        if (write_all(s->fd, &a, 1))
            return -1;
        if (a == '-')
            return recv_packet(s, out, cap);    /* bad checksum: await resend */
    }
    return (int)n;
}

/* Is a raw 0x03 interrupt byte waiting on the socket?  Non-blocking. */
static int
interrupt_pending(int fd)
{
    unsigned char c;
    ssize_t n = recv(fd, &c, 1, MSG_DONTWAIT);

    return (n == 1 && c == 0x03);
}

/****************************************************************
 * Register and memory helpers
 ****************************************************************/

/* Little-endian u32 -> 8 lowercase hex chars appended at *p. */
static void
put_u32_hex(char **p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) {        /* byte 0 first (little-endian) */
        unsigned b = (v >> (8 * i)) & 0xff;
        *(*p)++ = hexdig(b >> 4);
        *(*p)++ = hexdig(b & 0xf);
    }
}

/* Read a little-endian u32 from 8 hex chars; returns 0 on success. */
static int
get_u32_hex(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        int hi = hexval(p[2 * i]), lo = hexval(p[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        v |= (uint32_t)((hi << 4) | lo) << (8 * i);
    }
    *out = v;
    return 0;
}

static uint32_t
reg_get(rv_cpu *cpu, int n)
{
    if (n < NGPR)
        return rv_get_x(cpu, n);
    if (n == NGPR)
        return rv_get_pc(cpu);
    return 0;
}

static void
reg_set(rv_cpu *cpu, int n, uint32_t v)
{
    if (n < NGPR)
        rv_set_x(cpu, n, v);
    else if (n == NGPR)
        rv_set_pc(cpu, v);
}

/* True if `addr` lies in a mapped region. */
static int
addr_mapped(guest *g, uint32_t addr)
{
    int i;
    for (i = 0; i < g->mem.nregions; i++) {
        struct gm_region *r = &g->mem.regions[i];
        if (addr >= r->base && addr - r->base < r->size)
            return 1;
    }
    return 0;
}

/****************************************************************
 * Stop replies and execution
 ****************************************************************/

/* The signal to report for the current stopped state. */
static int
stop_signal(struct gdb *s)
{
    if (s->g->mem.fault)
        return 11;                  /* SIGSEGV */
    if (s->cpu->in_trap)
        return 5;                   /* SIGTRAP (a guest trap) */
    return 5;                       /* SIGTRAP: step done or breakpoint */
}

/* Send the stop reply for the current state: S<sig>, or W<code> when the guest
   has exited. */
static int
send_stop(struct gdb *s)
{
    char buf[8];
    if (s->g->exited) {
        snprintf(buf, sizeof buf, "W%02x", s->g->exit_code & 0xff);
    } else {
        snprintf(buf, sizeof buf, "S%02x", stop_signal(s));
    }
    return send_packet(s, buf);
}

static int
is_breakpoint(struct gdb *s, uint32_t pc)
{
    int i;
    for (i = 0; i < s->nbp; i++)
        if (s->bp[i] == pc)
            return 1;
    return 0;
}

/* Continue: single-step until a breakpoint, exit, fault/halt, or an interrupt
   byte on the socket.  Returns 1 if the guest exited, else 0 (stopped). */
static int
do_continue(struct gdb *s)
{
    uint32_t steps = 0;

    for (;;) {
        if (s->g->exited || s->cpu->halted)
            return s->g->exited ? 1 : 0;
        if (rv_step(s->cpu) < 0)        /* halted on this step */
            return s->g->exited ? 1 : 0;
        if (s->g->exited)
            return 1;
        if (is_breakpoint(s, rv_get_pc(s->cpu)))
            return 0;
        if (((++steps) & (IRQ_STRIDE - 1)) == 0 && interrupt_pending(s->fd))
            return 0;                   /* Ctrl-C from the debugger */
    }
}

/****************************************************************
 * Packet dispatch
 ****************************************************************/

/* Returns 1 to keep serving, 0 to end the session. */
static int
handle_packet(struct gdb *s, const char *p)
{
    char out[PKT_MAX];

    switch (p[0]) {
    case '?':                           /* why stopped */
        return send_stop(s) == 0;

    case 'q':
        if (strncmp(p, "qSupported", 10) == 0) {
            snprintf(out, sizeof out,
                     "PacketSize=%x;qXfer:features:read+", PKT_MAX - 1);
            return send_packet(s, out) == 0;
        }
        if (strncmp(p, "qXfer:features:read:", 20) == 0) {
            /* qXfer:features:read:ANNEX:OFFSET,LENGTH */
            const char *a = p + 20;
            unsigned long off, len;
            const char *comma;
            size_t xlen = sizeof target_xml - 1;

            comma = strchr(a, ':');
            if (!comma)
                return send_packet(s, "E00") == 0;
            off = strtoul(comma + 1, (char **)&comma, 16);
            len = (*comma == ',') ? strtoul(comma + 1, NULL, 16) : 0;
            if (off >= xlen)
                return send_packet(s, "l") == 0;
            if (len > sizeof out - 2)
                len = sizeof out - 2;
            if (off + len >= xlen) {
                out[0] = 'l';
                memcpy(out + 1, target_xml + off, xlen - off);
                out[1 + (xlen - off)] = '\0';
            } else {
                out[0] = 'm';
                memcpy(out + 1, target_xml + off, len);
                out[1 + len] = '\0';
            }
            return send_packet(s, out) == 0;
        }
        if (strcmp(p, "qAttached") == 0)
            return send_packet(s, "1") == 0;
        if (p[1] == 'C' && p[2] == '\0')        /* current thread */
            return send_packet(s, "QC01") == 0;
        if (strcmp(p, "qfThreadInfo") == 0)
            return send_packet(s, "m01") == 0;
        if (strcmp(p, "qsThreadInfo") == 0)
            return send_packet(s, "l") == 0;
        if (strncmp(p, "qTStatus", 8) == 0)
            return send_packet(s, "") == 0;
        return send_packet(s, "") == 0;

    case 'H':                           /* set thread for subsequent ops */
        return send_packet(s, "OK") == 0;

    case 'g': {                         /* read all registers */
        char *w = out;
        int i;
        for (i = 0; i < NREG; i++)
            put_u32_hex(&w, reg_get(s->cpu, i));
        *w = '\0';
        return send_packet(s, out) == 0;
    }

    case 'G': {                         /* write all registers */
        const char *h = p + 1;
        int i;
        for (i = 0; i < NREG; i++) {
            uint32_t v;
            if (strlen(h) < 8 || get_u32_hex(h, &v))
                break;
            reg_set(s->cpu, i, v);
            h += 8;
        }
        return send_packet(s, "OK") == 0;
    }

    case 'p': {                         /* read one register */
        char *w = out;
        long n = strtol(p + 1, NULL, 16);
        if (n < 0 || n > NGPR)
            return send_packet(s, "E01") == 0;
        put_u32_hex(&w, reg_get(s->cpu, (int)n));
        *w = '\0';
        return send_packet(s, out) == 0;
    }

    case 'P': {                         /* write one register: P n=val */
        const char *eq = strchr(p, '=');
        long n = strtol(p + 1, NULL, 16);
        uint32_t v;
        if (!eq || n < 0 || n > NGPR || get_u32_hex(eq + 1, &v))
            return send_packet(s, "E01") == 0;
        reg_set(s->cpu, (int)n, v);
        return send_packet(s, "OK") == 0;
    }

    case 'm': {                         /* read memory: m addr,len */
        char *end;
        uint32_t addr = (uint32_t)strtoul(p + 1, &end, 16);
        uint32_t len = (*end == ',') ? (uint32_t)strtoul(end + 1, NULL, 16) : 0;
        char *w = out;
        uint32_t i;

        if (len == 0)
            return send_packet(s, "E01") == 0;
        if (!addr_mapped(s->g, addr))
            return send_packet(s, "E01") == 0;
        if (len > (PKT_MAX - 1) / 2)
            len = (PKT_MAX - 1) / 2;
        for (i = 0; i < len; i++) {
            uint8_t b = gm_read8(&s->g->mem, addr + i);
            *w++ = hexdig(b >> 4);
            *w++ = hexdig(b & 0xf);
        }
        *w = '\0';
        return send_packet(s, out) == 0;
    }

    case 'M': {                         /* write memory: M addr,len:data */
        char *end;
        uint32_t addr = (uint32_t)strtoul(p + 1, &end, 16);
        uint32_t len = (*end == ',') ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
        const char *data = (*end == ':') ? end + 1 : NULL;
        uint32_t i;

        if (!data || !addr_mapped(s->g, addr))
            return send_packet(s, "E01") == 0;
        for (i = 0; i < len; i++) {
            int hi = hexval(data[2 * i]), lo = hexval(data[2 * i + 1]);
            if (hi < 0 || lo < 0)
                return send_packet(s, "E01") == 0;
            gm_write8(&s->g->mem, addr + i, (uint8_t)((hi << 4) | lo));
        }
        return send_packet(s, "OK") == 0;
    }

    case 'c': {                         /* continue [addr] */
        if (p[1])
            rv_set_pc(s->cpu, (uint32_t)strtoul(p + 1, NULL, 16));
        do_continue(s);
        return send_stop(s) == 0;
    }

    case 's': {                         /* single step [addr] */
        if (p[1])
            rv_set_pc(s->cpu, (uint32_t)strtoul(p + 1, NULL, 16));
        if (!s->g->exited && !s->cpu->halted)
            rv_step(s->cpu);
        return send_stop(s) == 0;
    }

    case 'Z':                           /* insert breakpoint: Z0,addr,kind */
        if (p[1] == '0') {
            uint32_t addr = (uint32_t)strtoul(p + 3, NULL, 16);
            if (!is_breakpoint(s, addr)) {
                if (s->nbp >= BP_MAX)
                    return send_packet(s, "E01") == 0;
                s->bp[s->nbp++] = addr;
            }
            return send_packet(s, "OK") == 0;
        }
        return send_packet(s, "") == 0; /* hardware/watch not supported */

    case 'z':                           /* remove breakpoint: z0,addr,kind */
        if (p[1] == '0') {
            uint32_t addr = (uint32_t)strtoul(p + 3, NULL, 16);
            int i;
            for (i = 0; i < s->nbp; i++)
                if (s->bp[i] == addr) {
                    s->bp[i] = s->bp[--s->nbp];
                    break;
                }
            return send_packet(s, "OK") == 0;
        }
        return send_packet(s, "") == 0;

    case 'D':                           /* detach */
        send_packet(s, "OK");
        return 0;

    case 'k':                           /* kill */
        return 0;

    default:
        return send_packet(s, "") == 0; /* unsupported: empty reply */
    }
}

/****************************************************************
 * Server setup and loop
 ****************************************************************/

static int
listen_accept(int port)
{
    int ls, conn, yes = 1;
    struct sockaddr_in addr;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0)
        return -1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(ls, 1) < 0) {
        close(ls);
        return -1;
    }
    fprintf(stderr, "skj-run: gdb server listening on 127.0.0.1:%d\n", port);
    fflush(stderr);
    conn = accept(ls, NULL, NULL);
    close(ls);
    if (conn < 0)
        return -1;
    setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    return conn;
}

int
rv_gdb_serve(rv_cpu *cpu, guest *g, int port, int *exit_code)
{
    struct gdb s;
    char pkt[PKT_MAX];

    memset(&s, 0, sizeof s);
    s.ack_mode = 1;
    s.cpu = cpu;
    s.g = g;

    s.fd = listen_accept(port);
    if (s.fd < 0) {
        fprintf(stderr, "skj-run: gdb server could not listen on port %d\n",
                port);
        return -1;
    }

    for (;;) {
        int n = recv_packet(&s, pkt, sizeof pkt);
        if (n == -1)
            break;                      /* connection closed */
        if (n == -2) {                  /* an interrupt between packets */
            send_stop(&s);
            continue;
        }
        if (n == 0)
            continue;                   /* empty packet */
        if (!handle_packet(&s, pkt))
            break;
    }

    close(s.fd);
    if (exit_code)
        *exit_code = g->exited ? g->exit_code : 0;
    return 0;
}
