/* C glue for the optional `netchan` unit: a thin UDP binding to netchan, the
   multiplexed reliable/unreliable channel protocol vendored at
   third_party/netchan/. Archived inside netchan.a; it needs nothing on the link
   line beyond the C library.  See doc/languages/kobold/kp-native-units.md.

   netchan is transport-agnostic: the core leaves the socket and the clock to the
   application, feeding received datagrams in with netchan_feed and handing back
   datagrams to transmit with netchan_send_next. This glue supplies both. Each
   connection owns one non-blocking UDP socket and a monotonic millisecond clock,
   and Service runs the pump: drain the socket into the core, drain the core onto
   the socket, then service its timers.

   Kobold holds no pointers, so a connection and a channel are each a small integer
   id into a bounded table. A host name or a payload is a Kobold `string`, which
   crosses as a guest string handle (kp_gptr), decoded with kp_str_bytes and built
   with kp_str_new; the glue is compiled -DKP_FREESTANDING to match libkpaot's
   no-vm signature (see "Strings cross as a handle" in the doc). */
#define _POSIX_C_SOURCE 200809L

#include "netchan.h"
#include "nc_udp.h"

#include "str.h"        /* kp_gptr, kp_str_bytes / kp_str_new */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define KPNC_MAX_CONN 16
#define KPNC_MAX_CHAN 32
#define KPNC_BUF      2048

struct kpnc_conn {
    struct netchan_conn *c;
    int fd;                     /* the UDP socket, or -1 for a free slot */
    int is_server;
    struct netchan_chan *last_data;   /* channel of the most recent EV_DATA */
};

struct kpnc_chan {
    struct netchan_chan *ch;    /* NULL for a free slot */
};

static struct kpnc_conn g_conn[KPNC_MAX_CONN];
static struct kpnc_chan g_chan[KPNC_MAX_CHAN];

static struct kpnc_conn *
conn_of(int id)
{
    if (id >= 1 && id <= KPNC_MAX_CONN && g_conn[id - 1].c != NULL)
        return &g_conn[id - 1];
    return NULL;
}

static struct kpnc_chan *
chan_of(int id)
{
    if (id >= 1 && id <= KPNC_MAX_CHAN && g_chan[id - 1].ch != NULL)
        return &g_chan[id - 1];
    return NULL;
}

/* The monotonic millisecond clock netchan takes at every feed, send, and
   service call. */
static uint32_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* A non-blocking UDP socket bound to `port` on the loopback-capable wildcard
   address (0 picks an ephemeral port). -1 on failure. */
static int
udp_socket(int port)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int fl;

    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

static int
alloc_conn(void)
{
    int i;

    for (i = 0; i < KPNC_MAX_CONN; i++)
        if (g_conn[i].c == NULL)
            break;
    return (i == KPNC_MAX_CONN) ? -1 : i;
}

int
kpnc_open_server(int port)
{
    int i = alloc_conn();
    int fd;

    if (i < 0)
        return 0;
    fd = udp_socket(port);
    if (fd < 0)
        return 0;
    g_conn[i].c = netchan_open(1);
    if (g_conn[i].c == NULL) {
        close(fd);
        return 0;
    }
    g_conn[i].fd = fd;
    g_conn[i].is_server = 1;
    g_conn[i].last_data = NULL;
    return i + 1;
}

int
kpnc_open_client(kp_gptr host, int port)
{
    const char *h = kp_str_bytes(host);
    char portstr[16];
    struct addrinfo hints, *res = NULL;
    struct nc_addr na;
    int i, fd;

    i = alloc_conn();
    if (i < 0)
        return 0;
    fd = udp_socket(0);                 /* ephemeral source port */
    if (fd < 0)
        return 0;
    /* resolve host:port to one UDP address */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(h, portstr, &hints, &res) != 0 || res == NULL) {
        close(fd);
        return 0;
    }
    if (nc_udp_from_sockaddr(&na, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        close(fd);
        return 0;
    }
    freeaddrinfo(res);
    g_conn[i].c = netchan_open(0);
    if (g_conn[i].c == NULL) {
        close(fd);
        return 0;
    }
    g_conn[i].fd = fd;
    g_conn[i].is_server = 0;
    g_conn[i].last_data = NULL;
    netchan_connect(g_conn[i].c, &na);
    return i + 1;
}

/* The local UDP port a connection is bound to, in host byte order; useful to
   tell a client which ephemeral port a server took. 0 for an invalid id. */
int
kpnc_port(int id)
{
    struct kpnc_conn *cn = conn_of(id);
    struct sockaddr_in a;
    socklen_t sl = sizeof a;

    if (cn == NULL)
        return 0;
    if (getsockname(cn->fd, (struct sockaddr *)&a, &sl) != 0)
        return 0;
    return ntohs(a.sin_port);
}

