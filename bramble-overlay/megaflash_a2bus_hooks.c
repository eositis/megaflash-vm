/*
 * MegaFlash a2bus guest hooks: Apple-bus / SPI / bring-up, plus thin host
 * stubs where guest UDP TX is unreliable (DNS/NTP). Radio JOIN + DHCP go
 * through Bramble CYW43 (SSID/PW → fake DHCP lease 192.168.4.2 into guest
 * lwIP netif). Do not poke netif IPs or host-complete DoTestWifi strings —
 * firmware FormatIPAddr owns the CP display path.
 */
#include "a2bus_bridge.h"
#include "bramble_ext.h"
#include "megaflash_guest_addrs.h"
#include "usb_guest.h"
#include "timer.h"
#include "cyw43.h"
#include "emulator.h"
#include "corepool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* CNTPTask layout (MegaFlash firmware): CUDPTask then attempt @0x58, time_t @0x60. */
#define A2BUS_CNTP_ATTEMPT_OFF      0x58u
#define A2BUS_CNTP_EPOCH_OFF        0x60u
#define A2BUS_CUDP_SERVER_ADDR_OFF  0x18u
#define A2BUS_CUDP_COMPLETED_OFF    0x4cu
#define A2BUS_NTP_DELTA             2208988800ull
#define A2BUS_SEND_NTP_REQUEST      0x1000918cu

#if !defined(_WIN32)
/* Host UDP SNTP; returns unix seconds or 0 on failure. */
static uint64_t a2bus_host_sntp(uint32_t server_ip_le) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return 0;
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(123);
    sa.sin_addr.s_addr = server_ip_le; /* guest stores network-order IPv4 */

    uint8_t req[48];
    memset(req, 0, sizeof(req));
    req[0] = 0x23; /* LI=0 VN=4 Mode=3 (client) */
    if (sendto(fd, req, sizeof(req), 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return 0;
    }
    uint8_t resp[48];
    ssize_t n = recvfrom(fd, resp, sizeof(resp), 0, NULL, NULL);
    close(fd);
    if (n < 48 || (resp[0] & 0x7) != 4 || resp[1] == 0) {
        return 0;
    }
    uint32_t sec1900 = ((uint32_t)resp[40] << 24) | ((uint32_t)resp[41] << 16) |
                       ((uint32_t)resp[42] << 8) | (uint32_t)resp[43];
    if (sec1900 < 3955046400u) {
        return (uint64_t)sec1900 + 0x100000000ull - A2BUS_NTP_DELTA;
    }
    return (uint64_t)sec1900 - A2BUS_NTP_DELTA;
}
#endif

static void a2bus_complete_ntp_on_host(uint32_t task) {
    uint64_t epoch = 0;
#if !defined(_WIN32)
    uint32_t sip = mem_read32(task + A2BUS_CUDP_SERVER_ADDR_OFF);
    if (sip != 0u) {
        epoch = a2bus_host_sntp(sip);
    }
#endif
    if (epoch == 0) {
        epoch = (uint64_t)time(NULL);
        fprintf(stderr, "[A2Bus] host NTP fallback to local time\n");
    }
    mem_write32(task + A2BUS_CNTP_EPOCH_OFF, (uint32_t)(epoch & 0xffffffffu));
    mem_write32(task + A2BUS_CNTP_EPOCH_OFF + 4u, (uint32_t)(epoch >> 32));
    mem_write8(task + A2BUS_CUDP_COMPLETED_OFF, 1u);
    {
        uint32_t attempt = mem_read32(task + A2BUS_CNTP_ATTEMPT_OFF);
        mem_write32(task + A2BUS_CNTP_ATTEMPT_OFF, attempt + 1u);
    }
    fprintf(stderr, "[A2Bus] host NTP unix epoch = %llu\n",
            (unsigned long long)epoch);
}




static int a2bus_wifi_hooks(void);
static int a2bus_spi_flash_hooks(void);
static void usb_guest_a2bus_tx_byte(uint8_t ch);
static int a2bus_fix_picoram_veneer(void) {
    if (!a2bus_bridge_active()) {
        return 0;
    }
    uint32_t pc = cpu.r[15] & ~1u;
    uint16_t hi = mem_read16(pc);
    uint16_t lo = mem_read16(pc + 2u);
    if (hi != 0xF85Fu || lo != 0xF000u) {
        return 0;
    }
    uint32_t target = mem_read32(pc + 4u) & ~1u;
    if (target == 0x2000147cu) { /* CopyMemoryAligned */
        usb_guest_stub_copy_memory_entry();
        return 1;
    }
    if (target == 0x2000135cu) { /* ReadBlock */
        usb_guest_stub_read_block();
        return 1;
    }
    if (target == 0x200013f0u) { /* WriteBlock */
        usb_guest_stub_write_block();
        return 1;
    }
    /* TranslateUnitNum: flash veneer → SRAM; needed by GetMediumType / GETVOLINFO. */
    if (pc == 0x10034dc0u && target == 0x200011fcu) {
        cpu.r[15] = target | 1u;
        return 1;
    }
    /* CheckWriteEnableKey: same class; DriveMapping uses index 1. */
    if (pc == 0x10034cd8u && target == 0x2000045cu) {
        cpu.r[15] = target | 1u;
        return 1;
    }
    /* core0Loop: Thumb ldr.w pc veneer → SRAM; needed for TestWifi IPC. */
    if (pc == 0x10034cf8u && target == 0x20000140u) {
        cpu.r[15] = target | 1u;
        return 1;
    }
    /* Other veneers: only rewrite when the insn itself lives in Pico RAM. */
    if (pc < 0x20000120u || pc >= 0x20005174u) {
        return 0;
    }
    cpu.r[15] = target | 1u;
    return 1;
}

static void usb_guest_a2bus_tx_byte(uint8_t ch) {
    static char line[192];
    static size_t len;
    static int suppress_logged;
    static int show_u2macraw = -1;

    if (show_u2macraw < 0) {
        const char *e = getenv("BRAMBLE_A2BUS_U2MACRAW");
        show_u2macraw = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
    }

    if (ch == '\r') {
        return;
    }
    if (ch != '\n' && len < sizeof(line) - 1u) {
        line[len++] = (char)ch;
        return;
    }
    line[len] = '\0';
    if (len >= 9u && memcmp(line, "[u2macraw]", 9) == 0 && !show_u2macraw) {
        if (!suppress_logged++) {
            fprintf(stderr,
                    "[A2Bus] suppressing [u2macraw] telemetry "
                    "(BRAMBLE_A2BUS_U2MACRAW=1 to show)\n");
        }
    } else if (len > 0u) {
        fputs(line, stderr);
        fputc('\n', stderr);
    } else {
        fputc('\n', stderr);
    }
    len = 0;
}

