# USB CDC console (MegaFlash diagnostic terminal)

Orchestration lives in **megaflash-vm**. Bramble provides the Pico emulator and `-usb-console` / `-spi-flash*` host I/O; this repo holds the runner, firmware, and flash images.

MegaFlash on Pico W checks **`stdio_usb_connected()`** after boot. That maps to TinyUSB **`tud_cdc_n_connected()`**, which requires:

1. USB device stack running (`stdio_usb_init()` in firmware)
2. Bramble’s **USB host simulation** to enumerate the device and assert **DTR** (`SET_CONTROL_LINE_STATE`)
3. Firmware not taking the “Apple II connected” path that disables the USB menu (Release builds)

Bramble implements (1)–(2) in `src/usb.c` whenever the guest enables the USB controller.

## Quick start — virtual serial port (recommended for terminal apps)

Use a **PTY** so standard serial programs (`screen`, `cu`, Serial.app, etc.) can attach without TCP.

On **macOS**, `./scripts/run-megaflash-usb-console.sh` defaults to PTY mode (virtual serial). Set `USB_CONSOLE_TCP=1` to use the legacy TCP socket instead.

**Terminal 1 — emulator:**

```bash
./scripts/run-megaflash-usb-console.sh
# or (overlay bramble + local firmware):
./bramble ./firmware/megaflash.uf2 \
  -arch m33 -clock 150 -cores 1 \
  -usb-serial -usb-stdio -timeout 120
```

Wait for:

```text
[USB] CDC console on serial port /dev/ttysNNN
[USB]   attach: screen /tmp/bramble-usb-console 115200
```

**Terminal 2 — menu / diagnostics:**

```bash
./scripts/connect-usb-console.sh
# or: screen /tmp/bramble-usb-console 115200
```

### macOS terminal and serial programs

| Program | How to connect |
|---------|----------------|
| **Terminal.app** (new window) | `./scripts/open-usb-console-macos.sh` (prefers **tio** if installed — needed for XMODEM; else screen) |
| **tio** (recommended for XMODEM) | `tio /tmp/bramble-usb-console` — see [TIO-CONSOLE.md](TIO-CONSOLE.md) (`Ctrl-T` then `x`) |
| **screen** / **cu** | `./scripts/connect-usb-console.sh` or `screen /tmp/bramble-usb-console 115200` (screen has **no** XMODEM send) |
| **Serial.app**, **CoolTerm**, etc. | Pick `/dev/ttysNNN` from the port list (path printed in Bramble stderr), or open symlink `/tmp/bramble-usb-console` if the app allows custom paths. Baud: **115200** (ignored by CDC). |
| **iTerm2** | `tio /tmp/bramble-usb-console` or `screen …` |

Bramble must stay running in Terminal 1 while you use the serial port. If connect says `PTY not found`, Bramble is not running or exited (e.g. timeout).

### XMODEM upload (menu item 2)

The PTY is a **binary-safe** serial pipe — suitable for XMODEM-CRC. Use a program that can **send a file on the same connection** you use for the menu:

**Recommended: minicom** (single session, built-in XMODEM send):

```bash
brew install minicom
# Terminal 1: ./scripts/run-megaflash-usb-console.sh
# Terminal 2:
minicom -D /tmp/bramble-usb-console -b 115200
```

In minicom: menu **2** (Upload) → unit **1** → type **CONFIRM** → when MegaFlash sends **`C`** repeatedly, press **Ctrl-A** then **S**, pick your `.po`/`.hdv` file, choose **xmodem** or **xmodem-crc**.

**Using tio:** step-by-step attach, menu upload, and quit are in **[TIO-CONSOLE.md](TIO-CONSOLE.md)** (`tio /tmp/bramble-usb-console`, menu **2** → **CONFIRM** → **Ctrl-T** **`x`**, quit with **Ctrl-T** **`q`**). tio’s sender waits ~1s per ACK, so a full 32MB `.po` may still fail; for that use **minicom**, **lrzsz**, or `./scripts/test-32mb-xmodem.sh`. **XMODEM-1K** is preferred over 128-byte CRC in tio.

**Alternative: lrzsz** (must own the port — disconnect tio/minicom first):

```bash
brew install lrzsz
# After MegaFlash shows "Please start upload..." and sends C, quit other clients, then:
sz -y /tmp/bramble-usb-console /path/to/disk.po
```

Only **one** program may open the serial port at a time.

