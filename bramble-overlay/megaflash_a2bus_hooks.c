/*
 * Temporary MegaFlash a2bus guest hooks (wifi/RTC/veneer/SmartPort PC stubs).
 * Overlay-only — delete when CYW43 is mapped to the macOS network stack.
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
#include <time.h>


static int a2bus_rtc_hooks(void);
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

/* MegaFlash RTC normally starts via NTP; on a2bus seed from host clock. */
static void a2bus_seed_megaflash_rtc(void) {
    if (!a2bus_bridge_active()) {
        return;
    }
    /* Re-apply after crt0 BSS zero; skip if firmware already started RTC. */
    if (mem_read8(USB_GUEST_RTC_RUNNING_BSS)) {
        return;
    }
    mem_write8(USB_GUEST_RTC_RUNNING_BSS, 1u);
    fprintf(stderr, "[A2Bus] MegaFlash RTC seeded from host (rtcRunning=1)\n");
}

static void usb_guest_stub_aon_timer_get_time_calendar(void) {
    uint32_t tm_ptr = cpu.r[0];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) {
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return;
    }
    /* newlib struct tm: 9x int fields. */
    mem_write32(tm_ptr + 0u,  (uint32_t)t->tm_sec);
    mem_write32(tm_ptr + 4u,  (uint32_t)t->tm_min);
    mem_write32(tm_ptr + 8u,  (uint32_t)t->tm_hour);
    mem_write32(tm_ptr + 12u, (uint32_t)t->tm_mday);
    mem_write32(tm_ptr + 16u, (uint32_t)t->tm_mon);
    mem_write32(tm_ptr + 20u, (uint32_t)t->tm_year);
    mem_write32(tm_ptr + 24u, (uint32_t)t->tm_wday);
    mem_write32(tm_ptr + 28u, (uint32_t)t->tm_yday);
    mem_write32(tm_ptr + 32u, (uint32_t)t->tm_isdst);
    cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
}

