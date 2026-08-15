/*
 * Host-side Uthernet II ($C0C4–$C0C7). Guest BusLoop does not update
 * registers.r[4..7] under a2bus inject (DATA reads stay 0x00), so the
 * 6502 W5100 window is completed here in the same RPC — AppleWin-style.
 *
 * Socket 0: MACRAW (IP65 / Contiki). Sockets 0–3: hardware TCP (A2Stream
 * uses socket 1 after ip65 DHCP/DNS on socket 0).
 */
#include "host_uthernet.h"
#include "cyw43.h"
#include "tapif.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define W5100_MEM_SIZE 0x8000
#define W5100_MR_RST   0x80u
#define W5100_MR_AI    0x02u
#define W5100_S0       0x0400u
#define W5100_TX_BASE  0x4000u
#define W5100_RX_BASE  0x6000u
#define SN_MR          0x00u
#define SN_CR          0x01u
#define SN_IR          0x02u
#define SN_SR          0x03u
#define SN_PORT0       0x04u
#define SN_DIPR0       0x0Cu
#define SN_DPORT0      0x10u
#define SN_TX_FSR0     0x20u
#define SN_TX_RD0      0x22u
#define SN_TX_WR0      0x24u
#define SN_RX_RSR0     0x26u
#define SN_RX_RD0      0x28u
#define SN_CR_OPEN     0x01u
#define SN_CR_CONNECT  0x04u
#define SN_CR_DISCON   0x08u
#define SN_CR_CLOSE    0x10u
#define SN_CR_SEND     0x20u
#define SN_CR_RECV     0x40u
#define SN_IR_SEND_OK  0x10u
#define SN_MR_TCP      0x01u
#define SN_MR_MACRAW   0x04u
#define SN_SR_CLOSED   0x00u
#define SN_SR_MACRAW   0x42u
#define SN_SR_INIT     0x13u
#define SN_SR_SYNSENT  0x15u
#define SN_SR_ESTABLISHED 0x17u
#define SN_SR_CLOSE_WAIT  0x1Cu

typedef struct {
    uint16_t sn;
    uint16_t rx_base, rx_size, tx_base, tx_size;
    uint16_t rx_wr, rx_rd, tx_rd;
    int tcp_fd;
    int tcp_connecting;
} u2_sock_t;

static uint8_t u2_mr;
static uint16_t u2_addr;
static uint8_t u2_mem[W5100_MEM_SIZE];
static u2_sock_t sk[4];
static int ready;
/* W5100 snapshots RSR/FSR for the 16-bit pair. A2Stream rereads until stable;
 * filling RX between the high and low byte yields a torn size and the player
 * treats later samples as earlier ones. */
static uint16_t u2_rsr_latch[4];
static uint16_t u2_fsr_latch[4];
static uint8_t u2_rsr_have[4];
static uint8_t u2_fsr_have[4];

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void sock_tcp_close(u2_sock_t *s)
{
    if (s->tcp_fd >= 0)
        close(s->tcp_fd);
    s->tcp_fd = -1;
    s->tcp_connecting = 0;
}

static void apply_sizes(void)
{
    uint8_t rmsr = u2_mem[0x1a];
    uint8_t tmsr = u2_mem[0x1b];
    uint16_t rx = W5100_RX_BASE;
    uint16_t tx = W5100_TX_BASE;
    for (int i = 0; i < 4; i++) {
        sk[i].sn = (uint16_t)(W5100_S0 + i * 0x100);
        uint16_t rbits = (uint16_t)((rmsr >> (i * 2)) & 3u);
        uint16_t tbits = (uint16_t)((tmsr >> (i * 2)) & 3u);
        sk[i].rx_size = (uint16_t)(1u << (10 + rbits));
        sk[i].tx_size = (uint16_t)(1u << (10 + tbits));
        sk[i].rx_base = rx;
        sk[i].tx_base = tx;
        if (rx + sk[i].rx_size > W5100_MEM_SIZE)
            sk[i].rx_size = (uint16_t)(W5100_MEM_SIZE - rx);
        if (tx + sk[i].tx_size > W5100_RX_BASE)
            sk[i].tx_size = (uint16_t)(W5100_RX_BASE - tx);
        rx = (uint16_t)(rx + sk[i].rx_size);
        tx = (uint16_t)(tx + sk[i].tx_size);
    }
}

