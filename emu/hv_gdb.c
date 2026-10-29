/* hv_gdb.c : the GDB remote-serial-protocol server for the HV cores (hv_gdb.h).
 *
 * It drives one process through the VMM seam: registers via the vcpu, memory
 * via the shared guest-memory layer, and execution by single-stepping the vcpu
 * with a host-side breakpoint table, servicing the syscall doorbell in between
 * so the guest keeps making progress.  The transport mirrors rv_gdb.c. */

#include "hv_gdb.h"
#include "hvcore.h"
#include "vmm.h"
#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define PKT_MAX     8192
#define BP_MAX      64
#define IRQ_STRIDE  4096            /* steps between interrupt-byte polls */

struct gdb {
    int fd;
    int ack_mode;
    proc *p;
    guest *g;
    vmm_vcpu *vcpu;
    uint64_t bp[BP_MAX];
    int nbp;
    int fault;                      /* a guest fault stopped the last run */
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
            continue;
        return 0;
    }
}

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
            return -2;
        if (c == '$')
            break;
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
            return recv_packet(s, out, cap);
    }
    return (int)n;
}

static int
interrupt_pending(int fd)
{
    unsigned char c;
    ssize_t n = recv(fd, &c, 1, MSG_DONTWAIT);

    return (n == 1 && c == 0x03);
}

/****************************************************************
 * Memory
 ****************************************************************/

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

static int
stop_signal(struct gdb *s)
{
    return s->fault ? 11 /* SIGSEGV */ : 5 /* SIGTRAP */;
}

static int
send_stop(struct gdb *s)
{
    char buf[8];
    if (s->g->exited)
        snprintf(buf, sizeof buf, "W%02x", s->g->exit_code & 0xff);
    else
        snprintf(buf, sizeof buf, "S%02x", stop_signal(s));
    return send_packet(s, buf);
}

static int
is_breakpoint(struct gdb *s, uint64_t pc)
{
    int i;
    for (i = 0; i < s->nbp; i++)
        if (s->bp[i] == pc)
            return 1;
    return 0;
}

/* Step one guest instruction, servicing any doorbell MMIO on the way.  Returns
 * 1 if the guest exited, else 0. */
static int
step_one(struct gdb *s)
{
    for (;;) {
        struct vmm_exit ex;

        if (s->g->exited || hv_is_halted(s->p))
            return s->g->exited ? 1 : 0;
        vmm_vcpu_run(s->vcpu, &ex);
        switch (ex.kind) {
        case VMM_X_DOORBELL:
            hv_service_doorbell(s->p, &ex);
            if (s->g->exited)
                return 1;
            break;                      /* not an instruction boundary */
        case VMM_X_DEBUG:
            return 0;                   /* one instruction retired */
        case VMM_X_INTR:
            break;
        case VMM_X_HALT:
            return s->g->exited ? 1 : 0;
        default:                        /* MMIO / ERROR: a guest fault */
            s->fault = 1;
            return 0;
        }
    }
}

/* Continue: single-step until a breakpoint, exit, fault/halt, or an interrupt
 * byte on the socket.  Returns 1 if the guest exited, else 0. */
static int
do_continue(struct gdb *s)
{
    uint32_t steps = 0;

    vmm_vcpu_debug_singlestep(s->vcpu, 1);
    for (;;) {
        if (s->g->exited || hv_is_halted(s->p))
            break;
        if (step_one(s))
            break;
        if (s->fault)
            break;
        if (is_breakpoint(s, vmm_vcpu_pc(s->vcpu)))
            break;
        if (((++steps) & (IRQ_STRIDE - 1)) == 0 && interrupt_pending(s->fd))
            break;
    }
    vmm_vcpu_debug_singlestep(s->vcpu, 0);
    return s->g->exited ? 1 : 0;
}

/****************************************************************
 * Packet dispatch
 ****************************************************************/