/* MegaFlash WiFi settings live in configBuffer; SSID at +0x5A. */
#define USB_GUEST_WIFI_SSID           (USB_GUEST_CONFIG_BUFFER + 0x5Au)
#define USB_GUEST_TEST_WIFI           0x100085f4u /* TestWifi */
#define USB_GUEST_DO_TEST_WIFI        0x10001d1cu /* DoTestWifi (core1) */
#define USB_GUEST_GET_NETWORK_TIME    0x1000859cu /* GetNetworkTime */
#define USB_GUEST_CONNECT_WIFI        0x10008bacu /* CUDPTask::ConnectWifi */
#define USB_GUEST_INIT_CYW43          0x100088f4u /* CUDPTask::InitCyw43 */
#define USB_GUEST_CYW43_ARCH_INIT     0x1001b030u /* cyw43_arch_init (InitPicoLed) */
#define USB_GUEST_CYW43_GPIO_PUT      0x1001afa0u /* cyw43_arch_gpio_put */
#define USB_GUEST_DATA_START          0x20000120u /* __data_start__ / core1Main */
#define USB_GUEST_DATA_LMA            0x100b3934u /* .data load address in flash */
/* Restore only BusLoop RAM code — not all of .data. A full .data reload after
 * cyw43_arch_init zeros default_alarm_pool (+16 lock) and breaks NETPUMP/NTP
 * (assert default_alarm_pool_initialized). BusLoopSlinky ends before LoadFAC. */
#define USB_GUEST_BUSLOOP_RAM_END     0x20001718u /* LoadFAC */
#define USB_GUEST_BUSLOOP_RAM_SIZE    (USB_GUEST_BUSLOOP_RAM_END - USB_GUEST_DATA_START)
#define USB_GUEST_NETERR_SSIDNOTSET   3u
#define USB_GUEST_NETERR_NONE         11u
#define USB_GUEST_DATA_XFER_MODE      0x2006160au /* dataBufferTransferMode */
#define USB_GUEST_DATA_BUFFER_INDEX   0x2000ccccu
#define USB_GUEST_PARAM_BUFFER_INDEX  0x20016fe8u
#define USB_GUEST_MALLOC_MUTEX        0x20005164u /* malloc_mutex (.data, often still zero) */
#define USB_GUEST_MUTEX_ENTER_VENEER  0x10034da0u /* __mutex_enter_blocking_veneer */
#define USB_GUEST_MUTEX_EXIT_VENEER   0x10034d70u /* __mutex_exit_veneer */
#define USB_GUEST_INIT_RTC            0x10004accu /* InitRTC — track epoch for CP clock */

/* Unlocked mutex owner = -1; spinlock slot in guest SRAM for [0]. */
static uint32_t a2bus_mutex_spinlock_byte = 0x20061f00u;

/* Guest AON RTC: CP clock / ProDOS read calendar via aon_timer after InitRTC. */
static int a2bus_rtc_valid;
static time_t a2bus_rtc_base_local;
static time_t a2bus_rtc_host_at_set;

static const int32_t a2bus_tzhour[] = {
    -12,-11,-10,-9,-9,-8,-7,-6,-5,-4,-3,-3,-2,-1,0,1,2,3,3,4,4,5,5,5,6,6,7,8,8,9,9,10,10,11,12,12,13,14
};
static const int32_t a2bus_tzmin[] = {
    0,0,0,30,0,0,0,0,0,0,30,0,0,0,0,0,0,0,30,0,30,0,30,45,0,30,0,0,45,0,30,0,30,0,0,45,0,0
};

static int32_t a2bus_guest_tz_offset_sec(void) {
    uint8_t id = mem_read8(USB_GUEST_CONFIG_BUFFER + 7u);
    size_t n = sizeof(a2bus_tzhour) / sizeof(a2bus_tzhour[0]);
    if ((size_t)id >= n) {
        id = 14u;
    }
    int32_t hour = a2bus_tzhour[id];
    int32_t min = a2bus_tzmin[id];
    int32_t offset_min = hour * 60;
    if (hour < 0) {
        offset_min -= min;
    } else {
        offset_min += min;
    }
    return offset_min * 60;
}

static void a2bus_write_tm(uint32_t tm_ptr, const struct tm *t) {
    mem_write32(tm_ptr + 0u,  (uint32_t)t->tm_sec);
    mem_write32(tm_ptr + 4u,  (uint32_t)t->tm_min);
    mem_write32(tm_ptr + 8u,  (uint32_t)t->tm_hour);
    mem_write32(tm_ptr + 12u, (uint32_t)t->tm_mday);
    mem_write32(tm_ptr + 16u, (uint32_t)t->tm_mon);
    mem_write32(tm_ptr + 20u, (uint32_t)t->tm_year);
    mem_write32(tm_ptr + 24u, (uint32_t)t->tm_wday);
    mem_write32(tm_ptr + 28u, (uint32_t)t->tm_yday);
    mem_write32(tm_ptr + 32u, (uint32_t)t->tm_isdst);
}

static void a2bus_apply_init_rtc(time_t utc_epoch, int32_t offset_sec) {
    a2bus_rtc_base_local = utc_epoch + (time_t)offset_sec;
    a2bus_rtc_host_at_set = time(NULL);
    a2bus_rtc_valid = 1;
    mem_write8(USB_GUEST_RTC_RUNNING_BSS, 1u);
    {
        struct tm *t = gmtime(&a2bus_rtc_base_local);
        if (t) {
            fprintf(stderr,
                    "[A2Bus] InitRTC from NTP: utc=%ld offset=%ld → "
                    "%04d-%02d-%02d %02d:%02d:%02d (rtcRunning=1)\n",
                    (long)utc_epoch, (long)offset_sec,
                    t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                    t->tm_hour, t->tm_min, t->tm_sec);
        }
        fflush(stderr);
    }
}

static time_t a2bus_rtc_now_local(void) {
    if (!a2bus_rtc_valid) {
        return time(NULL);
    }
    return a2bus_rtc_base_local + (time(NULL) - a2bus_rtc_host_at_set);
}

