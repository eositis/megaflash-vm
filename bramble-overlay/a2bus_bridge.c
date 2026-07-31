#include "a2bus_bridge.h"
#include "a2bus.h"
#include "devtools.h"
#include "emulator.h"
#include "pio.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
    int port;
    int listen_fd;
    int client_fd;
    int active;
    int handling;
    int saw_bus_io; /* READ/WRITE from MAME (not preflight PING/PEEK) */
    uint32_t regs_addr;
    a2bus_bridge_pump_fn pump;
    unsigned pump_steps;
} a2bus_bridge_t;

static a2bus_bridge_t br = {
    .port = 19765,
    .listen_fd = -1,
    .client_fd = -1,
    .active = 0,
    .handling = 0,
    .saw_bus_io = 0,
    .regs_addr = A2BUS_REGS_DEFAULT,
    .pump = NULL,
    /* Cap for command completion (BUSY clear). Idle DATA/PARAM use host-side path. */
    .pump_steps = 200000u,
};

/* MegaFlash BSS (pico2_debug) — keep in sync with megaflash.elf */
#define MF_DATA_BUF   0x2000caccu
#define MF_DATA_IDX   0x2000ccccu
#define MF_DATA_MODE  0x2006160au
#define MF_PARAM_BUF  0x20016fc8u
#define MF_PARAM_IDX  0x20016fe8u
#define MF_BUSYFLAG   0x80u
#define MF_MODE_LINEAR 0u
#define MF_DATA_MASK  0x1ffu
#define MF_PARAM_MASK 0x1fu

static void a2bus_drop_client(const char *why)
{
    if (br.client_fd >= 0) {
        close(br.client_fd);
        br.client_fd = -1;
    }
    fprintf(stderr, "[A2Bus] %s\n", why);
    /* Preflight only PINGs/PEEKs; MAME does READ/WRITE. Exit after MAME leaves. */
    if (br.saw_bus_io) {
        fprintf(stderr, "[A2Bus] MAME session ended — shutting down Bramble\n");
        semihost_exit_requested = 1;
        semihost_exit_code = 0;
    }
}

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void set_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void a2bus_bridge_set_port(int port)
{
    if (port > 0 && port < 65536) {
        br.port = port;
    }
}

void a2bus_bridge_set_regs_addr(uint32_t addr)
{
    if (addr != 0) {
        br.regs_addr = addr;
    }
}

void a2bus_bridge_set_pump(a2bus_bridge_pump_fn fn)
{
    br.pump = fn;
}

int a2bus_bridge_active(void)
{
    return br.active;
}

