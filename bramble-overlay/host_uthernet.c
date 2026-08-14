/*
 * Host-side Uthernet II ($C0C4–$C0C7). Guest BusLoop does not update
 * registers.r[4..7] under a2bus inject (DATA reads stay 0x00), so the
 * 6502 W5100 window is completed here in the same RPC — AppleWin-style.
 */
#include "host_uthernet.h"
#include "cyw43.h"
#include "tapif.h"

#include <stdio.h>
#include <string.h>

#define W5100_MEM_SIZE 0x8000
#define W5100_MR_RST   0x80u
#define W5100_MR_AI    0x02u
#define W5100_S0       0x0400u
#define W5100_TX_BASE  0x4000u
#define W5100_RX_BASE  0x6000u
#define SN_MR          0x00u
#define SN_CR          0x01u
#define SN_SR          0x03u
#define SN_TX_FSR0     0x20u
#define SN_TX_RD0      0x22u
#define SN_TX_WR0      0x24u
#define SN_RX_RSR0     0x26u
#define SN_RX_RD0      0x28u
#define SN_CR_OPEN     0x01u
#define SN_CR_CLOSE    0x10u
#define SN_CR_SEND     0x20u
#define SN_CR_RECV     0x40u
#define SN_IR          0x02u
#define SN_IR_SEND_OK  0x10u
#define SN_MR_MACRAW   0x04u
#define SN_SR_CLOSED   0x00u
#define SN_SR_MACRAW   0x42u

static uint8_t u2_mr;
static uint16_t u2_addr;
static uint8_t u2_mem[W5100_MEM_SIZE];
static uint16_t rx_base, rx_size, tx_base, tx_size;
static uint16_t rx_wr, rx_rd, tx_rd;
static int ready;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t size_from_rmsr(uint8_t field)
{
    return (uint16_t)(1u << (10 + (field & 3u)));
}

static void apply_sizes(void)
{
    uint8_t rmsr = u2_mem[0x1a];
    uint8_t tmsr = u2_mem[0x1b];
    rx_size = size_from_rmsr(rmsr);
    tx_size = size_from_rmsr(tmsr);
    rx_base = W5100_RX_BASE;
    tx_base = W5100_TX_BASE;
    if (rx_base + rx_size > W5100_MEM_SIZE)
        rx_size = (uint16_t)(W5100_MEM_SIZE - rx_base);
    if (tx_base + tx_size > W5100_RX_BASE)
        tx_size = (uint16_t)(W5100_RX_BASE - tx_base);
}