static void chip_reset(void)
{
    for (int i = 0; i < 4; i++)
        sock_tcp_close(&sk[i]);
    memset(u2_mem, 0, sizeof(u2_mem));
    u2_mr = 0;
    u2_mem[0x17] = 0x07;
    u2_mem[0x18] = 0xD0;
    u2_mem[0x19] = 0x08;
    u2_mem[0x1a] = 0x06;
    u2_mem[0x1b] = 0x06;
    u2_mem[0x28] = 0x28;
    u2_mem[0x09] = 0x00;
    u2_mem[0x0a] = 0x08;
    u2_mem[0x0b] = 0xDC;
    u2_mem[0x0c] = 0xA2;
    u2_mem[0x0d] = 0xA2;
    u2_mem[0x0e] = 0xA2;
    apply_sizes();
    for (int i = 0; i < 4; i++) {
        sk[i].rx_wr = sk[i].rx_rd = sk[i].tx_rd = 0;
        sk[i].tcp_fd = -1;
        sk[i].tcp_connecting = 0;
        u2_rsr_have[i] = 0;
        u2_fsr_have[i] = 0;
        wr16(&u2_mem[sk[i].sn + SN_TX_RD0], 0);
        wr16(&u2_mem[sk[i].sn + SN_TX_WR0], 0);
        wr16(&u2_mem[sk[i].sn + SN_RX_RD0], 0);
        u2_mem[sk[i].sn + SN_SR] = SN_SR_CLOSED;
    }
    ready = 1;
}

static void autoinc(void)
{
    if (!(u2_mr & W5100_MR_AI))
        return;
    uint16_t next = (uint16_t)(u2_addr + 1u);
    /* W5100 8K windows: TX 0x4000–0x5FFF, RX 0x6000–0x7FFF. */
    if (u2_addr >= W5100_TX_BASE && u2_addr < W5100_RX_BASE && next >= W5100_RX_BASE)
        next = W5100_TX_BASE;
    else if (u2_addr >= W5100_RX_BASE && next >= W5100_MEM_SIZE)
        next = W5100_RX_BASE;
    else
        next &= 0x7FFFu;
    u2_addr = next;
}

static uint16_t rx_used(const u2_sock_t *s)
{
    uint16_t u = (uint16_t)(s->rx_wr - s->rx_rd);
    if (s->rx_size == 0 || u > s->rx_size)
        return s->rx_size;
    return u;
}

static uint16_t tx_used(const u2_sock_t *s)
{
    uint16_t wr = be16(&u2_mem[s->sn + SN_TX_WR0]);
    uint16_t u = (uint16_t)(wr - s->tx_rd);
    if (s->tx_size == 0 || u > s->tx_size)
        return s->tx_size;
    return u;
}

static uint16_t tx_free(const u2_sock_t *s)
{
    return (uint16_t)(s->tx_size - tx_used(s));
}

static int sock_index(uint16_t a)
{
    if (a < W5100_S0 || a >= 0x0800)
        return -1;
    return (int)((a - W5100_S0) >> 8);
}

static uint8_t read_at(uint16_t a)
{
    a &= 0x7FFFu;
    if (a == 0)
        return u2_mr;
    int si = sock_index(a);
    if (si >= 0) {
        uint16_t off = (uint16_t)(a & 0xFFu);
        u2_sock_t *s = &sk[si];
        if (off == SN_RX_RSR0) {
            u2_rsr_latch[si] = rx_used(s);
            u2_rsr_have[si] = 1;
            return (uint8_t)(u2_rsr_latch[si] >> 8);
        }
        if (off == SN_RX_RSR0 + 1) {
            if (!u2_rsr_have[si])
                u2_rsr_latch[si] = rx_used(s);
            u2_rsr_have[si] = 0;
            return (uint8_t)u2_rsr_latch[si];
        }
        if (off == SN_TX_FSR0) {
            u2_fsr_latch[si] = tx_free(s);
            u2_fsr_have[si] = 1;
            return (uint8_t)(u2_fsr_latch[si] >> 8);
        }
        if (off == SN_TX_FSR0 + 1) {
            if (!u2_fsr_have[si])
                u2_fsr_latch[si] = tx_free(s);
            u2_fsr_have[si] = 0;
            return (uint8_t)u2_fsr_latch[si];
        }
    }
    return u2_mem[a];
}