uint32_t a2bus_bridge_regs_from_elf(const char *elf_path)
{
    FILE *f = fopen(elf_path, "rb");
    if (!f) {
        return 0;
    }

    uint8_t ident[16];
    if (fread(ident, 1, 16, f) != 16 || ident[0] != 0x7f || ident[1] != 'E') {
        fclose(f);
        return 0;
    }

    uint32_t e_shoff = 0;
    uint16_t e_shentsize = 0, e_shnum = 0;
    fseek(f, 32, SEEK_SET);
    if (fread(&e_shoff, 4, 1, f) != 1) {
        fclose(f);
        return 0;
    }
    fseek(f, 46, SEEK_SET);
    if (fread(&e_shentsize, 2, 1, f) != 1 || fread(&e_shnum, 2, 1, f) != 1) {
        fclose(f);
        return 0;
    }
    if (e_shnum == 0 || e_shentsize < 40) {
        fclose(f);
        return 0;
    }

    typedef struct {
        uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
        uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
    } elf32_shdr_t;
    typedef struct {
        uint32_t st_name, st_value, st_size;
        uint8_t st_info, st_other;
        uint16_t st_shndx;
    } elf32_sym_t;

    elf32_shdr_t *shdrs = calloc(e_shnum, sizeof(elf32_shdr_t));
    if (!shdrs) {
        fclose(f);
        return 0;
    }
    fseek(f, (long)e_shoff, SEEK_SET);
    for (int i = 0; i < e_shnum; i++) {
        if (fread(&shdrs[i], sizeof(elf32_shdr_t), 1, f) != 1) {
            break;
        }
        if (e_shentsize > sizeof(elf32_shdr_t)) {
            fseek(f, e_shentsize - (long)sizeof(elf32_shdr_t), SEEK_CUR);
        }
    }

    uint32_t found = 0;
    for (int i = 0; i < e_shnum; i++) {
        if (shdrs[i].sh_type != 2) { /* SHT_SYMTAB */
            continue;
        }
        uint32_t strtab_idx = shdrs[i].sh_link;
        if (strtab_idx >= (uint32_t)e_shnum) {
            continue;
        }
        uint32_t str_size = shdrs[strtab_idx].sh_size;
        char *strtab = malloc(str_size);
        if (!strtab) {
            continue;
        }
        fseek(f, (long)shdrs[strtab_idx].sh_offset, SEEK_SET);
        if (fread(strtab, 1, str_size, f) != str_size) {
            free(strtab);
            continue;
        }

        uint32_t nsyms = shdrs[i].sh_size / sizeof(elf32_sym_t);
        fseek(f, (long)shdrs[i].sh_offset, SEEK_SET);
        for (uint32_t s = 0; s < nsyms; s++) {
            elf32_sym_t sym;
            if (fread(&sym, sizeof(sym), 1, f) != 1) {
                break;
            }
            if (sym.st_name >= str_size || sym.st_value == 0) {
                continue;
            }
            if (strcmp(&strtab[sym.st_name], "registers") == 0) {
                found = sym.st_value;
                break;
            }
        }
        free(strtab);
        if (found) {
            break;
        }
    }

    free(shdrs);
    fclose(f);
    return found;
}

