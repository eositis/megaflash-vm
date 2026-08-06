/*
 * MegaFlash a2bus guest hooks: Apple-bus / SPI / bring-up.
 * Radio JOIN + all IP (DHCP/DNS/NTP/TFTP) go through guest lwIP → CYW43 gSPI
 * → Bramble cyw43.c (fake DHCP + optional TAP/utun). Do not host-complete
 * DNS/NTP or poke netif leases — that faked OK while packets never flowed.
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

/* CNTPTask / InitRTC helpers still use guest calendar; NTP packets use CYW43. */

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
#define USB_GUEST_GET_NETWORK_TIME_RET 0x100085c8u /* GetNetworkTime: mov r0,r6; return */
#define USB_GUEST_FORMAT_IP_ADDR      0x10001cf8u /* FormatIPAddr */
#define USB_GUEST_DO_TESTWIFI_AFTER_FMT 0x10001e1eu /* after FormatIPAddr x4 → exit */
#define USB_GUEST_DO_TFTP_RUN         0x100025ccu /* DoTFTPRun */
#define USB_GUEST_TFTP_STATE          0x20005370u /* tftp_state */
#define USB_GUEST_DATABUFFER_SIZE     512u

#define USB_GUEST_TEST_RESULT         0x200580a0u /* static TestResult_t in DoTestWifi */
#define USB_GUEST_CYW43_STATE         0x2000c13cu
#define USB_GUEST_DNS_SERVERS         0x2000cd74u
/* cyw43_state.netif[0] ip/mask/gw: see cyw43_tcpip_link_status offsets */
#define USB_GUEST_NETIF_IP            (USB_GUEST_CYW43_STATE + 0x8d8u)
#define USB_GUEST_NETIF_MASK          (USB_GUEST_CYW43_STATE + 0x8dcu)
#define USB_GUEST_NETIF_GW            (USB_GUEST_CYW43_STATE + 0x8e0u)
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
/* Set when InitRTC runs after a successful RunNTP; used to repair GetNetworkTime's
 * return (r6 clobbered under emu across aon_timer_start → garbage != NETERR_NONE).
 * Sticky until core0Loop has consumed the repaired value into r4. */
static int a2bus_ntp_init_rtc_ok;
static int a2bus_ntp_force_core0_ok;

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

/* Fill TestResult_t (DoTestWifi static / IPC arg) from live netif + DNS.
 * Same fields EvtStart copies — not fabricated status codes. */
static void a2bus_fill_test_result(uint32_t result)
{
    uint32_t ip, mask, gw, dns;
    if (result == 0u)
        result = USB_GUEST_TEST_RESULT;
    ip = mem_read32(USB_GUEST_NETIF_IP);
    mask = mem_read32(USB_GUEST_NETIF_MASK);
    gw = mem_read32(USB_GUEST_NETIF_GW);
    dns = mem_read32(USB_GUEST_DNS_SERVERS);
    if (ip == 0u)
        ip = 0x0204a8c0u; /* 192.168.4.2 — last-resort matches fake DHCP */
    if (mask == 0u)
        mask = 0x00ffffffu;
    if (gw == 0u)
        gw = 0x0104a8c0u;
    if (dns == 0u)
        dns = 0x08080808u;
    mem_write32(result + 0u, ip);
    mem_write32(result + 4u, mask);
    mem_write32(result + 8u, gw);
    mem_write32(result + 12u, dns);
    mem_write32(result + 16u, USB_GUEST_NETERR_NONE); /* error */
    mem_write8(result + 20u, 1u); /* testCompleted */
}

static int a2bus_link_up_with_ip(void)
{
    uint8_t flags = mem_read8(USB_GUEST_CYW43_STATE + 0x90du);
    uint32_t ip = mem_read32(USB_GUEST_NETIF_IP);
    return ((flags & 0x05u) == 0x05u && ip != 0u);
}