static int rx_push_macraw(const uint8_t *frame, uint16_t len)
{
    u2_sock_t *s = &sk[0];
    if (!ready || s->rx_size == 0 || !frame || len == 0)
        return 0;
    uint16_t total = (uint16_t)(2u + len);
    uint16_t used = rx_used(s);
    uint16_t freeb = (uint16_t)(s->rx_size - used);
    if (total >= s->rx_size || freeb <= total)
        return 0;
    uint16_t mask = (uint16_t)(s->rx_size - 1u);
    uint16_t wr = s->rx_wr;
    uint16_t wire = (uint16_t)(len + 2u);
    u2_mem[s->rx_base + (wr & mask)] = (uint8_t)(wire >> 8);
    wr++;
    u2_mem[s->rx_base + (wr & mask)] = (uint8_t)wire;
    wr++;
    for (uint16_t i = 0; i < len; i++) {
        u2_mem[s->rx_base + (wr & mask)] = frame[i];
        wr++;
    }
    s->rx_wr = wr;
    return 1;
}

static uint16_t ipv4_csum(const uint8_t *ip, int len)
{
    uint32_t s = 0;
    for (int i = 0; i < len; i += 2) {
        uint16_t w = (uint16_t)((ip[i] << 8) | ip[i + 1]);
        s += w;
    }
    while (s >> 16)
        s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)~s;
}

static void dhcp_reply(const uint8_t *req, int req_len, uint8_t msg)
{
    /* req is full Ethernet DHCP packet */
    if (req_len < 14 + 20 + 8 + 236)
        return;
    const uint8_t *bootp = req + 14 + 20 + 8;
    uint8_t frame[590];
    memset(frame, 0, sizeof(frame));
    /* ip65 still has 0.0.0.0 until ACK — unicast OFFER to 192.168.4.3 is dropped. */
    memset(frame, 0xFF, 6);
    static const uint8_t gw[6] = {0x02, 0xCA, 0xFE, 0xBA, 0xBE, 0x01};
    memcpy(frame + 6, gw, 6);
    frame[12] = 0x08;
    frame[13] = 0x00;
    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    ip[8] = 64;
    ip[9] = 17;
    ip[12] = 192;
    ip[13] = 168;
    ip[14] = 4;
    ip[15] = 1;
    ip[16] = 255;
    ip[17] = 255;
    ip[18] = 255;
    ip[19] = 255;
    uint8_t *udp = ip + 20;
    udp[0] = 0;
    udp[1] = 67;
    udp[2] = 0;
    udp[3] = 68;
    uint8_t *bp = udp + 8;
    bp[0] = 2;
    bp[1] = 1;
    bp[2] = 6;
    memcpy(bp + 4, bootp + 4, 4); /* xid */
    bp[10] = 0x80; /* broadcast flag — matches ip65 DISCOVER */
    bp[16] = 192;
    bp[17] = 168;
    bp[18] = 4;
    bp[19] = 3; /* yiaddr */
    bp[20] = 192;
    bp[21] = 168;
    bp[22] = 4;
    bp[23] = 1; /* siaddr */
    memcpy(bp + 28, bootp + 28, 16); /* chaddr */
    bp[236] = 99;
    bp[237] = 130;
    bp[238] = 83;
    bp[239] = 99;
    int o = 240;
    bp[o++] = 53;
    bp[o++] = 1;
    bp[o++] = msg;
    bp[o++] = 54;
    bp[o++] = 4;
    bp[o++] = 192;
    bp[o++] = 168;
    bp[o++] = 4;
    bp[o++] = 1;
    bp[o++] = 51;
    bp[o++] = 4;
    bp[o++] = 0;
    bp[o++] = 0;
    bp[o++] = 0x0e;
    bp[o++] = 0x10;
    bp[o++] = 1;
    bp[o++] = 4;
    bp[o++] = 255;
    bp[o++] = 255;
    bp[o++] = 255;
    bp[o++] = 0;
    bp[o++] = 3;
    bp[o++] = 4;
    bp[o++] = 192;
    bp[o++] = 168;
    bp[o++] = 4;
    bp[o++] = 1;
    bp[o++] = 6;
    bp[o++] = 4;
    bp[o++] = 8;
    bp[o++] = 8;
    bp[o++] = 8;
    bp[o++] = 8;
    bp[o++] = 255;
    int bootp_len = o;
    int udp_len = 8 + bootp_len;
    int ip_len = 20 + udp_len;
    udp[4] = (uint8_t)(udp_len >> 8);
    udp[5] = (uint8_t)udp_len;
    ip[2] = (uint8_t)(ip_len >> 8);
    ip[3] = (uint8_t)ip_len;
    uint16_t c = ipv4_csum(ip, 20);
    ip[10] = (uint8_t)(c >> 8);
    ip[11] = (uint8_t)c;
    rx_push_macraw(frame, (uint16_t)(14 + ip_len));
    fprintf(stderr, "[A2Bus] U2 DHCP %s broadcast yiaddr=192.168.4.3 (%d bytes)\n",
            msg == 2 ? "OFFER" : "ACK", 14 + ip_len);
}