static int a2bus_rtc_hooks(void) {
    if (!a2bus_bridge_active()) {
        return 0;
    }
    uint32_t pc = cpu.r[15] & ~1u;
    if (pc == USB_GUEST_AON_TIMER_GET_CAL) {
        a2bus_seed_megaflash_rtc();
        usb_guest_stub_aon_timer_get_time_calendar();
        return 1;
    }
    return 0;
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
#define USB_GUEST_NETERR_SSIDNOTSET   3u
#define USB_GUEST_DATA_XFER_MODE      0x2006160au /* dataBufferTransferMode */
#define USB_GUEST_DATA_BUFFER_INDEX   0x2000ccccu
#define USB_GUEST_MALLOC_MUTEX        0x20005164u /* malloc_mutex (.data, often still zero) */
#define USB_GUEST_MUTEX_ENTER_VENEER  0x10034da0u /* __mutex_enter_blocking_veneer */
#define USB_GUEST_MUTEX_EXIT_VENEER   0x10034d70u /* __mutex_exit_veneer */
#define USB_GUEST_SLEEP_UNTIL         0x1000bdc4u
#define USB_GUEST_SLEEP_US            0x1000be78u
#define USB_GUEST_SLEEP_MS            0x1000be9cu
/* Legacy aliases used by empty-SSID epilogue */
#define USB_GUEST_TESTWIFI_ERRBYTE    USB_GUEST_PARAMETER_BUFFER
#define USB_GUEST_TESTWIFI_FLAG_A     USB_GUEST_DATA_XFER_MODE
#define USB_GUEST_TESTWIFI_FLAG_B     USB_GUEST_DATA_BUFFER_INDEX
#define USB_GUEST_TESTWIFI_PARAM      USB_GUEST_DATA_BUFFER
#define USB_GUEST_TESTWIFI_MISC       USB_GUEST_PARAM_BUFFER_INDEX

/* Unlocked mutex owner = -1; spinlock slot in guest SRAM for [0]. */
static uint32_t a2bus_mutex_spinlock_byte = 0x20061f00u;

static void a2bus_guest_time_advance_us(uint64_t us) {
    if (us == 0u) {
        us = 1u;
    }
    if (us > 5000000ull) {
        us = 5000000ull; /* cap per call — cyw43 init uses many short sleeps */
    }
    timer_tick((uint32_t)us);
    /* Wake any WFE waiter that raced into sleep before this stub. */
    for (int i = 0; i < NUM_CORES; i++) {
        cores[i].is_wfi = 0;
    }
}

static void a2bus_ensure_malloc_mutex(void) {
    static int ready;
    if (ready++) {
        return;
    }
    /* mutex_t: [0]=spin_lock*, [4]=int8 owner (-1 unlocked). Zero owner → forever WFE. */
    if (mem_read32(USB_GUEST_MALLOC_MUTEX) == 0u) {
        mem_write32(USB_GUEST_MALLOC_MUTEX, a2bus_mutex_spinlock_byte);
        mem_write8(a2bus_mutex_spinlock_byte, 0);
    }
    mem_write8(USB_GUEST_MALLOC_MUTEX + 4u, 0xFFu);
    mem_write8(USB_GUEST_STDIO_MUTEX + 4u, 0xFFu);
    fprintf(stderr, "[A2Bus] malloc_mutex unlocked (owner=-1)\n");
}

/*
 * Opt into BusLoop-safe cyw43 stubs (no real JOIN / DNS / NTP). Default is real
 * CYW43 so MegaFlash Test Wifi / GetNetworkTime use hostif when -wifi [-tap].
 * Empty-SSID still fails fast: those paths throw C++ exceptions Bramble cannot unwind.
 */
static int a2bus_stub_wifi(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("BRAMBLE_A2BUS_STUB_WIFI");
        cached = (e && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        if (cached) {
            fprintf(stderr,
                    "[A2Bus] BRAMBLE_A2BUS_STUB_WIFI=1 — stub cyw43_arch_init "
                    "(no synthetic TestWifi/NTP results)\n");
        } else {
            fprintf(stderr, "[A2Bus] real CYW43 path (DNS/NTP via hostif when -tap)\n");
        }
    }
    return cached;
}

/*
 * Empty-SSID TestWifi/NTP uses C++ exceptions (__cxa_throw). Fail fast SSIDNOTSET.
 * Configured SSID: fall through to firmware + Bramble CYW43 (real diagnostics).
 * BRAMBLE_A2BUS_STUB_WIFI=1: stub only cyw43_arch_init / LED / InitCyw43 / ConnectWifi
 * so BusLoop can survive without host net (TestWifi then fails in firmware, not fake OK).
 */
static int a2bus_wifi_hooks(void) {
    if (!a2bus_bridge_active() || !cyw43.enabled) {
        return 0;
    }
    a2bus_ensure_malloc_mutex();
    uint32_t pc = cpu.r[15] & ~1u;

    if (pc == USB_GUEST_SLEEP_MS) {
        a2bus_guest_time_advance_us((uint64_t)cpu.r[0] * 1000ull);
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_SLEEP_US) {
        uint64_t us = (uint64_t)cpu.r[0] | ((uint64_t)cpu.r[1] << 32);
        a2bus_guest_time_advance_us(us);
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_SLEEP_UNTIL) {
        uint64_t target = (uint64_t)cpu.r[0] | ((uint64_t)cpu.r[1] << 32);
        uint64_t now = timer_state.time_us;
        if (target > now) {
            a2bus_guest_time_advance_us(target - now);
        } else {
            a2bus_guest_time_advance_us(1u);
        }
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    if (pc == USB_GUEST_MUTEX_ENTER_VENEER || pc == USB_GUEST_MUTEX_EXIT_VENEER ||
        pc == USB_GUEST_MUTEX_ENTER_V || pc == USB_GUEST_MUTEX_EXIT_V) {
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }

    if (a2bus_stub_wifi()) {
        if (pc == USB_GUEST_CYW43_ARCH_INIT) {
            static int once;
            if (!once++) {
                fprintf(stderr, "[A2Bus] stub cyw43_arch_init (BRAMBLE_A2BUS_STUB_WIFI=1)\n");
            }
            mem_write8(0x200615fcu, 1u); /* s_cyw43Inited */
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

    if (pc != USB_GUEST_DO_TEST_WIFI && pc != USB_GUEST_GET_NETWORK_TIME &&
        pc != USB_GUEST_TEST_WIFI && pc != 0x20004548u /* __DoTestWifi_veneer */ &&
        pc != 0x200044b0u /* __GetNetworkTime_veneer */) {
        return 0;
    }

    /* Configured SSID: real firmware JOIN / DHCP / DNS / NTP (no synthetic OK). */
    if (mem_read8(USB_GUEST_WIFI_SSID) != 0u) {
        return 0;
    }

    if (pc == USB_GUEST_DO_TEST_WIFI || pc == 0x20004548u) {
        fprintf(stderr, "[A2Bus] DoTestWifi: SSID not set → NETERR_SSIDNOTSET\n");
        fflush(stderr);
        mem_write8(USB_GUEST_TESTWIFI_ERRBYTE, (uint8_t)USB_GUEST_NETERR_SSIDNOTSET);
        mem_write8(USB_GUEST_TESTWIFI_FLAG_A, 0);
        mem_write32(USB_GUEST_TESTWIFI_FLAG_B, 0);
        mem_write8(USB_GUEST_REGISTERS + 2u, mem_read8(USB_GUEST_TESTWIFI_PARAM));
        mem_write32(USB_GUEST_TESTWIFI_MISC, 0);
        mem_write8(USB_GUEST_REGISTERS + 1u, (uint8_t)USB_GUEST_NETERR_SSIDNOTSET);
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
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
            const char *ssid = "BrambleNet";
            const char *wpa = "password";
            uint32_t i;
            for (i = 0; ssid[i] && i < 32u; i++) {
                mem_write8(USB_GUEST_WIFI_SSID + i, (uint8_t)ssid[i]);
            }
            mem_write8(USB_GUEST_WIFI_SSID + i, 0);
            for (i = 0; wpa[i] && i < 64u; i++) {
                mem_write8(USB_GUEST_CONFIG_BUFFER + 0x1au + i, (uint8_t)wpa[i]);
            }
            mem_write8(USB_GUEST_CONFIG_BUFFER + 0x1au + i, 0);
            fprintf(stderr, "[A2Bus] seeded WiFi SSID='%s' (BRAMBLE_A2BUS_SEED_WIFI)\n", ssid);
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
    /* Host-format guest printf — newlib _vfprintf_r is too slow on a2bus. */
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
    if (pc == USB_GUEST_PANIC) {
        static int panic_logged;
        uint32_t msg = cpu.r[0];
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
        /* Park on panic entry — do not resume a failing malloc path. */
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
    /* Security-register SPI program hangs under a2bus; config kept in SRAM (+ host file). */
    if (pc == USB_GUEST_ENCRYPT_WRITE_CFG ||
        pc == USB_GUEST_TS_WRITE_SEC_REG) {
        cpu.r[15] = (cpu.r[14] & ~1u) | 1u;
        return 1;
    }
    if (pc == USB_GUEST_TS_READ_SEC_REG) {
        /* Leave dest unchanged / zeros; callers validate magic. */
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
    if (a2bus_rtc_hooks()) {
        return 1;
    }
    if (a2bus_spi_flash_hooks()) {
        return 1;
    }
    return 0;
}