static void chip_reset(void)
{
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
    rx_wr = rx_rd = tx_rd = 0;
    wr16(&u2_mem[W5100_S0 + SN_TX_RD0], 0);
    wr16(&u2_mem[W5100_S0 + SN_TX_WR0], 0);
    wr16(&u2_mem[W5100_S0 + SN_RX_RD0], 0);
    u2_mem[W5100_S0 + SN_SR] = SN_SR_CLOSED;
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

static uint16_t rx_used(void)
{
    if (rx_size == 0)
        return 0;
    uint16_t mask = (uint16_t)(rx_size - 1u);
    int d = (int)(rx_wr & mask) - (int)(rx_rd & mask);
    if (d < 0)
        d += (int)rx_size;
    return (uint16_t)d;
}

static uint16_t tx_free(void)
{
    if (tx_size == 0)
        return 0;
    uint16_t mask = (uint16_t)(tx_size - 1u);
    uint16_t wr = be16(&u2_mem[W5100_S0 + SN_TX_WR0]);
    int used = (int)(wr & mask) - (int)(tx_rd & mask);
    if (used < 0)
        used += (int)tx_size;
    if (used >= (int)tx_size)
        return 0;
    return (uint16_t)(tx_size - (uint16_t)used);
}

static uint8_t read_at(uint16_t a)
{
    a &= 0x7FFFu;
    if (a == 0)
        return u2_mr;
    if (a == W5100_S0 + SN_RX_RSR0)
        return (uint8_t)(rx_used() >> 8);
    if (a == W5100_S0 + SN_RX_RSR0 + 1)
        return (uint8_t)rx_used();
    if (a == W5100_S0 + SN_TX_FSR0)
        return (uint8_t)(tx_free() >> 8);
    if (a == W5100_S0 + SN_TX_FSR0 + 1)
        return (uint8_t)tx_free();
    return u2_mem[a];
}

static void rx_push(const uint8_t *frame, uint16_t len)
{
    if (!ready || rx_size == 0 || !frame || len == 0)
        return;
    uint16_t total = (uint16_t)(2u + len);
    uint16_t used = rx_used();
    uint16_t freeb = (uint16_t)(rx_size - used);
    if (total >= rx_size || freeb <= total)
        return;
    uint16_t mask = (uint16_t)(rx_size - 1u);
    uint16_t wr = rx_wr;
    uint16_t wire = (uint16_t)(len + 2u);
    u2_mem[rx_base + (wr & mask)] = (uint8_t)(wire >> 8);
    wr++;
    u2_mem[rx_base + (wr & mask)] = (uint8_t)wire;
    wr++;
    for (uint16_t i = 0; i < len; i++) {
        u2_mem[rx_base + (wr & mask)] = frame[i];
        wr++;
    }
    rx_wr = wr;
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
    rx_push(frame, (uint16_t)(14 + ip_len));
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

static void do_send(void)
{
    if (tx_size == 0)
        return;
    uint16_t mask = (uint16_t)(tx_size - 1u);
    uint16_t wr = be16(&u2_mem[W5100_S0 + SN_TX_WR0]);
    int n = (int)(wr & mask) - (int)(tx_rd & mask);
    if (n < 0)
        n += (int)tx_size;
    if (n <= 0)
        return;
    uint8_t buf[1518];
    int copy = n > (int)sizeof(buf) ? (int)sizeof(buf) : n;
    for (int i = 0; i < copy; i++)
        buf[i] = u2_mem[tx_base + ((tx_rd + (uint16_t)i) & mask)];
    tx_rd = (uint16_t)(tx_rd + (uint16_t)n);
    wr16(&u2_mem[W5100_S0 + SN_TX_RD0], tx_rd);
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

static void sock_cr(uint8_t v)
{
    uint8_t mr = u2_mem[W5100_S0 + SN_MR];
    switch (v) {
    case SN_CR_OPEN:
        rx_wr = rx_rd = tx_rd = 0;
        wr16(&u2_mem[W5100_S0 + SN_TX_RD0], 0);
        wr16(&u2_mem[W5100_S0 + SN_TX_WR0], 0);
        wr16(&u2_mem[W5100_S0 + SN_RX_RD0], 0);
        if ((mr & 0x0Fu) == SN_MR_MACRAW) {
            u2_mem[W5100_S0 + SN_SR] = SN_SR_MACRAW;
            fprintf(stderr, "[A2Bus] U2 socket0 MACRAW open\n");
        } else {
            u2_mem[W5100_S0 + SN_SR] = SN_SR_CLOSED;
        }
        break;
    case SN_CR_CLOSE:
        u2_mem[W5100_S0 + SN_SR] = SN_SR_CLOSED;
        break;
    case SN_CR_SEND:
        do_send();
        u2_mem[W5100_S0 + SN_IR] |= SN_IR_SEND_OK;
        break;
    case SN_CR_RECV:
        rx_rd = be16(&u2_mem[W5100_S0 + SN_RX_RD0]);
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
    if (a == W5100_S0 + SN_CR) {
        sock_cr(v);
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

void host_u2_on_tap_frame(const uint8_t *eth, int len)
{
    if (!ready || !eth || len < 14)
        return;
    if (u2_mem[W5100_S0 + SN_SR] != SN_SR_MACRAW)
        return;
    uint8_t mf = u2_mem[W5100_S0 + SN_MR] & 0x40u;
    if (mf) {
        int bcast = (eth[0] & eth[1] & eth[2] & eth[3] & eth[4] & eth[5]) == 0xFF;
        int ours = memcmp(eth, &u2_mem[0x09], 6) == 0;
        if (!bcast && !ours)
            return;
    }
    rx_push(eth, (uint16_t)len);
}

void host_u2_poll(void)
{
    if (cyw43.tap_fd >= 0)
        tapif_service(cyw43.tap_fd);
}