static int dhcp_msg_type(const uint8_t *bootp, int bootp_len)
{
    if (bootp_len < 241)
        return -1;
    if (bootp[236] != 99 || bootp[237] != 130 || bootp[238] != 83 || bootp[239] != 99)
        return -1;
    int i = 240;
    while (i + 1 < bootp_len) {
        uint8_t t = bootp[i++];
        if (t == 255)
            break;
        if (t == 0)
            continue;
        if (i >= bootp_len)
            break;
        uint8_t l = bootp[i++];
        if (t == 53 && l >= 1 && i < bootp_len)
            return bootp[i];
        i += l;
    }
    return 1;
}

static int try_local_dhcp(const uint8_t *eth, int len)
{
    if (len < 14 + 20 + 8 + 236)
        return 0;
    if (eth[12] != 0x08 || eth[13] != 0x00)
        return 0;
    const uint8_t *ip = eth + 14;
    if (ip[9] != 17)
        return 0;
    int ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || len < 14 + ihl + 8)
        return 0;
    const uint8_t *udp = ip + ihl;
    uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
    if (dport != 67)
        return 0;
    const uint8_t *bootp = udp + 8;
    int blen = len - (int)(bootp - eth);
    int mt = dhcp_msg_type(bootp, blen);
    if (mt == 1)
        dhcp_reply(eth, len, 2);
    else if (mt == 3)
        dhcp_reply(eth, len, 5);
    else
        dhcp_reply(eth, len, 2);
    return 1;
}

static void do_send_macraw(u2_sock_t *s)
{
    if (s->tx_size == 0)
        return;
    uint16_t mask = (uint16_t)(s->tx_size - 1u);
    int n = (int)tx_used(s);
    if (n <= 0)
        return;
    uint8_t buf[1518];
    int copy = n > (int)sizeof(buf) ? (int)sizeof(buf) : n;
    for (int i = 0; i < copy; i++)
        buf[i] = u2_mem[s->tx_base + ((s->tx_rd + (uint16_t)i) & mask)];
    s->tx_rd = (uint16_t)(s->tx_rd + (uint16_t)n);
    wr16(&u2_mem[s->sn + SN_TX_RD0], s->tx_rd);
    static int txn;
    if (txn < 12) {
        txn++;
        fprintf(stderr, "[A2Bus] U2 MACRAW TX %d bytes\n", copy);
    }
    if (try_local_dhcp(buf, copy))
        return;
    if (cyw43.tap_fd >= 0)
        tapif_write(cyw43.tap_fd, buf, copy);
}

static void do_send_tcp(u2_sock_t *s)
{
    if (s->tcp_fd < 0 || s->tx_size == 0)
        return;
    uint16_t mask = (uint16_t)(s->tx_size - 1u);
    int n = (int)tx_used(s);
    if (n <= 0)
        return;
    uint8_t buf[2048];
    int copy = n > (int)sizeof(buf) ? (int)sizeof(buf) : n;
    for (int i = 0; i < copy; i++)
        buf[i] = u2_mem[s->tx_base + ((s->tx_rd + (uint16_t)i) & mask)];
    int off = 0;
    while (off < copy) {
        ssize_t sent = send(s->tcp_fd, buf + off, (size_t)(copy - off), 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            fprintf(stderr, "[A2Bus] U2 TCP send: %s\n", strerror(errno));
            sock_tcp_close(s);
            u2_mem[s->sn + SN_SR] = SN_SR_CLOSED;
            return;
        }
        off += (int)sent;
    }
    s->tx_rd = (uint16_t)(s->tx_rd + (uint16_t)off);
    wr16(&u2_mem[s->sn + SN_TX_RD0], s->tx_rd);
    static int txn;
    if (txn < 8) {
        txn++;
        fprintf(stderr, "[A2Bus] U2 TCP TX %d bytes\n", off);
    }
}