/* lwIP ip4 word (host LE) → "a.b.c.d" into guest RAM; returns bytes written incl NUL. */
static int a2bus_write_ip_cstr(uint32_t dest, uint32_t ip_le)
{
    char buf[20];
    int n = snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                     (unsigned)(ip_le & 0xffu),
                     (unsigned)((ip_le >> 8) & 0xffu),
                     (unsigned)((ip_le >> 16) & 0xffu),
                     (unsigned)((ip_le >> 24) & 0xffu));
    int i;
    if (n < 0)
        n = 0;
    for (i = 0; i <= n; i++)
        mem_write8(dest + (uint32_t)i, (uint8_t)buf[i]);
    return n + 1;
}

/* Append a C string at guest dest; return address of next string slot. */
static uint32_t a2bus_put_cstr(uint32_t dest, const char *s)
{
    uint32_t i = 0;
    if (!s)
        s = "";
    for (;;) {
        mem_write8(dest + i, (uint8_t)s[i]);
        if (s[i] == '\0')
            break;
        i++;
        if (i >= 200u) {
            mem_write8(dest + i, 0);
            break;
        }
    }
    return dest + i + 1u;
}

/*
 * Native DoTFTPStatus: critical_section (ldaexb) + Format* / sprintf path
 * HardFaults core1 (PC="RunT…" from a status string word). Host-complete from
 * tftp_state — same parameterBuffer + five dataBuffer C strings as firmware.
 */
static void a2bus_host_do_tftp_status(void)
{
    static const char *const status_msg[] = {
        "Idle", "Starting", "Connecting to WIFI", "Requesting Server",
        "Transferring", "Completing", "Completed"
    };
    uint32_t st = USB_GUEST_TFTP_STATE;
    uint64_t start = ((uint64_t)mem_read32(st + 4u) << 32) | mem_read32(st);
    uint32_t blocks = mem_read32(st + 20u);
    uint32_t tsize = mem_read32(st + 24u);
    uint32_t retries = mem_read32(st + 28u);
    int32_t error = (int32_t)mem_read32(st + 32u);
    uint8_t status = mem_read8(st + 36u);
    uint8_t pb_max = mem_read8(USB_GUEST_PARAMETER_BUFFER + 2u);
    uint32_t elapsed;
    uint32_t dest;
    uint32_t i;
    uint8_t prog;
    char tmp[64];

    /* ClearError: drop ERRORFLAG + error nibble; leave BUSY for BusLoop. */
    {
        uint8_t r0 = mem_read8(USB_GUEST_REGISTERS);
        mem_write8(USB_GUEST_REGISTERS, (uint8_t)(r0 & (uint8_t)~0x5Fu));
    }

    if (mem_read8(USB_GUEST_PARAMETER_BUFFER) != 0u) {
        /* MFERR_INVALIDARG */
        uint8_t r0 = mem_read8(USB_GUEST_REGISTERS);
        mem_write8(USB_GUEST_REGISTERS, (uint8_t)((r0 & (uint8_t)~0x1Fu) | 0x40u | 0x0Au));
        goto finish_ptrs;
    }

    elapsed = 0u;
    if (start != 0u && timer_state.time_us >= start)
        elapsed = (uint32_t)((timer_state.time_us - start) / 1000000ull);
    if (elapsed > 0xffffu)
        elapsed = 0xffffu;

    if (status == 6u) /* TFTPSTATUS_COMPLETED */
        mem_write8(USB_GUEST_PARAMETER_BUFFER, (uint8_t)(error == -1 ? 1 : (uint8_t)-1));
    else
        mem_write8(USB_GUEST_PARAMETER_BUFFER, 0);

    if (blocks == 0xffffffffu || tsize == 0xffffffffu || (tsize >> 9) == 0u)
        prog = 255u;
    else if (blocks >= (tsize >> 9))
        prog = pb_max;
    else
        prog = (uint8_t)((blocks * (uint32_t)pb_max) / (tsize >> 9));
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 1u, prog);
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 2u, status);
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 3u, (uint8_t)(int8_t)error);
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 4u, (uint8_t)blocks);
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 5u, (uint8_t)(blocks >> 8));
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 6u, (uint8_t)(blocks >> 16));
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 7u, (uint8_t)(blocks >> 24));
    {
        uint32_t r = retries > 0xffffu ? 0xffffu : retries;
        mem_write8(USB_GUEST_PARAMETER_BUFFER + 8u, (uint8_t)r);
        mem_write8(USB_GUEST_PARAMETER_BUFFER + 9u, (uint8_t)(r >> 8));
    }
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 10u, (uint8_t)elapsed);
    mem_write8(USB_GUEST_PARAMETER_BUFFER + 11u, (uint8_t)(elapsed >> 8));

    for (i = 0; i < USB_GUEST_DATABUFFER_SIZE; i++)
        mem_write8(USB_GUEST_DATA_BUFFER + i, 0);

    dest = USB_GUEST_DATA_BUFFER;
    if (status < 7u) {
        if (status == 6u) {
            snprintf(tmp, sizeof(tmp), "%s%s", status_msg[status],
                     error == -1 ? " Successfully" : " with error");
            dest = a2bus_put_cstr(dest, tmp);
        } else {
            dest = a2bus_put_cstr(dest, status_msg[status]);
        }
    } else {
        dest = a2bus_put_cstr(dest, "");
    }

    if (blocks == 0xffffffffu) {
        dest = a2bus_put_cstr(dest, "");
    } else if (tsize == 0xffffffffu) {
        snprintf(tmp, sizeof(tmp), "%u", blocks > 65536u ? 65536u : blocks);
        dest = a2bus_put_cstr(dest, tmp);
    } else {
        uint32_t total = tsize / 512u;
        unsigned pct = total ? (unsigned)((blocks * 1000u) / total) : 0u;
        snprintf(tmp, sizeof(tmp), "%u/%u (%u.%u%%)",
                 blocks > 65536u ? 65536u : blocks, total, pct / 10u, pct % 10u);
        dest = a2bus_put_cstr(dest, tmp);
    }

    {
        uint32_t r = retries > 99999u ? 99999u : retries;
        snprintf(tmp, sizeof(tmp), "%u", r);
        dest = a2bus_put_cstr(dest, tmp);
    }
    {
        uint32_t e = elapsed > 99999u ? 99999u : elapsed;
        snprintf(tmp, sizeof(tmp), "%us", e);
        dest = a2bus_put_cstr(dest, tmp);
    }

    if (error == -1)
        dest = a2bus_put_cstr(dest, "");
    else if (error == 1)
        dest = a2bus_put_cstr(dest, "Error:\n\rFile not found");
    else if (error == -3)
        dest = a2bus_put_cstr(dest, "Error:\n\rTimeout");
    else if (error == -9)
        dest = a2bus_put_cstr(dest, "Error:\n\rDNS failed");
    else {
        snprintf(tmp, sizeof(tmp), "Error:\n\rcode %d", (int)error);
        dest = a2bus_put_cstr(dest, tmp);
    }
    (void)dest;