/*
 * CMD_GETTIMESTR / CP DisplayTime: 8 high-bit chars at cols 32-39 ($7D0+32).
 * Native DoGetTimeString uses sprintf → _svfprintf_r, which hangs under a2bus
 * (BUSY timeout at ~0x1002DE12). Unstick then leaves PARAM garbage that the
 * CP redraws every cgetc_showclock tick — the flickering bottom-line junk.
 */
static void a2bus_host_do_get_time_string(void) {
    char buf[16];
    uint32_t i;

    if (a2bus_rtc_valid || mem_read8(USB_GUEST_RTC_RUNNING_BSS)) {
        time_t now = a2bus_rtc_now_local();
        /* InitRTC stores timezone-adjusted wall time as a fake UTC epoch. */
        struct tm *t = gmtime(&now);
        if (t) {
            int h = t->tm_hour;
            int m = t->tm_min;
            int h12 = h;
            if (h12 == 0) {
                h12 = 12;
            } else if (h12 >= 13) {
                h12 -= 12;
            }
            snprintf(buf, sizeof(buf), "%2d:%02d AM", h12, m);
            if (h >= 12) {
                buf[6] = 'P';
            }
            for (i = 0; i < 8u; i++) {
                mem_write8(USB_GUEST_PARAMETER_BUFFER + i,
                           (uint8_t)buf[i] | 0x80u);
            }
        } else {
            for (i = 0; i < 8u; i++) {
                mem_write8(USB_GUEST_PARAMETER_BUFFER + i, (uint8_t)(' ' | 0x80));
            }
        }
    } else {
        for (i = 0; i < 8u; i++) {
            mem_write8(USB_GUEST_PARAMETER_BUFFER + i, (uint8_t)(' ' | 0x80));
        }
    }
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 8u, 0);
    mem_write32(USB_GUEST_PARAM_BUFFER_INDEX, 0);
    mem_write8(USB_GUEST_REGISTERS + 1u, mem_read8(USB_GUEST_PARAMETER_BUFFER));
    {
        uint8_t st = mem_read8(USB_GUEST_REGISTERS);
        mem_write8(USB_GUEST_REGISTERS, (uint8_t)(st & (uint8_t)~0x5Fu));
    }
}

/*
 * Empty-SSID TestWifi/NTP uses C++ exceptions Bramble cannot unwind — fail fast.
 * Configured-SSID: run real DoTestWifi (core1 BUSY-wait + core0 IPC TestWifi).
 * Pump steps both cores so that matches hardware. Do not host-complete the
 * command or fabricate dataBuffer — FormatIPAddr is firmware's job.
 *
 * BRAMBLE_A2BUS_STUB_WIFI=1 — emergency stub of cyw43_arch_init only (BusLoop WIP).
 */
static int a2bus_stub_cyw43(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("BRAMBLE_A2BUS_STUB_WIFI");
        cached = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        if (cached) {
            fprintf(stderr, "[A2Bus] BRAMBLE_A2BUS_STUB_WIFI=1 — stub cyw43_arch_init\n");
        } else {
            fprintf(stderr,
                    "[A2Bus] real CYW43 path (radio stub; native DoTestWifi via core0 IPC)\n");
        }
    }
    return cached;
}

static void a2bus_ensure_malloc_mutex(void) {
    static int ready;
    if (ready++) {
        return;
    }
    if (mem_read32(USB_GUEST_MALLOC_MUTEX) == 0u) {
        mem_write32(USB_GUEST_MALLOC_MUTEX, a2bus_mutex_spinlock_byte);
        mem_write8(a2bus_mutex_spinlock_byte, 0);
    }
    mem_write8(USB_GUEST_MALLOC_MUTEX + 4u, 0xFFu);
    mem_write8(USB_GUEST_STDIO_MUTEX + 4u, 0xFFu);
    fprintf(stderr, "[A2Bus] malloc_mutex unlocked (owner=-1)\n");
}

/* Launch BusLoop after CYW43 bring-up so core1 PIO is not HardFaulted by
 * concurrent gSPI/DMA on core0 (PC→0x1FFF8F6C during clmload). */
static void a2bus_ensure_busloop_core1(void) {
    static int launched;
    static int saw_cyw43_arch_init;
    if (launched || !a2bus_bridge_active()) {
        return;
    }
    uint32_t c1pc = cores[CORE1].r[15] & ~1u;
    if (!cores[CORE1].is_halted && c1pc >= 0x20000120u && c1pc < 0x20010000u) {
        launched = 1;
        return;
    }

    uint32_t pc = cpu.r[15] & ~1u;
    if (pc == USB_GUEST_CYW43_ARCH_INIT) {
        saw_cyw43_arch_init = 1;
    }

    int ready = 0;
    const char *why = NULL;
    if (!cyw43.enabled) {
        ready = 1;
        why = "no-wifi";
    } else if (a2bus_stub_cyw43()) {
        ready = 1;
        why = "stub-wifi";
    } else if (saw_cyw43_arch_init && cyw43.feedbead_ok &&
               (pc == 0x10004f18u || pc == 0x10004ef0u || pc == 0x10004f1cu)) {
        /* After cyw43_arch_init: next is TurnOffPicoLed (also matches its entry). */
        ready = 1;
        why = "cyw43_arch_init-done";
    }
    if (!ready) {
        return;
    }

    launched = 1;
    /* cyw43_arch_init DMA/buffers can clobber BusLoop in Pico RAM. Reload only
     * that code range so SDK/runtime .data (alarm pool, workers) stays intact. */
    if (mem_guest_memcpy(USB_GUEST_DATA_START, USB_GUEST_DATA_LMA,
                         USB_GUEST_BUSLOOP_RAM_SIZE)) {
        fprintf(stderr,
                "[A2Bus] restored BusLoop RAM (%u bytes) from flash before launch\n",
                (unsigned)USB_GUEST_BUSLOOP_RAM_SIZE);
    } else {
        fprintf(stderr, "[A2Bus] WARN: BusLoop RAM restore failed\n");
    }
    /* Belt-and-suspenders: mark default alarm pool initialized (offset +16). */
    mem_write32(USB_GUEST_ALARM_POOL_RAM + 16u, USB_GUEST_SPIN_LOCK_HW + 2u);
    a2bus_ensure_malloc_mutex();
    fprintf(stderr, "[A2Bus] launching BusLoop core1 after radio ready (%s)\n", why);
    fflush(stderr);
    sio_force_core1_launch(0x20000120u, 0x20081000u, 0);
}