int a2bus_bridge_init(void)
{
    if (br.port <= 0) {
        return 0;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[A2Bus] bridge: socket failed: %s\n", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)br.port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0) {
        fprintf(stderr, "[A2Bus] bridge: listen on 127.0.0.1:%d failed: %s\n",
                br.port, strerror(errno));
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    br.listen_fd = fd;
    br.active = 1;
    fprintf(stderr, "[A2Bus] bridge listening on 127.0.0.1:%d (regs @ 0x%08X)\n",
            br.port, br.regs_addr);
    return 0;
}

void a2bus_bridge_cleanup(void)
{
    if (br.client_fd >= 0) {
        close(br.client_fd);
        br.client_fd = -1;
    }
    if (br.listen_fd >= 0) {
        close(br.listen_fd);
        br.listen_fd = -1;
    }
    br.active = 0;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int recv_exact(int fd, uint8_t *buf, size_t len)
{
    size_t off = 0;
    int spins = 0;
    while (off < len) {
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++spins > 100000) {
                    return -1;
                }
                usleep(10);
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        spins = 0;
        off += (size_t)n;
    }
    return 0;
}

static int client_readable(int fd)
{
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    return r > 0 && FD_ISSET(fd, &rfds);
}

static uint8_t peek_reg(uint8_t nibble)
{
    uint32_t addr = br.regs_addr + (uint32_t)(nibble & 0xFu);
    if (rp2350_sram_ptr && addr >= 0x20000000u && addr < 0x20000000u + 520u * 1024u) {
        return rp2350_sram_ptr[addr - 0x20000000u];
    }
    return mem_read8(addr);
}

static void poke_reg(uint8_t nibble, uint8_t val)
{
    uint32_t addr = br.regs_addr + (uint32_t)(nibble & 0xFu);
    if (rp2350_sram_ptr && addr >= 0x20000000u && addr < 0x20000000u + 520u * 1024u) {
        rp2350_sram_ptr[addr - 0x20000000u] = val;
    } else {
        mem_write8(addr, val);
    }
}

/*
 * When STATUS is idle, mirror BusLoop DATA/PARAM/ID side-effects in host SRAM
 * so LOAD_CPANEL (58 pages × 256 data reads) does not need a guest pump per byte.
 * Returns 1 if handled (skip inject+pump).
 */
static int mf_native_mode(void)
{
    /* Slinky activation uses $C0C0–$C0C3 differently; only accelerate after ID is live. */
    uint8_t id = peek_reg(3);
    return id == 0x96u || id == 0x69u;
}

static int host_fast_read(uint8_t nibble, uint8_t *out)
{
    nibble &= 0xFu;
    if (!mf_native_mode() || (peek_reg(0) & MF_BUSYFLAG) != 0) {
        return 0;
    }
    if (nibble == 0u) {
        *out = peek_reg(0);
        return 1;
    }
    if (nibble == 2u) { /* DATA */
        *out = peek_reg(2);
        uint32_t idx = mem_read32(MF_DATA_IDX);
        /* dataBufferTransferMode is 1 byte; read32 pulls dhcp_pcb_refcount etc. */
        uint32_t mode = mem_read8(MF_DATA_MODE);
        if (mode == MF_MODE_LINEAR) {
            idx = (idx + 1u) & MF_DATA_MASK;
        } else {
            idx = (idx & 0x100u) ? (idx + 1u) & 0xffu : (idx | 0x100u);
        }
        mem_write32(MF_DATA_IDX, idx);
        poke_reg(2, mem_read8(MF_DATA_BUF + (idx & MF_DATA_MASK)));
        return 1;
    }
    if (nibble == 1u) { /* PARAM */
        *out = peek_reg(1);
        uint32_t idx = mem_read32(MF_PARAM_IDX);
        idx = (idx + 1u) & MF_PARAM_MASK;
        mem_write32(MF_PARAM_IDX, idx);
        poke_reg(1, mem_read8(MF_PARAM_BUF + (idx & MF_PARAM_MASK)));
        return 1;
    }
    if (nibble == 3u) { /* ID toggle */
        *out = peek_reg(3);
        poke_reg(3, (uint8_t)(~(*out)));
        return 1;
    }
    return 0;
}

static int host_fast_write(uint8_t nibble, uint8_t wdata)
{
    nibble &= 0xFu;
    if (!mf_native_mode() || (peek_reg(0) & MF_BUSYFLAG) != 0) {
        return 0;
    }
    if (nibble == 0u) {
        return 0; /* CMD — must run guest DoCommand */
    }
    if (nibble == 2u) { /* DATA */
        uint32_t idx = mem_read32(MF_DATA_IDX);
        mem_write8(MF_DATA_BUF + (idx & MF_DATA_MASK), wdata);
        /* Must be read8 — see host_fast_read. */
        uint32_t mode = mem_read8(MF_DATA_MODE);
        if (mode == MF_MODE_LINEAR) {
            idx = (idx + 1u) & MF_DATA_MASK;
        } else {
            idx = (idx & 0x100u) ? (idx + 1u) & 0xffu : (idx | 0x100u);
        }
        mem_write32(MF_DATA_IDX, idx);
        poke_reg(2, mem_read8(MF_DATA_BUF + (idx & MF_DATA_MASK)));
        return 1;
    }
    if (nibble == 1u) { /* PARAM */
        uint32_t idx = mem_read32(MF_PARAM_IDX);
        mem_write8(MF_PARAM_BUF + (idx & MF_PARAM_MASK), wdata);
        idx = (idx + 1u) & MF_PARAM_MASK;
        mem_write32(MF_PARAM_IDX, idx);
        poke_reg(1, mem_read8(MF_PARAM_BUF + (idx & MF_PARAM_MASK)));
        return 1;
    }
    if (nibble == 3u) {
        return 1; /* ID not writable */
    }
    return 0;
}

static void pump_guest(void)
{
    if (br.pump) {
        br.pump(br.pump_steps);
        return;
    }
    /*
     * Core1 only. After a CMD write, BusLoop sets BUSY then clears it when
     * DoCommand returns. Do not stop early if BUSY was never observed — a short
     * spin can finish before core1 enters DoCommand, so the Apple reads PARAM
     * while WE_KEY is already zeroed and treats configbyte1 as 0 (disables
     * slot‑4 autoboot). Only exit early after seeing BUSY then idle.
     */
    int seen_busy = (peek_reg(0) & MF_BUSYFLAG) != 0;
    for (unsigned i = 0; i < br.pump_steps; i++) {
        if (!cpu_is_halted_core(CORE1)) {
            cpu_step_core(CORE1);
        }
        pio_step();
        uint8_t s = peek_reg(0);
        if ((s & MF_BUSYFLAG) != 0) {
            seen_busy = 1;
        } else if (seen_busy) {
            break;
        }
    }
}

static void handle_one(void)
{
    uint8_t op = 0;
    if (recv_exact(br.client_fd, &op, 1) < 0) {
        a2bus_drop_client("bridge client recv error — dropped");
        return;
    }

    uint8_t status = 0;
    uint8_t data = 0;
    br.handling = 1;

    switch (op) {
    case A2BUS_BRIDGE_OP_PING:
        data = 0xA2;
        break;
    case A2BUS_BRIDGE_OP_PHI:
        a2bus_phi0_pulse_for_detect();
        pump_guest();
        data = 1;
        break;
    case A2BUS_BRIDGE_OP_READ: {
        uint8_t nibble = 0;
        if (recv_exact(br.client_fd, &nibble, 1) < 0) {
            br.handling = 0;
            a2bus_drop_client("bridge client recv error — dropped");
            return;
        }
        br.saw_bus_io = 1;
        /* Idle DATA/PARAM/ID/STATUS: host-side side effects (LOAD_CPANEL hot path). */
        if (!host_fast_read(nibble, &data)) {
            data = peek_reg(nibble);
            a2bus_inject_read(nibble & 0xFu);
            pump_guest();
        }
        break;
    }
    case A2BUS_BRIDGE_OP_WRITE: {
        uint8_t nibble = 0, wdata = 0;
        if (recv_exact(br.client_fd, &nibble, 1) < 0 ||
            recv_exact(br.client_fd, &wdata, 1) < 0) {
            br.handling = 0;
            a2bus_drop_client("bridge client recv error — dropped");
            return;
        }
        br.saw_bus_io = 1;
        if (!host_fast_write(nibble, wdata)) {
            a2bus_inject_write(nibble & 0xFu, wdata);
            pump_guest();
        }
        data = peek_reg(nibble);
        break;
    }
    case A2BUS_BRIDGE_OP_PEEK: {
        uint8_t nibble = 0;
        if (recv_exact(br.client_fd, &nibble, 1) < 0) {
            br.handling = 0;
            a2bus_drop_client("bridge client recv error — dropped");
            return;
        }
        data = peek_reg(nibble);
        break;
    }
    default:
        status = 1;
        break;
    }

    br.handling = 0;

    uint8_t rsp[2] = { status, data };
    if (send_all(br.client_fd, rsp, 2) < 0) {
        a2bus_drop_client("bridge client send error — dropped");
    }
}

void a2bus_bridge_poll(void)
{
    if (!br.active || br.handling) {
        return;
    }

    if (br.listen_fd >= 0 && br.client_fd < 0) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int cfd = accept(br.listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (cfd >= 0) {
            set_nonblock(cfd);
            set_nodelay(cfd);
            br.client_fd = cfd;
            fprintf(stderr, "[A2Bus] bridge client connected\n");
        }
    }

    if (br.client_fd < 0) {
        return;
    }

    /* Drop clients that closed without a pending request so accept() can run. */
    {
        fd_set rfds, efds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_ZERO(&efds);
        FD_SET(br.client_fd, &rfds);
        FD_SET(br.client_fd, &efds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        int r = select(br.client_fd + 1, &rfds, NULL, &efds, &tv);
        if (r > 0 && FD_ISSET(br.client_fd, &efds)) {
            a2bus_drop_client("bridge client error — dropped");
            return;
        }
        if (r > 0 && FD_ISSET(br.client_fd, &rfds)) {
            /* Peek: EOF means peer closed. */
            uint8_t b;
            ssize_t n = recv(br.client_fd, &b, 1, MSG_PEEK);
            if (n == 0) {
                a2bus_drop_client("bridge client disconnected");
                return;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                a2bus_drop_client("bridge client recv error — dropped");
                return;
            }
            if (n > 0) {
                handle_one();
            }
        }
    }
}