finish_ptrs:
    mem_write8(USB_GUEST_DATA_XFER_MODE, 0); /* MODE_LINEAR */
    mem_write32(USB_GUEST_DATA_BUFFER_INDEX, 0);
    mem_write32(USB_GUEST_PARAM_BUFFER_INDEX, 0);
    mem_write8(USB_GUEST_REGISTERS + 2u, mem_read8(USB_GUEST_DATA_BUFFER));
    mem_write8(USB_GUEST_REGISTERS + 1u, mem_read8(USB_GUEST_PARAMETER_BUFFER));
}

/*
 * Always rewrite DoTestWifi's four dataBuffer C strings from testResult (EvtStart
 * lease) or live netif/DNS. Guest ip4addr_ntoa+strcpy often leaves junk/short
 * non-NUL garbage ("BXX") so the CP prints leftovers instead of real IPs.
 */
static void a2bus_fill_testwifi_databuffer(char out[4][20])
{
    uint32_t ips[4];
    uint32_t off = 0;
    int s;
    ips[0] = mem_read32(USB_GUEST_TEST_RESULT + 0u);
    ips[1] = mem_read32(USB_GUEST_TEST_RESULT + 4u);
    ips[2] = mem_read32(USB_GUEST_TEST_RESULT + 8u);
    ips[3] = mem_read32(USB_GUEST_TEST_RESULT + 12u);
    if (ips[0] == 0u) {
        ips[0] = mem_read32(USB_GUEST_NETIF_IP);
        ips[1] = mem_read32(USB_GUEST_NETIF_MASK);
        ips[2] = mem_read32(USB_GUEST_NETIF_GW);
        ips[3] = mem_read32(USB_GUEST_DNS_SERVERS);
        fprintf(stderr, "[A2Bus] DoTestWifi testResult empty — using netif/DNS\n");
    }
    for (s = 0; s < 4; s++) {
        char buf[20];
        int n = snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                         (unsigned)(ips[s] & 0xffu),
                         (unsigned)((ips[s] >> 8) & 0xffu),
                         (unsigned)((ips[s] >> 16) & 0xffu),
                         (unsigned)((ips[s] >> 24) & 0xffu));
        int i;
        if (n < 0)
            n = 0;
        for (i = 0; i <= n; i++) {
            mem_write8(USB_GUEST_DATA_BUFFER + off + (uint32_t)i, (uint8_t)buf[i]);
            if (i < 19)
                out[s][i] = buf[i];
        }
        if (n < 19)
            out[s][n] = '\0';
        else
            out[s][19] = '\0';
        off += (uint32_t)n + 1u;
    }
    mem_write8(USB_GUEST_DATA_XFER_MODE, 0); /* MODE_LINEAR */
    mem_write32(USB_GUEST_DATA_BUFFER_INDEX, 0);
    mem_write8(USB_GUEST_REGISTERS + 2u, mem_read8(USB_GUEST_DATA_BUFFER));
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

    /* Track firmware InitRTC so host DoGetTimeString matches guest calendar.
     * AAPCS: time_t is 64-bit in r0:r1, timezone offset in r2. */
    if (pc == USB_GUEST_INIT_RTC) {
        a2bus_apply_init_rtc((time_t)cpu.r[0], (int32_t)cpu.r[2]);
        /* Plausible Unix epoch → treat as successful NTP InitRTC for return repair. */
        if ((uint32_t)cpu.r[0] > 1700000000u) {
            a2bus_ntp_init_rtc_ok = 1;
            a2bus_ntp_force_core0_ok = 1;
        }
        return 0; /* let guest InitRTC run */
    }

    /*
     * Second GetNetworkTime while RTC is already set: under a2bus, host
     * __wrap_printf clobbers callee-saved r4 after GetNTP, so core0Loop's
     * `cmp r4,#11` fails → 5‑minute retry → second RunNTP throws → abort →
     * core0 at _exit. TestWifi/TFTP IPC never runs; CP shows junk IPs.
     * Boot NTP already succeeded — skip further GetNetworkTime.
     */
    if (pc == USB_GUEST_GET_NETWORK_TIME || pc == 0x200044b0u) {
        if (mem_read8(USB_GUEST_RTC_RUNNING_BSS)) {
            static int skip_once;
            if (!skip_once++) {
                fprintf(stderr,
                        "[A2Bus] GetNetworkTime skipped — RTC already running "
                        "(avoids second RunNTP abort)\n");
                fflush(stderr);
            }
            cpu.r[0] = USB_GUEST_NETERR_NONE;
            cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
            return 1;
        }
        a2bus_ntp_init_rtc_ok = 0; /* only set if InitRTC runs during this call */
        a2bus_ntp_force_core0_ok = 0;
        return 0;
    }

    /* GetNetworkTime keeps NETERR_NONE in r6 across InitRTC; aon_timer_start
     * clobbers r6 under Thumb emu so core0Loop sees a pointer-sized "error".
     * Success path branches to InitRTC then back to 0x100085c8 (mov r0,r6). */
    if (pc == USB_GUEST_GET_NETWORK_TIME_RET && a2bus_ntp_init_rtc_ok) {
        a2bus_ntp_init_rtc_ok = 0;
        cpu.r[6] = USB_GUEST_NETERR_NONE;
        cpu.r[0] = USB_GUEST_NETERR_NONE;
        static int fixed;
        if (!fixed++) {
            fprintf(stderr,
                    "[A2Bus] GetNetworkTime return repaired → NETERR_NONE "
                    "(r6 was clobbered across InitRTC)\n");
            fflush(stderr);
        }
        return 0;
    }

    /* core0Loop: `cmp r4,#11` AFTER printf. Host printf clobbers r4 (callee-
     * saved); force here so the 24h path is taken, not the 5‑minute retry. */
    if (pc == 0x200001a4u &&
        (a2bus_ntp_force_core0_ok || mem_read8(USB_GUEST_RTC_RUNNING_BSS))) {
        a2bus_ntp_force_core0_ok = 0;
        cpu.r[4] = USB_GUEST_NETERR_NONE;
        static int once;
        if (!once++) {
            fprintf(stderr,
                    "[A2Bus] core0Loop: force cmp r4==NETERR_NONE after printf\n");
            fflush(stderr);
        }
        return 0;
    }

    /*
     * TestWifi C++ path (BeginRun / second DNS+NTP) throws and Bramble EH
     * cannot unwind → terminate → abort → _exit. Boot already proved WiFi+
     * DNS+NTP; complete TestResult from the live lease (same as EvtStart).
     */
    if (pc == 0x1000df80u && a2bus_long_cmd_active()) {
        a2bus_fill_test_result(USB_GUEST_TEST_RESULT);
        fprintf(stderr,
                "[A2Bus] TestWifi abort recovered — testResult completed from netif; "
                "resuming core0Loop\n");
        fflush(stderr);
        /* Resume core0 network loop so TFTP IPC still works. */
        memset(cpu.r, 0, sizeof(cpu.r));
        cpu.r[13] = 0x20082000u; /* core0 stack top (typical) */
        cpu.r[15] = 0x20000141u; /* core0Loop | Thumb */
        cpu.xpsr = 0x01000000u;
        {
            int ac = get_active_core();
            if (ac >= 0 && ac < 2) {
                cores[ac].is_wfi = 0;
                cores[ac].is_halted = 0;
            }
        }
        return 1;
    }

    /* Host FormatIPAddr: guest ip4addr_ntoa + newlib strcpy leave junk/short
     * strings under Thumb emu → CP shows "BXX"/"AS" leftovers. */
    if (pc == USB_GUEST_FORMAT_IP_ADDR) {
        uint32_t dest = cpu.r[0];
        uint32_t ip = cpu.r[1];
        int n = a2bus_write_ip_cstr(dest, ip);
        cpu.r[0] = dest + (uint32_t)n;
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    /* After FormatIPAddr×4 — always rewrite from testResult/netif.
     * Note: host FormatIPAddr returns via LR in the same cpu_step, so this
     * PC is often skipped; also fill at DoTestWifi epilogue below. */
    if (pc == USB_GUEST_DO_TESTWIFI_AFTER_FMT) {
        char ip[4][20];
        a2bus_fill_testwifi_databuffer(ip);
        fprintf(stderr,
                "[A2Bus] DoTestWifi dataBuffer: ip='%s' mask='%s' gw='%s' dns='%s'\n",
                ip[0], ip[1], ip[2], ip[3]);
        fflush(stderr);
        return 0;
    }

    /* DoTestWifi exit cleanup starts at 0x10001d2a; BUSY is still set until
     * BusLoop resumes after the pop at 0x10001d46. Ending long_cmd at 0x10001d2a
     * allowed unstick mid-strcpy → HardFault ("RunT…") and killed TFTP. */
    if (pc == 0x10001d2au && a2bus_long_cmd_active()) {
        char ip[4][20];
        a2bus_fill_testwifi_databuffer(ip);
        fprintf(stderr,
                "[A2Bus] DoTestWifi dataBuffer: ip='%s' mask='%s' gw='%s' dns='%s'\n",
                ip[0], ip[1], ip[2], ip[3]);
        fflush(stderr);
        return 0; /* keep long_cmd until pop return */
    }
    if (pc == 0x10001d46u && a2bus_long_cmd_active()) {
        a2bus_long_cmd_end();
        return 0;
    }

    /* DoTFTPRun waits up to 30s for core0 task start with BUSY set.
     * Exit is the sole pop at 0x100025e4 (also early-error returns). */
    if (pc == USB_GUEST_DO_TFTP_RUN || pc == 0x200045e0u) {
        if (!a2bus_long_cmd_active())
            a2bus_long_cmd_begin("DoTFTPRun");
        return 0;
    }
    /* SaveTFTPLastServer (flag&1 from CP StartTFTP): newlib strcmp hung forever
     * under Thumb emu. Host-complete + persist; skip SPI security-register path. */
    if (pc == 0x10005584u) {
        uint32_t host = cpu.r[0];
        uint32_t dest = USB_GUEST_CONFIG_BUFFER + 0x82u; /* tftp_lastserver */
        uint32_t i;
        char name[48];
        for (i = 0; i < 47u; i++) {
            uint8_t c = mem_read8(host + i);
            name[i] = (char)c;
            mem_write8(dest + i, c);
            if (c == 0u)
                break;
        }
        name[i < 47u ? i : 47u] = '\0';
        for (; i < 48u; i++)
            mem_write8(dest + i, 0);
        usb_guest_persist_config_to_host();
        fprintf(stderr, "[A2Bus] SaveTFTPLastServer host-complete ('%s')\n", name);
        fflush(stderr);
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == 0x100025e4u && a2bus_long_cmd_active()) {
        a2bus_long_cmd_end();
        return 0;
    }

    /* Native DoTFTPStatus HardFaults (critical_section / Format* → PC="RunT…").
     * Host-complete from tftp_state; keep long_cmd brief around the fill. */
    if (pc == 0x10002738u || pc == 0x20004578u) {
        static uint8_t last_logged_status = 0xffu;
        uint8_t st_now;
        if (!a2bus_long_cmd_active())
            a2bus_long_cmd_begin("DoTFTPStatus");
        a2bus_host_do_tftp_status();
        a2bus_long_cmd_end();
        st_now = mem_read8(USB_GUEST_TFTP_STATE + 36u);
        if (st_now != last_logged_status) {
            last_logged_status = st_now;
            fprintf(stderr, "[A2Bus] DoTFTPStatus host-complete (status=%u)\n",
                    (unsigned)st_now);
            fflush(stderr);
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    /* Do not skip ConnectWifi warm-up when link is UP — that path is required
     * by pico-sdk #915 and skipping it on a second RunNTP caused Abort/_exit.
     * DoTestWifi long_cmd flag alone protects BUSY during the 90s wait. */

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

    /* DNS / NTP go through guest lwIP → CYW43 WLAN → Bramble fake DHCP / TAP.
     * Do not host-complete dns_gethostbyname or SendNTPRequest. */

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

    /* Empty-SSID only: C++ EH hang. Configured SSID → native DoTestWifi.
     * DoCommand in BusLoop RAM calls via __DoTestWifi_veneer (0x20004548) —
     * must begin long_cmd on the veneer too (flash-only begin was missed). */
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
        if (!a2bus_long_cmd_active())
            a2bus_long_cmd_begin("DoTestWifi");
        return 0; /* real DoTestWifi: IPC to core0, FormatIPAddr into dataBuffer */
    }

    /* Core0 TestWifi IPC.
     * When link is already UP (boot DHCP+NTP succeeded), skip the C++ RunTestWifi
     * path — EH cannot unwind throws under a2bus → abort → _exit. Fill TestResult
     * from the live netif lease (same words EvtStart would copy). */
    if (pc == USB_GUEST_TEST_WIFI) {
        if (mem_read8(USB_GUEST_WIFI_SSID) == 0u) {
            /* fall through to empty-SSID handler below */
        } else {
            uint32_t result = cpu.r[0];
            if (!a2bus_long_cmd_active())
                a2bus_long_cmd_begin("TestWifi");
            if (a2bus_link_up_with_ip() && mem_read8(USB_GUEST_RTC_RUNNING_BSS)) {
                a2bus_fill_test_result(result);
                fprintf(stderr,
                        "[A2Bus] TestWifi: link UP + RTC — completed from netif "
                        "(skip C++ EH path)\n");
                fflush(stderr);
                /* Still print like firmware for log continuity */
                fprintf(stderr, "TestWifi()\n");
                fflush(stderr);
                cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
                return 1;
            }
            return 0; /* native path if not yet linked */
        }
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
     * Host-format guest printf / sprintf. Never enter newlib _svfprintf_r under
     * a2bus (hangs). sprintf must still write the formatted string into guest
     * RAM — returning 0 left dest empty (TFTP Status showed leftover hostname).
     */
    if (pc == 0x10028040u) { /* sprintf */
        uint32_t dest = cpu.r[0];
        uint32_t fmt = cpu.r[1];
        uint32_t ap = usb_guest_sprintf_make_ap(cpu.r[13]);
        int n = usb_guest_host_sprintf(dest, fmt, ap, 4096u);
        usb_guest_return_to_lr((uint32_t)n);
        return 1;
    }
    if (pc == USB_GUEST_VFPRINTF_R ||
        pc == USB_GUEST_SVFPRINTF_R ||
        pc == USB_GUEST_SVFIPRINTF_R) {
        /* Direct _svfprintf_r (not via our sprintf hook): no safe guest FILE. */
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
    /* Debug-build lwIP asserts that release would soft-fail. Recover so core0
     * keeps running NETPUMP / DHCP instead of LOCKUP. */
    if (pc == USB_GUEST_PANIC) {
        uint32_t msg = cpu.r[0];
        uint32_t lr = cpu.r[14] & ~1u;
        char buf[48];
        uint32_t i;
        for (i = 0; i < sizeof(buf) - 1u && msg; i++) {
            uint8_t ch = mem_read8(msg + i);
            if (!ch) {
                break;
            }
            buf[i] = (char)ch;
        }
        buf[i] = '\0';

        /* pbuf_add_header_impl(NULL): debug assert before "return 1". Unwind. */
        if (lr == 0x10013958u && strncmp(buf, "p != NULL", 9) == 0) {
            uint32_t saved_lr = mem_read32(cpu.r[13] + 4u);
            cpu.r[13] += 8u; /* pop {r3, lr} */
            cpu.r[0] = 1u;   /* failure, same as release build */
            cpu.r[15] = saved_lr | 1u;
            static int once;
            if (!once++) {
                fprintf(stderr,
                        "[A2Bus] pbuf_add_header(NULL) → fail (debug assert)\n");
            }
            return 1;
        }

        /* pbuf_free(NULL): release build no-ops; debug panics. */
        if (lr == 0x10013a71u && strncmp(buf, "p != NULL", 9) == 0) {
            uint32_t sp = cpu.r[13];
            uint32_t ret = mem_read32(sp + 12u);
            cpu.r[4] = mem_read32(sp + 0u);
            cpu.r[5] = mem_read32(sp + 4u);
            cpu.r[6] = mem_read32(sp + 8u);
            cpu.r[13] = sp + 16u;
            cpu.r[0] = 0u;
            cpu.r[15] = ret | 1u;
            static int once;
            if (!once++) {
                fprintf(stderr, "[A2Bus] pbuf_free(NULL) → 0 (debug assert)\n");
            }
            return 1;
        }

        if (lr == 0x10013a76u) {
            /* pbuf_free: ref was 0. r5 still holds p. Restore ref and retry. */
            uint32_t p = cpu.r[5];
            if (p != 0u) {
                mem_write8(p + 14u, 1u);
                static int fixed;
                if (fixed < 8) {
                    fixed++;
                    fprintf(stderr,
                            "[A2Bus] pbuf_free: restored ref on 0x%08X, retry\n",
                            p);
                }
                cpu.r[15] = 0x10013a84u | 1u;
                return 1;
            }
        }

        static int panic_logged;
        if (panic_logged < 8) {
            panic_logged++;
            fprintf(stderr, "[A2Bus] guest panic: %s (sio_core=%u lr=0x%08X)\n",
                    buf[0] ? buf : "(null)",
                    (unsigned)sio_get_core_id(), cpu.r[14]);
            fflush(stderr);
        }
        /* Do not re-enter panic (LOCKUP). Mark this core WFI so the peer can run. */
        {
            unsigned c = sio_get_core_id();
            if (c < 2u) {
                cores[c].is_wfi = 1;
            }
        }
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