static int a2bus_wifi_hooks(void) {
    if (!a2bus_bridge_active() || !cyw43.enabled) {
        return 0;
    }
    a2bus_ensure_malloc_mutex();
    a2bus_ensure_busloop_core1();
    uint32_t pc = cpu.r[15] & ~1u;

    /* Track firmware InitRTC so host DoGetTimeString matches guest calendar. */
    if (pc == USB_GUEST_INIT_RTC) {
        a2bus_apply_init_rtc((time_t)cpu.r[0], (int32_t)cpu.r[1]);
        return 0; /* let guest InitRTC run */
    }

    /* Do not accelerate sleep_ms here — that burned DoTestWifi's 90s wait before
     * radio init finished. Guest timer advances with normal core stepping. */

    /* Under a2bus, USB guest hooks are off (no -usb-console); keep alarm pool
     * usable for lwIP timeouts / NETPUMP / TestWifi. */
    if (pc == USB_GUEST_ALARM_POOL_DEFAULT) {
        mem_write32(USB_GUEST_ALARM_POOL_RAM + 16u, USB_GUEST_SPIN_LOCK_HW + 2u);
        cpu.r[0] = USB_GUEST_ALARM_POOL_RAM;
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    /*
     * wifi_connect_timeout_ms polls with cyw43_arch_wait_for_work_until.
     * Under a2bus that WFIs on wall-clock TIMER and can burn the 15s guest
     * deadline before JOIN events + DHCP ACK are processed. Return immediately
     * so the poll loop can drain CYW43 RX (connect events + fake DHCP).
     * Lease still comes from guest dhcp_start → Bramble fake DHCP → lwIP netif.
     */
    if (pc == 0x1001afd4u) { /* cyw43_arch_wait_for_work_until */
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    /* Guest lwIP DHCP_OPTIONS_LEN=68; long hostname fills the buffer. */
    if (pc == 0x100185f0u) { /* dhcp_option_hostname */
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    /* Host DNS — guest UDP TX pbufs are unreliable; resolve on host (ERR_OK). */
    if (pc == 0x100131b0u || pc == 0x1001322cu) { /* dns_gethostbyname[_addrtype] */
        uint32_t host_p = cpu.r[0];
        uint32_t addr_p = cpu.r[1];
        char host[256];
        uint32_t i;
        if (host_p == 0u || addr_p == 0u) {
            cpu.r[0] = 0xfffffff0u; /* ERR_ARG */
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
            return 1;
        }
        for (i = 0; i < sizeof(host) - 1u; i++) {
            uint8_t c = mem_read8(host_p + i);
            host[i] = (char)c;
            if (c == 0) {
                break;
            }
        }
        host[sizeof(host) - 1u] = '\0';
#if !defined(_WIN32)
        {
            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(host, NULL, &hints, &res) == 0 && res != NULL) {
                struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
                uint32_t ip = (uint32_t)sin->sin_addr.s_addr;
                mem_write32(addr_p, ip);
                freeaddrinfo(res);
                fprintf(stderr, "[A2Bus] host DNS '%s' → %u.%u.%u.%u\n", host,
                        (unsigned)(ip & 0xffu), (unsigned)((ip >> 8) & 0xffu),
                        (unsigned)((ip >> 16) & 0xffu), (unsigned)((ip >> 24) & 0xffu));
                cpu.r[0] = 0; /* ERR_OK */
                cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
                return 1;
            }
            fprintf(stderr, "[A2Bus] host DNS failed for '%s'\n", host);
        }
#endif
        cpu.r[0] = 0xfffffff0u; /* ERR_ARG */
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    /* Host NTP — guest UDP TX still broken after DNS; SNTP on host socket. */
    if (pc == A2BUS_SEND_NTP_REQUEST) {
        uint32_t task = cpu.r[0];
        uint8_t resolved = mem_read8(task + 0x1cu); /* serverIpResolved */
        if (!resolved) {
            return 0; /* let guest assert path run */
        }
        fprintf(stderr, "[A2Bus] Sending NTP Request (host SNTP)\n");
        a2bus_complete_ntp_on_host(task);
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    if (pc == USB_GUEST_MUTEX_ENTER_VENEER || pc == USB_GUEST_MUTEX_EXIT_VENEER ||
        pc == USB_GUEST_MUTEX_ENTER_V || pc == USB_GUEST_MUTEX_EXIT_V) {
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    if (a2bus_stub_cyw43()) {
        if (pc == USB_GUEST_CYW43_ARCH_INIT) {
            static int once;
            if (!once++) {
                fprintf(stderr, "[A2Bus] stub cyw43_arch_init (BRAMBLE_A2BUS_STUB_WIFI)\n");
            }
            mem_write8(0x200615fcu, 1u);
            cpu.r[0] = 0;
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
            return 1;
        }
        if (pc == USB_GUEST_CYW43_GPIO_PUT) {
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
            return 1;
        }
        if (pc == USB_GUEST_INIT_CYW43) {
            mem_write8(0x200615fcu, 1u);
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
            return 1;
        }
        if (pc == USB_GUEST_CONNECT_WIFI) {
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
            return 1;
        }
    }

    /* Empty-SSID only: C++ EH hang. Configured SSID → native DoTestWifi. */
    if (pc == USB_GUEST_DO_TEST_WIFI || pc == 0x20004548u) {
        if (mem_read8(USB_GUEST_WIFI_SSID) == 0u) {
            fprintf(stderr, "[A2Bus] DoTestWifi: SSID not set → NETERR_SSIDNOTSET\n");
            fflush(stderr);
            mem_write8(USB_GUEST_PARAMETER_BUFFER, (uint8_t)USB_GUEST_NETERR_SSIDNOTSET);
            mem_write8(USB_GUEST_DATA_XFER_MODE, 0);
            mem_write32(USB_GUEST_DATA_BUFFER_INDEX, 0);
            mem_write32(USB_GUEST_PARAM_BUFFER_INDEX, 0);
            mem_write8(USB_GUEST_REGISTERS + 1u, (uint8_t)USB_GUEST_NETERR_SSIDNOTSET);
            mem_write8(USB_GUEST_REGISTERS + 2u, mem_read8(USB_GUEST_DATA_BUFFER));
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
            return 1;
        }
        return 0; /* real DoTestWifi: IPC to core0, FormatIPAddr into dataBuffer */
    }

    /* Empty-SSID GetNetworkTime / TestWifi (IPC) — fail fast (C++ EH hang). */
    if (pc != USB_GUEST_GET_NETWORK_TIME && pc != USB_GUEST_TEST_WIFI &&
        pc != 0x200044b0u) {
        return 0;
    }
    if (mem_read8(USB_GUEST_WIFI_SSID) != 0u) {
        return 0;
    }

    if (pc == USB_GUEST_TEST_WIFI) {
        uint32_t result = cpu.r[0];
        fprintf(stderr, "TestWifi()\nSSID not set\n");
        fflush(stderr);
        if (result != 0u) {
            mem_write32(result + 16u, USB_GUEST_NETERR_SSIDNOTSET);
            mem_write8(result + 20u, 1u);
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    fprintf(stderr, "GetNetworkTime()\nSSID not set\n");
    fflush(stderr);
    cpu.r[0] = USB_GUEST_NETERR_SSIDNOTSET;
    cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
    return 1;
}

/*
 * MAME a2bus: stub flash bring-up + block I/O only. Do not enable the full
 * USB-console hook set (InitSpi/multicore skips HardFault early boot).
 */
static int a2bus_spi_flash_hooks(void) {
    if (!a2bus_bridge_active()) {
        return 0;
    }
    a2bus_ensure_busloop_core1();
    uint32_t pc = cpu.r[15] & ~1u;

    static int hw_claim_ready;
    if (!hw_claim_ready++) {
        usb_guest_hw_claim_bootstrap();
    }

    if (pc == USB_GUEST_HW_CLAIM_LOCK) {
        mem_write8(USB_GUEST_HW_CLAIM_LOCK_BYTE, 0);
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_HW_CLAIM_OR_ASSERT) {
        uint32_t base = cpu.r[0];
        uint32_t bit = cpu.r[1];
        uint32_t byte = bit >> 3;
        uint8_t mask = (uint8_t)(1u << (bit & 7u));
        uint8_t v = mem_read8(base + byte);
        mem_write8(base + byte, v | mask);
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_HW_CLAIM_UNUSED) {
        uint32_t base = cpu.r[0];
        uint32_t start = cpu.r[2];
        uint32_t end = cpu.r[3];
        if (start <= end) {
            uint32_t byte = start >> 3;
            uint8_t mask = (uint8_t)(1u << (start & 7u));
            uint8_t v = mem_read8(base + byte);
            mem_write8(base + byte, v | mask);
            cpu.r[0] = start;
        } else {
            cpu.r[0] = 0xffffffffu;
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_HW_CLAIM_CLEAR_FAIL) {
        cpu.r[15] = 0x1000af84u | 1u;
        return 1;
    }

    /*
     * main calls U2_Init before LoadAllConfigs. Under a2bus, U2_Net_Init can
     * stall core0 forever while script-launched core1 already serves the bus —
     * configBuffer stays BSS-zero → CP Unexpected Error:0. Skip U2 like USB mode.
     */
    if (pc == USB_GUEST_U2_INIT || pc == USB_GUEST_U2_INIT_CALL) {
        static int u2_logged;
        if (!u2_logged++) {
            fprintf(stderr, "[A2Bus] skip U2_Init\n");
        }
        if (pc == USB_GUEST_U2_INIT_CALL) {
            cpu.r[15] = 0x1000032cu | 1u; /* next: bl LoadAllConfigs */
        } else {
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        }
        return 1;
    }

    /* InitSpi can stall core0 before LoadAllConfigs; skip call and function. */
    if (pc == USB_GUEST_INIT_SPI_CALL) {
        static int spi_logged;
        if (!spi_logged++) {
            fprintf(stderr, "[A2Bus] skip InitSpi\n");
        }
        cpu.r[15] = 0x100002e6u | 1u;
        return 1;
    }
    if (pc == 0x1000318cu) { /* InitSpi */
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    if (pc == USB_GUEST_LOAD_ALL_CONFIGS) {
        static int load_logged;
        if (!load_logged++) {
            fprintf(stderr, "[A2Bus] stub LoadAllConfigs\n");
        }
        /* DEFCFGBYTE1 = AUTOBOOT only; do not force ROMDISKFLAG (extra unit). */
        usb_guest_init_default_config();
        /* Optional: seed WiFi creds for join-path bring-up (BRAMBLE_A2BUS_SEED_WIFI=1). */
        if (getenv("BRAMBLE_A2BUS_SEED_WIFI") != NULL) {
            /* InitWifiSettings layout: WPA at +0x1A (33 bytes), SSID at +0x5A (33). */
            const char *ssid = "BrambleNet";
            const char *wpa = "password";
            uint32_t i;
            for (i = 0; i < 33u; i++) {
                mem_write8(USB_GUEST_CONFIG_BUFFER + 0x1au + i, 0);
                mem_write8(USB_GUEST_WIFI_SSID + i, 0);
            }
            for (i = 0; ssid[i] && i < 32u; i++) {
                mem_write8(USB_GUEST_WIFI_SSID + i, (uint8_t)ssid[i]);
            }
            for (i = 0; wpa[i] && i < 32u; i++) {
                mem_write8(USB_GUEST_CONFIG_BUFFER + 0x1au + i, (uint8_t)wpa[i]);
            }
            fprintf(stderr,
                    "[A2Bus] seeded WiFi SSID='%s' WPA_len=%u (BRAMBLE_A2BUS_SEED_WIFI)\n",
                    ssid, (unsigned)strlen(wpa));
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    /* Script already launched core1; firmware multicore_launch would stall. */
    if (pc == USB_GUEST_MULTICORE_LAUNCH) {
        static int mc_logged;
        if (!mc_logged++) {
            fprintf(stderr, "[A2Bus] skip multicore_launch_core1\n");
        }
        cpu.r[15] = 0x1000034cu | 1u; /* skip SaveConfigs too */
        return 1;
    }
    if (pc == USB_GUEST_CLOCK_GET_HZ) {
        cpu.r[0] = 150000000u;
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_SPI_GET_BAUDRATE) {
        cpu.r[0] = USB_GUEST_SPI_BAUDRATE_HZ;
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_CHECK_ALLOC) {
        /* Skip panic on HeapLimit; newlib sbrk under emu can sit slightly past
         * 0x20080000. core1 SP is raised to StackOneTop to keep a cushion. */
        usb_guest_return_to_lr(cpu.r[0]);
        return 1;
    }
    /*
     * Host-format guest printf. Never enter newlib _vfprintf_r / _svfprintf_r
     * under a2bus: dual-core smash turns mbtowc into 0x30034280 → HardFault, or
     * _svfprintf_r spins (CMD BUSY timeout @0x1002DE12) → CP clock line junk.
     */
    if (pc == USB_GUEST_VFPRINTF_R ||
        pc == USB_GUEST_SVFPRINTF_R ||
        pc == USB_GUEST_SVFIPRINTF_R) {
        usb_guest_return_to_lr(0);
        return 1;
    }
    if (pc == USB_GUEST_WRAP_VPRINTF) {
        usb_guest_set_vprintf_tx(usb_guest_a2bus_tx_byte);
        int n = usb_guest_host_vprintf(cpu.r[0],
                                       usb_guest_uart_make_ap(cpu.r[13]));
        fflush(stderr);
        usb_guest_return_to_lr((uint32_t)n);
        return 1;
    }
    if (pc == USB_GUEST_WRAP_PRINTF) {
        static int printf_logged;
        if (!printf_logged++) {
            fprintf(stderr, "[A2Bus] host __wrap_printf active\n");
        }
        usb_guest_set_vprintf_tx(usb_guest_a2bus_tx_byte);
        int n = usb_guest_host_vprintf(cpu.r[0],
                                       usb_guest_uart_make_ap(cpu.r[13]));
        fflush(stderr);
        usb_guest_return_to_lr((uint32_t)n);
        return 1;
    }
    if (pc == USB_GUEST_ASCII_MBTOWC || pc == (USB_GUEST_ASCII_MBTOWC | 0x20000000u)) {
        int ret = 0;
        if (cpu.r[1] != 0u && cpu.r[2] != 0u) {
            mem_write32(cpu.r[1], (uint32_t)mem_read8(cpu.r[2]));
            ret = 1;
        }
        {
            uint32_t loc = 0x20004f00u + 0x1ccu;
            if ((mem_read32(loc) & ~1u) == (USB_GUEST_ASCII_MBTOWC | 0x20000000u)) {
                mem_write32(loc, USB_GUEST_ASCII_MBTOWC | 1u);
            }
        }
        usb_guest_return_to_lr((uint32_t)ret);
        return 1;
    }
    if (pc == USB_GUEST_WRAP_PUTS) {
        uint32_t str = cpu.r[0];
        for (uint32_t i = 0; i < 8192u; i++) {
            uint8_t ch = mem_read8(str + i);
            if (ch == 0) {
                break;
            }
            fputc((int)ch, stderr);
        }
        fputc('\n', stderr);
        fflush(stderr);
        cpu.r[0] = 0;
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    /* Soft-continue lwIP TX ref assert only when pbuf is non-NULL; NULL p
     * means alloc failed — returning ERR_BUF without parking is fine. */
    if (pc == USB_GUEST_PANIC) {
        uint32_t msg = cpu.r[0];
        uint32_t lr = cpu.r[14] & ~1u;
        if (lr == 0x1001ab10u || lr == 0x1001ab16u) {
            char buf[40];
            uint32_t i;
            for (i = 0; i < sizeof(buf) - 1u && msg; i++) {
                uint8_t ch = mem_read8(msg + i);
                if (!ch) {
                    break;
                }
                buf[i] = (char)ch;
            }
            buf[i] = '\0';
            if (strncmp(buf, "p->ref == 1", 11) == 0 ||
                strncmp(buf, "check that first pbuf", 21) == 0) {
                static int soft;
                if (soft < 5) {
                    soft++;
                    fprintf(stderr,
                            "[A2Bus] soft-continue lwIP TX assert '%s'\n", buf);
                }
                cpu.r[0] = 0xffffffffu; /* ERR_BUF */
                cpu.r[15] = 0x1001ab04u | 1u;
                return 1;
            }
        }
        if (lr == 0x10013a70u || lr == 0x10013956u || lr == 0x10013a76u) {
            /* pbuf_free / pbuf_add_header null or ref asserts — soft continue */
            static int nullp;
            if (nullp < 8) {
                nullp++;
                fprintf(stderr, "[A2Bus] soft-continue pbuf assert lr=0x%08X\n", lr);
            }
            cpu.r[0] = 0;
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
            return 1;
        }
        static int panic_logged;
        if (!panic_logged++) {
            fprintf(stderr, "[A2Bus] guest panic: ");
            if (msg) {
                for (uint32_t i = 0; i < 200u; i++) {
                    uint8_t ch = mem_read8(msg + i);
                    if (ch == 0) {
                        break;
                    }
                    fputc((int)ch, stderr);
                }
            }
            fprintf(stderr, " (sio_core=%u lr=0x%08X)\n",
                    (unsigned)sio_get_core_id(), cpu.r[14]);
        }
        cpu.r[15] = USB_GUEST_PANIC | 1u;
        return 1;
    }
    /* Belt-and-suspenders: core0 may still be stuck pre-main while core1
     * serves GETUSERSETTINGS; seed defaults then run real GetUserSettings. */
    if (pc == 0x10005524u) { /* GetUserSettings */
        if (mem_read8(USB_GUEST_CONFIG_BUFFER + 6u) != 1u) {
            usb_guest_init_default_config();
            fprintf(stderr, "[A2Bus] GetUserSettings: late-seeded configBuffer\n");
        }
        return 0; /* continue into real function */
    }
    if (pc == USB_GUEST_SAVE_USER_SETTINGS) {
        usb_guest_stub_save_user_settings();
        return 1;
    }
    /*
     * With Bramble -wifi, MegaFlash must see Pico W so core0 enters core0Loop
     * (NTP / TestWifi IPC / cyw43_arch_poll). Force when CYW43 emulation is on.
     */
    /* MAME bridge ⇒ Apple is present; needed so main enters core0Loop (NTP). */
    if (pc == USB_GUEST_IS_APPLE_CONNECTED_FN ||
        pc == USB_GUEST_IS_APPLE_CONNECTED_CALL ||
        pc == USB_GUEST_IS_APPLE_CONNECTED_CALL2) {
        static int apple_logged;
        if (!apple_logged++) {
            fprintf(stderr, "[A2Bus] IsAppleConnected forced true (a2bus-bridge)\n");
        }
        cpu.r[0] = 1u;
        if (pc == USB_GUEST_IS_APPLE_CONNECTED_FN) {
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        } else if (pc == USB_GUEST_IS_APPLE_CONNECTED_CALL) {
            cpu.r[15] = 0x1000033cu | 1u; /* next insn after bl */
        } else {
            cpu.r[15] = 0x1000040cu | 1u;
        }
        return 1;
    }
    if (cyw43.enabled && pc == USB_GUEST_CHECK_PICOW_FN) {
        static int picow_logged;
        if (!picow_logged++) {
            fprintf(stderr, "[A2Bus] CheckPicoW forced true (-wifi)\n");
        }
        cpu.r[0] = 1u;
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    /*
     * stdio_usb_init blocks forever without a USB host. Skip the call but land
     * on bl InitPicoLed (0x100003ca / 0x1000048a) — NOT the following insn.
     * Jumping to 0x100003ce skipped cyw43_arch_init; TestWifi then asserted in
     * cyw43_ensure_up (cyw43_is_initialized false).
     */
    if (pc == USB_GUEST_STDIO_USB_INIT_CALL1) {
        static int usb_skip_logged;
        if (!usb_skip_logged++) {
            fprintf(stderr, "[A2Bus] skip stdio_usb_init → InitPicoLed/cyw43\n");
        }
        cpu.r[15] = USB_GUEST_INIT_PICOLED_CALL1 | 1u; /* bl InitPicoLed */
        return 1;
    }
    if (pc == USB_GUEST_STDIO_USB_INIT_CALL2) {
        cpu.r[15] = USB_GUEST_INIT_PICOLED_CALL2 | 1u; /* bl InitPicoLed */
        return 1;
    }
    if (pc == USB_GUEST_STDIO_USB_INIT_FN) {
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    /* Security-register SPI program hangs under a2bus; mirror full configBuffer. */
    if (pc == USB_GUEST_ENCRYPT_WRITE_CFG) {
        usb_guest_persist_config_to_host();
        mem_write8(USB_GUEST_SETTINGS_NOT_FLASH, 0u);
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_TS_WRITE_SEC_REG) {
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_TS_READ_SEC_REG) {
        /* Leave dest unchanged / zeros; callers validate magic. */
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    /*
     * Open-Apple at CP entry → CMD_GETINFOSTR → GetDeviceInfoString.
     * Native sprintf(%f) hangs under Bramble and leaves STATUS BUSY, so the
     * next ID check / menu action reports MegaFlash missing. Host-complete
     * the whole DoGetInfoString (and the leaf) so BusLoop stays idle.
     */
    if (pc == 0x10001e64u /* DoGetInfoString */ ||
        pc == 0x20004690u /* __DoGetInfoString_veneer */) {
        uint8_t type = mem_read8(USB_GUEST_PARAMETER_BUFFER);
        if (type == 0u) { /* INFOSTR_DEVICE */
            usb_guest_fill_device_info_string(USB_GUEST_DATA_BUFFER);
            mem_write8(USB_GUEST_DATA_XFER_MODE, 0); /* MODE_LINEAR */
            mem_write32(USB_GUEST_DATA_BUFFER_INDEX, 0);
            mem_write32(USB_GUEST_PARAM_BUFFER_INDEX, 0);
            mem_write8(USB_GUEST_REGISTERS + 2u, mem_read8(USB_GUEST_DATA_BUFFER));
            mem_write8(USB_GUEST_REGISTERS + 1u, mem_read8(USB_GUEST_PARAMETER_BUFFER));
            /* ClearError: drop ERRORFLAG + error code field; leave BUSY to DoCommand. */
            uint8_t st = mem_read8(USB_GUEST_REGISTERS);
            mem_write8(USB_GUEST_REGISTERS, (uint8_t)(st & (uint8_t)~0x5Fu));
            fprintf(stderr, "[A2Bus] DoGetInfoString host-filled (Open-Apple device info)\n");
            fflush(stderr);
        } else {
            uint8_t st = mem_read8(USB_GUEST_REGISTERS);
            mem_write8(USB_GUEST_REGISTERS, (uint8_t)((st & (uint8_t)~0x1Fu) | 0x40u | 0x0Au));
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_GET_DEVICE_INFO) { /* GetDeviceInfoString leaf */
        uint32_t dest = cpu.r[0];
        if (dest == 0u) {
            dest = USB_GUEST_DATA_BUFFER;
        }
        usb_guest_fill_device_info_string(dest);
        fprintf(stderr, "[A2Bus] GetDeviceInfoString host-filled\n");
        fflush(stderr);
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_DO_GET_TIME_STR || pc == USB_GUEST_DO_GET_TIME_STR_V) {
        a2bus_host_do_get_time_string();
        static int gettime_logged;
        if (!gettime_logged++) {
            fprintf(stderr, "[A2Bus] DoGetTimeString host-complete\n");
            fflush(stderr);
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_AON_TIMER_GET_CAL) {
        time_t now = a2bus_rtc_now_local();
        /* Match InitRTC: wall time is stored as timezone-adjusted fake UTC. */
        struct tm *t = gmtime(&now);
        if (t) {
            a2bus_write_tm(cpu.r[0], t);
        }
        if (a2bus_rtc_valid && !mem_read8(USB_GUEST_RTC_RUNNING_BSS)) {
            mem_write8(USB_GUEST_RTC_RUNNING_BSS, 1u);
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_GET_CONFIG_BYTE1) {
        usb_guest_stub_get_config_byte1();
        return 1;
    }
    if (pc == USB_GUEST_GET_CONFIG_BYTE2) {
        usb_guest_stub_get_config_byte2();
        return 1;
    }
    if (pc == USB_GUEST_INIT_FLASH) {
        usb_guest_init_flash_stub();
        /* init_flash_stub sets LR return; rebuild mapping + mappingEnabled. */
        usb_guest_setup_flash_unit_mapping_stub();
        return 1;
    }
    if (pc == USB_GUEST_SETUP_FLASH_MAP) {
        usb_guest_setup_flash_unit_mapping_stub();
        return 1;
    }
    if (pc == USB_GUEST_GET_TOTAL_UNIT_COUNT ||
        pc == USB_GUEST_GET_TOTAL_UNIT_COUNT_V) {
        usb_guest_stub_get_total_unit_count();
        return 1;
    }
    if (pc == USB_GUEST_IS_VALID_UNIT_NUM ||
        pc == USB_GUEST_IS_VALID_UNIT_NUM_V) {
        usb_guest_stub_is_valid_unit_num();
        return 1;
    }
    if (pc == USB_GUEST_GET_BLOCK_COUNT ||
        pc == USB_GUEST_GET_BLOCK_COUNT_V) {
        usb_guest_stub_get_block_count();
        return 1;
    }
    if (pc == USB_GUEST_GET_UNIT_COUNT_FLASH_EN ||
        pc == USB_GUEST_GET_UNIT_COUNT_FLASH_EN_V ||
        pc == USB_GUEST_GET_UNIT_COUNT_FLASH_ACT) {
        cpu.r[0] = usb_guest_flash_unit_total();
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_GET_VOLUME_INFO) {
        usb_guest_stub_get_volume_info();
        return 1;
    }
    if (pc == USB_GUEST_READ_BLOCK_VENEER || pc == USB_GUEST_READ_BLOCK) {
        usb_guest_stub_read_block();
        return 1;
    }
    if (pc == USB_GUEST_WRITE_BLOCK_VENEER || pc == USB_GUEST_WRITE_BLOCK) {
        usb_guest_stub_write_block();
        return 1;
    }
    if (pc == USB_GUEST_COPY_MEMORY_ALIGNED ||
        pc == USB_GUEST_COPY_MEMORY_ALIGNED_V) {
        /* CMD_LOAD_CPANEL copies cpanel pages via DMA; stub host memcpy. */
        usb_guest_stub_copy_memory_entry();
        return 1;
    }
    /*
     * Host-complete LOAD_CPANEL / GETDEVINFO so BusLoop never stalls on DMA or
     * flash walks. Option 7 and Ctrl-Reset chkmegaflashex depend on these.
     */
    if (pc == 0x10001c84u /* DoLoadCPanel */ ||
        pc == 0x20004448u /* __DoLoadCPanel_veneer */) {
        const uint32_t cpanel = 0x1006f230u;
        uint32_t cpanel_len = mem_read32(0x10072bd0u);
        uint32_t page_count = (cpanel_len + 255u) / 256u;
        uint32_t page = mem_read8(USB_GUEST_PARAMETER_BUFFER);
        if (page < page_count) {
            uint32_t src = cpanel + page * 256u;
            for (uint32_t i = 0; i < 256u; i++) {
                mem_write8(USB_GUEST_DATA_BUFFER + i, mem_read8(src + i));
            }
            uint8_t st = mem_read8(USB_GUEST_REGISTERS);
            mem_write8(USB_GUEST_REGISTERS, (uint8_t)(st & (uint8_t)~0x5Fu));
        } else {
            uint8_t st = mem_read8(USB_GUEST_REGISTERS);
            mem_write8(USB_GUEST_REGISTERS,
                       (uint8_t)((st & (uint8_t)~0x1Fu) | 0x40u | 0x08u));
        }
        mem_write8(USB_GUEST_DATA_XFER_MODE, 0);
        mem_write32(USB_GUEST_DATA_BUFFER_INDEX, 0);
        mem_write32(USB_GUEST_PARAM_BUFFER_INDEX, 0);
        mem_write8(USB_GUEST_REGISTERS + 2u, mem_read8(USB_GUEST_DATA_BUFFER));
        mem_write8(USB_GUEST_REGISTERS + 1u, mem_read8(USB_GUEST_PARAMETER_BUFFER));
        static int loadcp_logged;
        if (!loadcp_logged++) {
            fprintf(stderr, "[A2Bus] DoLoadCPanel host-complete\n");
            fflush(stderr);
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == 0x10001ed0u /* DoGetDeviceInfo */ ||
        pc == 0x20004420u /* __DoGetDeviceInfo_veneer */) {
        uint32_t i;
        for (i = 0; i < 32u; i++) {
            mem_write8(USB_GUEST_PARAMETER_BUFFER + i, 0);
        }
        mem_write8(USB_GUEST_PARAMETER_BUFFER + 0u, 0x88u); /* SIGNATURE1 */
        mem_write8(USB_GUEST_PARAMETER_BUFFER + 1u, 0x74u); /* SIGNATURE2 */
        mem_write8(USB_GUEST_PARAMETER_BUFFER + 2u, 0x01u); /* DeviceInfoVer */
        mem_write8(USB_GUEST_PARAMETER_BUFFER + 3u, 35u);   /* FIRMWAREVER lo */
        mem_write8(USB_GUEST_PARAMETER_BUFFER + 4u, 0u);
        mem_write8(USB_GUEST_PARAMETER_BUFFER + 5u, 0x81u); /* BRD_PICO2W */
        mem_write8(USB_GUEST_PARAMETER_BUFFER + 6u, 0u);
        {
            uint32_t units = usb_guest_flash_unit_total();
            mem_write8(USB_GUEST_PARAMETER_BUFFER + 7u, (uint8_t)units);
            mem_write8(USB_GUEST_PARAMETER_BUFFER + 8u, (uint8_t)units);
        }
        {
            unsigned mb = (unsigned)(mem_read32(USB_GUEST_FLASH_SIZE0) +
                                     mem_read32(USB_GUEST_FLASH_SIZE1));
            if (mb == 0u) {
                mb = 128u;
            }
            mem_write8(USB_GUEST_PARAMETER_BUFFER + 9u, (uint8_t)(mb & 0xffu));
            mem_write8(USB_GUEST_PARAMETER_BUFFER + 10u, (uint8_t)((mb >> 8) & 0xffu));
        }
        mem_write32(USB_GUEST_PARAM_BUFFER_INDEX, 0);
        mem_write32(USB_GUEST_DATA_BUFFER_INDEX, 0);
        mem_write8(USB_GUEST_REGISTERS + 1u, mem_read8(USB_GUEST_PARAMETER_BUFFER));
        {
            uint8_t st = mem_read8(USB_GUEST_REGISTERS);
            mem_write8(USB_GUEST_REGISTERS, (uint8_t)(st & (uint8_t)~0x5Fu));
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_TS_READ_JEDECID) {
        cpu.r[0] = USB_GUEST_WINBOND_JEDEC24;
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_SET_FLASH_DRIVE_STR || pc == USB_GUEST_ENABLE_4BYTE_ADDR) {
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    return 0;
}


int bramble_ext_guest_hook(void)
{
    if (!a2bus_bridge_active()) {
        return 0;
    }
    /* WiFi stubs before veneer so DoTestWifi hits SRAM veneer path too. */
    if (a2bus_wifi_hooks()) {
        return 1;
    }
    if (a2bus_fix_picoram_veneer()) {
        return 1;
    }
    if (a2bus_spi_flash_hooks()) {
        return 1;
    }
    return 0;
}