static int tcp_connect_start(u2_sock_t *s)
{
    sock_tcp_close(s);
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        fprintf(stderr, "[A2Bus] U2 TCP socket: %s\n", strerror(errno));
        return 0;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl != -1)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    memcpy(&dest.sin_addr.s_addr, &u2_mem[s->sn + SN_DIPR0], 4);
    dest.sin_port = htons(be16(&u2_mem[s->sn + SN_DPORT0]));
    const uint8_t *d = &u2_mem[s->sn + SN_DIPR0];
    fprintf(stderr, "[A2Bus] U2 socket%u TCP CONNECT %u.%u.%u.%u:%u\n",
            (unsigned)((s->sn - W5100_S0) >> 8),
            d[0], d[1], d[2], d[3],
            (unsigned)be16(&u2_mem[s->sn + SN_DPORT0]));
    int rc = connect(fd, (struct sockaddr *)&dest, sizeof(dest));
    s->tcp_fd = fd;
    if (rc == 0) {
        u2_mem[s->sn + SN_SR] = SN_SR_ESTABLISHED;
        s->tcp_connecting = 0;
        fprintf(stderr, "[A2Bus] U2 TCP ESTABLISHED (immediate)\n");
        return 1;
    }
    if (errno == EINPROGRESS) {
        u2_mem[s->sn + SN_SR] = SN_SR_SYNSENT;
        s->tcp_connecting = 1;
        return 1;
    }
    fprintf(stderr, "[A2Bus] U2 TCP connect: %s\n", strerror(errno));
    sock_tcp_close(s);
    u2_mem[s->sn + SN_SR] = SN_SR_CLOSED;
    return 0;
}

static void tcp_poll_sock(u2_sock_t *s)
{
    if (s->tcp_fd < 0)
        return;
    struct pollfd p;
    p.fd = s->tcp_fd;
    p.events = POLLIN;
    if (s->tcp_connecting)
        p.events |= POLLOUT;
    p.revents = 0;
    if (poll(&p, 1, 0) < 0)
        return;
    if (s->tcp_connecting && (p.revents & (POLLOUT | POLLERR | POLLHUP))) {
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(s->tcp_fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err != 0) {
            fprintf(stderr, "[A2Bus] U2 TCP connect failed: %s\n", strerror(err));
            sock_tcp_close(s);
            u2_mem[s->sn + SN_SR] = SN_SR_CLOSED;
            return;
        }
        s->tcp_connecting = 0;
        u2_mem[s->sn + SN_SR] = SN_SR_ESTABLISHED;
        fprintf(stderr, "[A2Bus] U2 TCP ESTABLISHED\n");
    }
    if (u2_mem[s->sn + SN_SR] != SN_SR_ESTABLISHED &&
        u2_mem[s->sn + SN_SR] != SN_SR_CLOSE_WAIT)
        return;
    if (!(p.revents & (POLLIN | POLLHUP)))
        return;
    uint16_t used = rx_used(s);
    if (used >= s->rx_size)
        return;
    uint16_t space = (uint16_t)(s->rx_size - used);
    uint8_t buf[512];
    size_t want = space > sizeof(buf) ? sizeof(buf) : (size_t)space;
    ssize_t n = recv(s->tcp_fd, buf, want, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        sock_tcp_close(s);
        u2_mem[s->sn + SN_SR] = SN_SR_CLOSED;
        return;
    }
    if (n == 0) {
        u2_mem[s->sn + SN_SR] = SN_SR_CLOSE_WAIT;
        return;
    }
    uint16_t mask = (uint16_t)(s->rx_size - 1u);
    uint16_t wr = s->rx_wr;
    for (ssize_t i = 0; i < n; i++) {
        u2_mem[s->rx_base + (wr & mask)] = buf[i];
        wr++;
    }
    s->rx_wr = wr;
}