/* Run one pump step: send everything the core has queued, feed in everything the
   socket has received, service the timers, and (on a server) accept a pending
   handshake. Returns the connection state afterward, so a caller can loop on it. */
int
kpnc_service(int id)
{
    struct kpnc_conn *cn = conn_of(id);
    uint8_t buf[KPNC_BUF];
    struct nc_addr dst;
    struct sockaddr_storage ss;
    struct sockaddr_storage from;
    socklen_t sl, fl;
    size_t n;
    ssize_t r;

    if (cn == NULL)
        return NETCHAN_STATE_CLOSED;
    /* drain the core onto the socket */
    for (;;) {
        memset(&dst, 0, sizeof dst);
        n = netchan_send_next(cn->c, buf, sizeof buf, &dst, now_ms());
        if (n == 0)
            break;
        sl = nc_udp_to_sockaddr(&dst, &ss);
        if (sl != 0)
            sendto(cn->fd, buf, n, 0, (struct sockaddr *)&ss, sl);
    }
    /* drain the socket into the core */
    fl = sizeof from;
    while ((r = recvfrom(cn->fd, buf, sizeof buf, MSG_DONTWAIT,
                         (struct sockaddr *)&from, &fl)) > 0) {
        struct nc_addr a;

        nc_udp_from_sockaddr(&a, (struct sockaddr *)&from, fl);
        netchan_feed(cn->c, buf, (size_t)r, &a, now_ms());
        fl = sizeof from;
    }
    netchan_service(cn->c, now_ms());
    if (cn->is_server && netchan_state(cn->c) == NETCHAN_STATE_CONNECTING)
        netchan_accept(cn->c);
    return netchan_state(cn->c);
}

int
kpnc_state(int id)
{
    struct kpnc_conn *cn = conn_of(id);

    return cn ? netchan_state(cn->c) : NETCHAN_STATE_CLOSED;
}

/* Drain one event; returns its type (NETCHAN_EV_NONE when the queue is empty).
   For a data event the channel is remembered so Read can pull from it. */
int
kpnc_poll(int id)
{
    struct kpnc_conn *cn = conn_of(id);
    struct netchan_event ev;

    if (cn == NULL)
        return NETCHAN_EV_NONE;
    if (!netchan_poll(cn->c, &ev))
        return NETCHAN_EV_NONE;
    if (ev.type == NETCHAN_EV_DATA && ev.ch)
        cn->last_data = ev.ch;
    return ev.type;
}

/* Open a send channel on a connection; a 1-based channel id, or 0 on failure.
   Both peers must open the same channel layout in the same order. */
int
kpnc_open_channel(int id, int type, int dir, kp_gptr content_type)
{
    struct kpnc_conn *cn = conn_of(id);
    struct netchan_chan *ch;
    int i;

    if (cn == NULL)
        return 0;
    for (i = 0; i < KPNC_MAX_CHAN; i++)
        if (g_chan[i].ch == NULL)
            break;
    if (i == KPNC_MAX_CHAN)
        return 0;
    ch = netchan_chan_open(cn->c, type, dir, kp_str_bytes(content_type));
    if (ch == NULL)
        return 0;
    g_chan[i].ch = ch;
    return i + 1;
}

/* Queue one datagram on a channel. Returns the byte count, or a negative
   NETCHAN_ERR_* (an invalid channel id is NETCHAN_ERR). */
int
kpnc_write(int chan, kp_gptr data)
{
    struct kpnc_chan *cl = chan_of(chan);

    if (cl == NULL)
        return NETCHAN_ERR;
    return netchan_chan_write(cl->ch, kp_str_bytes(data), (size_t)kp_str_len(data));
}

/* Read the next datagram delivered on the connection's most recent data channel,
   as a new Kobold string; the empty string when nothing is ready. */
kp_gptr
kpnc_read(int id)
{
    struct kpnc_conn *cn = conn_of(id);
    char buf[KPNC_BUF];
    int n;

    if (cn == NULL || cn->last_data == NULL)
        return kp_str_new("", 0);
    n = netchan_chan_read(cn->last_data, buf, sizeof buf);
    if (n <= 0)
        return kp_str_new("", 0);
    return kp_str_new(buf, n);
}

void
kpnc_close(int id)
{
    struct kpnc_conn *cn = conn_of(id);

    if (cn) {
        if (cn->c)
            netchan_close(cn->c);
        if (cn->fd >= 0)
            close(cn->fd);
        cn->c = NULL;
        cn->fd = -1;
        cn->last_data = NULL;
    }
}