**External flash persistence:** MegaFlash has two SPI flash chips. Each chip is backed by its own host file when `-spi-flash1` / `-spi-flash2` is set. Defaults are this repo’s `flash/spi-flash*.bin` (gitignored volume images). Units are 32MB slices within each chip file (chip #1 → units 1–2, chip #2 → units 3–4 at default sizes). XMODEM uploads survive between runs. Without these flags, flash is volatile (in-memory only).

| Flag | Purpose |
|------|---------|
| `-spi-flash1 [path]` | SPI chip #1 backing file (default: `flash/spi-flash1.bin`) |
| `-spi-flash1-size <MB>` | Chip #1 size in MB (default 64; rounded up to multiple of 32) |
| `-spi-flash2 [path]` | SPI chip #2 backing file (default: `flash/spi-flash2.bin`) |
| `-spi-flash2-size <MB>` | Chip #2 size in MB |
| `SPI_FLASH1`, `SPI_FLASH2` | Run-script env overrides for chip paths |
| `SPI_FLASH1_SIZE`, `SPI_FLASH2_SIZE` | Run-script env overrides for chip sizes |

**Emulator note:** XMODEM upload uses `WriteBlockForImageTransfer` (direct SPI program path), not the normal `WriteBlock` veneer — Bramble stubs that to the SPI flash backing files. Bulk RX uses host-polling `usb_getraw_timeout` for XMODEM-1K packets.

**Large uploads (32MB `.po`):** Emulated XMODEM is slow. Use **one core**, a long timeout, and no other program on the PTY (quit `tio`/`minicom` before restarting Bramble):

```bash
CORES=1 TIMEOUT=7200 ./scripts/run-megaflash-usb-console.sh
# minicom / tio on /tmp/bramble-usb-console → menu 2 → drive → CONFIRM → XMODEM-1K send
```

If the host shows `N`/`Write packet to serial failed`, the guest likely hit its XMODEM error limit while flash writes were slow under emulation (PTY backpressure), or Bramble exited (`-timeout` too low). Bramble throttles PTY reads when the host RX buffer exceeds **90%** full and resumes below **80%** (stderr: `host RX throttle ON/OFF`). Restart with `TIMEOUT=7200` (or `TIMEOUT=0`), `-cores 1`, and no other client on the PTY. A full 32MB image can take ~1–2 minutes in the emulator with mmap-backed flash I/O.

**Automated full-image test** (starts Bramble, waits for menu, runs XMODEM, verifies `flash/spi-flash1.bin`):

```bash
./scripts/test-32mb-xmodem.sh
# optional image path:
./scripts/test-32mb-xmodem.sh /path/to/disk.po
```

Do not run another client on `/tmp/bramble-usb-console` while this script is active. Smaller regression:

```bash
XMODEM_TEST_BYTES=262144 python3 scripts/test-xmodem-upload.py /path/to/disk.po
```

| Flag | Purpose |
|------|---------|
| `-usb-serial [path]` | PTY with optional symlink (default `/tmp/bramble-usb-console`) |
| `-usb-console pty[:path]` | Same as `-usb-serial` |
| `-usb-console <port>` | Legacy TCP mode (`nc localhost <port>`) |

Baud rate is ignored by USB CDC but terminal programs often require one (use `115200`).

## Quick start — TCP session

Same pattern as Bramble’s [UART-CONSOLE.md](../../Bramble/docs/eositis/UART-CONSOLE.md), but traffic goes to **USB CDC**, not UART0.

**Do not use `megaflash-bus.stub` or `run-megaflash-stub.sh` for USB menu testing.** The stub’s `a2phi` event makes `IsAppleConnected()` return true. Firmware then enters **`core0Loop()`** (Apple bus / network) and **does not** run **`UserTerminal()`** (USB diagnostic menu). Bramble also ignores `a2phi` while `-usb-console` is active, but the no-stub runner is the supported path.

**Terminal 1 — emulator (no Apple stub):**

```bash
./scripts/run-megaflash-usb-console.sh
# or:
./bramble ./firmware/megaflash.uf2 \
  -arch m33 -clock 150 -cores 1 \
  -usb-console 5555 -usb-stdio -timeout 7200
```

Wait for:

```text
[USB] CDC console listening on TCP port 5555
[USB] CDC active — host may call stdio_usb_connected() (DTR asserted)
```

The `CDC active` line appears automatically once the guest USB stack is up (no `-trace-usb` required). Connect `nc` early so DTR is mirrored into guest RAM.

**Terminal 2 — menu / diagnostics:**

```bash
./scripts/connect-usb-console.sh 5555
```

You should see the DEBUG boot banner, ASCII art, and `UserTerminal()` main menu (`Device Information`, upload/download ProDOS image, etc.).

**Verified (2026-06-02):** `pico2_debug/megaflash.uf2` with `-cores 1`, no Apple-bus stub — TCP receives the full banner plus menu prompt `Please Select:`. Sending `1` echoes `1` and runs menu item 1 (Device Information).

**Input path:** TCP bytes are pushed into the guest TinyUSB CDC RX fifo; `stdio_usb` driver RAM is seeded because `stdio_usb_init` is skipped under emulation.

### Flags

| Flag | Purpose |
|------|---------|
| `-usb-console <port>` | Bidirectional USB CDC ↔ TCP (`nc localhost <port>`) |
| `-usb-console pty[:path]` / `-usb-serial` | USB CDC on a host PTY (virtual serial port) |
| `-usb-stdio` | Route host input to **USB CDC first** (needed when firmware also prints on UART0) |
| `-stdin` | Bridge terminal to guest console (UART or USB); implies USB stdout when CDC is active |
| `-trace-usb` | Log enumeration steps to stderr (debug “connected at boot”) |

**Do not** combine `-stdin` on Bramble with a second `nc` feeding the same port — use **either** interactive Bramble stdin **or** TCP `nc`, not both.

### Apple bus stub (slot I/O only — not USB menu)

Use when exercising **`core0Loop`**, `a2read` / `a2write`, and core1 — **not** when you want the USB terminal:

```bash
../Bramble/scripts/run-megaflash-stub.sh -timeout 120
```

If you set `USB_CONSOLE_PORT` on the stub runner, it exits with a hint to use megaflash-vm `./scripts/run-megaflash-usb-console.sh` instead.

## How firmware chooses USB vs Apple bus

From `MegaFlash/pico/main.c` (Pico W / CheckPicoW):

| Build | Condition | Core0 behavior |
|-------|-----------|----------------|
| **Debug** | `stdio_usb_connected()` | `UserTerminal()` (USB diagnostic module) |
| **Release** | `stdio_usb_connected() && !IsAppleConnected()` | `UserTerminal()` |
| Either | Apple connected at boot | `core0Loop()` (network), USB terminal suppressed in Release |

Implications for Bramble:

- Use a **Debug** UF2 (`pico2_debug/megaflash.uf2`) for easiest USB-menu testing, **or**
- In **Release**, do **not** drive `IsAppleConnected()` true before core0 checks USB — avoid early `a2phi` in `megaflash-bus.stub` if you want the USB menu.
- UART debug (`-uart-console`) does **not** satisfy `stdio_usb_connected()`; use **`-usb-console`**.

## What Bramble’s USB host does

On each `usb_step()` after the guest enables USB:

1. Simulates attach / bus reset / descriptor fetch / `SET_CONFIGURATION`
2. Sends **`SET_CONTROL_LINE_STATE` with DTR+RTS** so `tud_cdc_n_connected()` returns true
3. Bridges CDC **IN** (device → host) to TCP or stdout
4. Bridges TCP or stdin → CDC **OUT** (host → device) via `usb_cdc_rx_push()`

RP2350 USB registers: `0x50100000` (DPRAM) / `0x50110000` (controller) — already mapped.

## Troubleshooting

| Symptom | Check |
|---------|--------|
| No `[USB] CDC active` | Rebuild `bramble` after pulling USB fixes; guest may not have reached `stdio_usb_init()` yet |
| TCP connects but no menu | Missing **`-usb-stdio`**; or core0 still in TinyUSB init (see PC ~`0x1000F5C0`) — connect `nc` early, wait for `CDC active` |
| PC stuck ~`0x1000F5C6` | USB control-transfer stall in `stdio_usb_init`; ensure fresh build and `-usb-console` |
| Never enters `UserTerminal` | **Apple connected** (`IsAppleConnected()` after stub `a2phi`) — use `run-megaflash-usb-console.sh` **without** `-script`; Release also requires `!IsAppleConnected()` |
| Garbled text | Wrong port or duplicate input (`-stdin` + `nc` both feeding RX) |

## UART vs USB (MegaFlash)

| Channel | Firmware API | Bramble flag |
|---------|--------------|--------------|
| UART cable / debug serial | `stdio_uart_init()` | `-uart-console <port>` |
| USB serial (FT232 / built-in USB) | `stdio_usb_init()` | `-usb-console <port>` + `-usb-stdio` |

## Windows

Run Bramble on **macOS, Linux, or WSL2**. Connect with **PuTTY** (raw TCP to `host:5555`) or **ncat** from Windows. Native Bramble on Windows still needs a Winsock port of the bridge code.