static void sock_cr(u2_sock_t *s, uint8_t v)
{
    uint8_t mr = u2_mem[s->sn + SN_MR] & 0x0Fu;
    switch (v) {
    case SN_CR_OPEN:
        sock_tcp_close(s);
        s->rx_wr = s->rx_rd = s->tx_rd = 0;
        wr16(&u2_mem[s->sn + SN_TX_RD0], 0);
        wr16(&u2_mem[s->sn + SN_TX_WR0], 0);
        wr16(&u2_mem[s->sn + SN_RX_RD0], 0);
        u2_rsr_have[(s->sn - W5100_S0) >> 8] = 0;
        u2_fsr_have[(s->sn - W5100_S0) >> 8] = 0;
        if (mr == SN_MR_MACRAW && s == &sk[0]) {
            u2_mem[s->sn + SN_SR] = SN_SR_MACRAW;
            fprintf(stderr, "[A2Bus] U2 socket0 MACRAW open\n");
        } else if (mr == SN_MR_TCP) {
            u2_mem[s->sn + SN_SR] = SN_SR_INIT;
            fprintf(stderr, "[A2Bus] U2 socket%u TCP OPEN (INIT)\n",
                    (unsigned)((s->sn - W5100_S0) >> 8));
        } else {
            u2_mem[s->sn + SN_SR] = SN_SR_CLOSED;
        }
        break;
    case SN_CR_CONNECT:
        if (mr == SN_MR_TCP && u2_mem[s->sn + SN_SR] == SN_SR_INIT)
            tcp_connect_start(s);
        break;
    case SN_CR_DISCON:
    case SN_CR_CLOSE:
        sock_tcp_close(s);
        u2_mem[s->sn + SN_SR] = SN_SR_CLOSED;
        break;
    case SN_CR_SEND:
        if (u2_mem[s->sn + SN_SR] == SN_SR_MACRAW)
            do_send_macraw(s);
        else
            do_send_tcp(s);
        u2_mem[s->sn + SN_IR] |= SN_IR_SEND_OK;
        break;
    case SN_CR_RECV:
        s->rx_rd = be16(&u2_mem[s->sn + SN_RX_RD0]);
        break;
    default:
        break;
    }
}

static void write_at(uint16_t a, uint8_t v)
{
    a &= 0x7FFFu;
    if (a == 0) {
        if (v & W5100_MR_RST)
            chip_reset();
        else
            u2_mr = v;
        return;
    }
    u2_mem[a] = v;
    if (a == 0x1a || a == 0x1b)
        apply_sizes();
    int si = sock_index(a);
    if (si >= 0 && (a & 0xFFu) == SN_CR) {
        sock_cr(&sk[si], v);
        u2_mem[a] = 0; /* CR self-clears */
    }
}

void host_u2_init(void)
{
    chip_reset();
    tapif_set_eth_sniff(host_u2_on_tap_frame);
}

int host_u2_read(uint8_t nibble, uint8_t *out)
{
    if (!ready)
        chip_reset();
    /* A2Stream SINGLE_SOCKET PWM reads RX DATA with auto-increment. recv() on
     * every byte tears Sn_RX_RSR and can wrap new payload over unread samples. */
    uint8_t nb = (uint8_t)(nibble & 3u);
    if (!(nb == 3u && u2_addr >= W5100_TX_BASE))
        host_u2_poll();
    switch (nibble & 3u) {
    case 0:
        *out = u2_mr;
        return 1;
    case 1:
        *out = (uint8_t)(u2_addr >> 8);
        return 1;
    case 2:
        *out = (uint8_t)(u2_addr & 0xFFu);
        return 1;
    default: {
        *out = read_at(u2_addr);
        autoinc();
        return 1;
    }
    }
}

int host_u2_write(uint8_t nibble, uint8_t wdata)
{
    if (!ready)
        chip_reset();
    host_u2_poll();
    switch (nibble & 3u) {
    case 0:
        if (wdata & W5100_MR_RST)
            chip_reset();
        else
            u2_mr = wdata;
        return 1;
    case 1:
        u2_addr = (uint16_t)((u2_addr & 0x00FFu) | ((uint16_t)wdata << 8));
        return 1;
    case 2:
        u2_addr = (uint16_t)((u2_addr & 0xFF00u) | wdata);
        return 1;
    default:
        write_at(u2_addr, wdata);
        autoinc();
        return 1;
    }
}

int host_u2_on_tap_frame(const uint8_t *eth, int len)
{
    if (!ready || !eth || len < 14)
        return 1;
    if (u2_mem[sk[0].sn + SN_SR] != SN_SR_MACRAW)
        return 1;
    uint8_t mf = u2_mem[sk[0].sn + SN_MR] & 0x40u;
    if (mf) {
        int bcast = (eth[0] & eth[1] & eth[2] & eth[3] & eth[4] & eth[5]) == 0xFF;
        int ours = memcmp(eth, &u2_mem[0x09], 6) == 0;
        if (!bcast && !ours)
            return 1;
    }
    return rx_push_macraw(eth, (uint16_t)len);
}

void host_u2_poll(void)
{
    if (cyw43.tap_fd >= 0)
        tapif_service(cyw43.tap_fd);
    for (int i = 0; i < 4; i++)
        tcp_poll_sock(&sk[i]);
}