static int
handle_packet(struct gdb *s, const char *p)
{
    char out[PKT_MAX];

    switch (p[0]) {
    case '?':
        return send_stop(s) == 0;

    case 'q':
        if (strncmp(p, "qSupported", 10) == 0) {
            snprintf(out, sizeof out, "PacketSize=%x", PKT_MAX - 1);
            return send_packet(s, out) == 0;
        }
        if (strcmp(p, "qAttached") == 0)
            return send_packet(s, "1") == 0;
        if (p[1] == 'C' && p[2] == '\0')
            return send_packet(s, "QC01") == 0;
        if (strcmp(p, "qfThreadInfo") == 0)
            return send_packet(s, "m01") == 0;
        if (strcmp(p, "qsThreadInfo") == 0)
            return send_packet(s, "l") == 0;
        return send_packet(s, "") == 0;

    case 'H':
        return send_packet(s, "OK") == 0;

    case 'g': {                         /* read all registers */
        uint8_t regs[VMM_GDB_REGBYTES];
        char *w = out;
        int n = vmm_vcpu_gdb_read_regs(s->vcpu, regs), i;
        for (i = 0; i < n; i++) {
            *w++ = hexdig(regs[i] >> 4);
            *w++ = hexdig(regs[i] & 0xf);
        }
        *w = '\0';
        return send_packet(s, out) == 0;
    }

    case 'G': {                         /* write all registers */
        uint8_t regs[VMM_GDB_REGBYTES];
        const char *h = p + 1;
        int i = 0;
        while (i < (int)sizeof regs && h[0] && h[1]) {
            int hi = hexval(h[0]), lo = hexval(h[1]);
            if (hi < 0 || lo < 0)
                break;
            regs[i++] = (uint8_t)((hi << 4) | lo);
            h += 2;
        }
        vmm_vcpu_gdb_write_regs(s->vcpu, regs, i);
        return send_packet(s, "OK") == 0;
    }

    case 'm': {                         /* read memory: m addr,len */
        char *end;
        uint64_t addr = strtoull(p + 1, &end, 16);
        uint32_t len = (*end == ',') ? (uint32_t)strtoul(end + 1, NULL, 16) : 0;
        char *w = out;
        uint32_t i;

        if (len == 0 || !addr_mapped(s->g, (uint32_t)addr))
            return send_packet(s, "E01") == 0;
        if (len > (PKT_MAX - 1) / 2)
            len = (PKT_MAX - 1) / 2;
        for (i = 0; i < len; i++) {
            uint8_t b = gm_read8(&s->g->mem, (uint32_t)addr + i);
            *w++ = hexdig(b >> 4);
            *w++ = hexdig(b & 0xf);
        }
        *w = '\0';
        return send_packet(s, out) == 0;
    }

    case 'M': {                         /* write memory: M addr,len:data */
        char *end;
        uint64_t addr = strtoull(p + 1, &end, 16);
        uint32_t len = (*end == ',') ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
        const char *data = (*end == ':') ? end + 1 : NULL;
        uint32_t i;

        if (!data || !addr_mapped(s->g, (uint32_t)addr))
            return send_packet(s, "E01") == 0;
        for (i = 0; i < len; i++) {
            int hi = hexval(data[2 * i]), lo = hexval(data[2 * i + 1]);
            if (hi < 0 || lo < 0)
                return send_packet(s, "E01") == 0;
            gm_write8(&s->g->mem, (uint32_t)addr + i, (uint8_t)((hi << 4) | lo));
        }
        return send_packet(s, "OK") == 0;
    }

    case 'c':                           /* continue [addr] */
        if (p[1])
            vmm_vcpu_set_pc(s->vcpu, strtoull(p + 1, NULL, 16));
        do_continue(s);
        return send_stop(s) == 0;

    case 's':                           /* single step [addr] */
        if (p[1])
            vmm_vcpu_set_pc(s->vcpu, strtoull(p + 1, NULL, 16));
        if (!s->g->exited && !hv_is_halted(s->p)) {
            vmm_vcpu_debug_singlestep(s->vcpu, 1);
            step_one(s);
            vmm_vcpu_debug_singlestep(s->vcpu, 0);
        }
        return send_stop(s) == 0;

    case 'Z':                           /* insert breakpoint: Z0,addr,kind */
        if (p[1] == '0') {
            uint64_t addr = strtoull(p + 3, NULL, 16);
            if (!is_breakpoint(s, addr)) {
                if (s->nbp >= BP_MAX)
                    return send_packet(s, "E01") == 0;
                s->bp[s->nbp++] = addr;
            }
            return send_packet(s, "OK") == 0;
        }
        return send_packet(s, "") == 0;

    case 'z':                           /* remove breakpoint: z0,addr,kind */
        if (p[1] == '0') {
            uint64_t addr = strtoull(p + 3, NULL, 16);
            int i;
            for (i = 0; i < s->nbp; i++)
                if (s->bp[i] == addr) {
                    s->bp[i] = s->bp[--s->nbp];
                    break;
                }
            return send_packet(s, "OK") == 0;
        }
        return send_packet(s, "") == 0;

    case 'D':
        send_packet(s, "OK");
        return 0;

    case 'k':
        return 0;

    default:
        return send_packet(s, "") == 0;
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

void
hv_gdb_serve(proc *root, int port, int *exit_code)
{
    struct gdb s;
    char pkt[PKT_MAX];

    memset(&s, 0, sizeof s);
    s.ack_mode = 1;
    s.p = root;
    s.g = &root->g;
    s.vcpu = hv_vcpu_of(root);

    s.fd = listen_accept(port);
    if (s.fd < 0) {
        fprintf(stderr, "skj-run: gdb server could not listen on port %d\n",
                port);
        if (exit_code)
            *exit_code = 0;
        return;
    }

    for (;;) {
        int n = recv_packet(&s, pkt, sizeof pkt);
        if (n == -1)
            break;
        if (n == -2) {
            send_stop(&s);
            continue;
        }
        if (n == 0)
            continue;
        if (!handle_packet(&s, pkt))
            break;
    }

    close(s.fd);
    if (exit_code)
        *exit_code = s.g->exited ? s.g->exit_code : 0;
}
