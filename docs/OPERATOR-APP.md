# MegaFlash Operator (macOS)

Desktop app for running MegaFlash under Bramble: **Pico USB console** first, then hand off to **Apple //c + MAME**.

## Requirements

- macOS (Apple Silicon or Intel)
- Node.js 20+
- Rust stable (`rustup`)
- Overlay Bramble built in this repo: `cmake -B build && make -C build bramble`
- Firmware: `firmware/megaflash.uf2` (or sync via the app / `./scripts/sync-firmware-from-megaflash.sh`)
- For //c mode: MAME **0.288** (double-click `scripts/Install MAME 0.288.command`, or Operator **Install MAME 0.288…**). Do not `brew install mame` (that is 0.289 now). `iic.bin` is bundled.

## Develop

```bash
cd app
npm install
npm run tauri dev
```

Set `MEGAFLASH_VM_ROOT` if the app cannot find this repo (defaults walk from the binary / env).

## Build .app

```bash
cd app
npm install
npm run tauri build
```

The app bundle lands under:

`app/src-tauri/target/release/bundle/macos/MegaFlash Operator.app`

The DMG (`…/bundle/dmg/MegaFlash Operator_0.1.0_aarch64.dmg`) contains the **.app**, an Applications shortcut, **Install MAME 0.288.command**, and a short Read Me. Double-click the `.command` on that disk after opening Operator once (so Homebrew exists). Do not `brew install mame` (that is 0.289).

**Important:** `cargo build --release` alone does **not** refresh that `.app`. After a fix, run `npm run tauri build` (or `npm run tauri dev`) so Finder/`open` is not launching a stale binary. Compare mtimes under `…/bundle/macos/…/MacOS/megaflash-operator` vs `target/release/megaflash-operator`.

## Features (v1)

| Area | Behavior |
|------|----------|
| Pico | Pick UF2, 0–2 SPI flash chips, Start/Stop overlay Bramble with `-usb-console pty:…` |
| Console | Embedded xterm.js; select/copy; paste; optional file log |
| XMODEM | After menu Upload → CONFIRM → `CCCC`, **XMODEM upload…** copies the file to a temp path and runs `scripts/test-xmodem-upload.py` (`XMODEM_SEND_ONLY`) without waiting for another `C`. Progress lines appear as `[xmodem] …`. Abort/`Please Select` ends the wait (not a 2-hour hang). File picker: `.po` / `.hdv` / `.dsk` / `.bin` / `.img`. |
| Pico / //c | **Collapsible** setup sections (paths, flash, display); Start/Stop stay visible. Left pane scrolls independently of the USB console. Window is capped to the current screen. |
| Right pane | Tabs: **USB console** and **Apple //c**. Launching //c selects the Apple tab and starts MAME with `SDL_VIDEO_WINDOW_POS` over that pane. If Accessibility is already trusted, Operator silently resizes the SDL window (no system prompt — prompting every timer tick caused a permission loop). MAME stays a normal OS window with a title bar. **Stop //c** signals the whole process group. |
| //c handoff | **Launch //c** runs `scripts/run-megaflash-mame.sh`. Display prefs come from Operator `settings.json`: monitor **Color / B&W / Green / Amber** (B&W = MAME value 7, true grayscale); scale = `-intscalex`/`-intscaley` plus `-nomaximize -resolution`. MAME Lua forwards `$C0C0–$C0C7` (MegaFlash + Uthernet II). Slot-4 `$C0C4–$C0C7` is **host-completed** in overlay Bramble (W5100 window + local DHCP `192.168.4.3`); guest BusLoop inject does not update those registers. |
| Network helper | One-time admin install → `/usr/local/libexec/megaflash-net-helper.sh` + sudoers NOPASSWD for pf NAT |
| Concurrent windows | UI toggle present but **disabled** (future) |

Bramble/MAME child stderr is written to `~/Library/Logs/MegaFlashOperator/` (`bramble-pico.stderr.log`, etc.). Do not pipe undrained stdio — UF2 loader output fills the pipe and blocks PTY creation.

The Operator opens the USB PTY in **raw / no-echo** mode (like `tio`/`screen`). Default macOS termios ECHO would loop guest TX into RX and spam the console. xterm uses `convertEol` so bare LF from firmware starts a new line correctly.

Banner/menu newlines require Bramble’s `__wrap_puts` host path to append `\n` (the compiler turns `printf("…\n")` into `puts`). Rebuild overlay `bramble` after pulling that fix.

Settings persist in:

`~/Library/Application Support/MegaFlashOperator/settings.json`

Shipping to another Mac (feasibility, not implemented): [PACKAGING-MACOS.md](PACKAGING-MACOS.md).

## Network helper

1. In the app, click **Install network helper…** and approve the admin dialog once.
2. That installs the helper and a sudoers rule for passwordless `enable|disable|status`.
3. **Enable NAT** / MAME Wi‑Fi path can then call the helper without a password prompt. Bramble itself runs unelevated (utun + userspace UDP NAT do not need root once pf is on).

Creating a utun for Bramble may still require elevation **only if the helper is not installed** (askpass dialog for `.run/start-bramble-hostnet.sh`).

## Architecture note

USB console mode must **not** use `-a2bus-bridge` / Apple stubs (firmware skips the USB menu when Apple is “connected”). MAME mode uses the opposite. Concurrent USB + MAME needs two Bramble processes and firmware/host support; scaffolded only in v1.

## Responsiveness (Tauri IPC)

Tauri commands that touch session mutexes or spawn/wait on processes are **`async` + `spawn_blocking`**, and the UI refreshes settings/status/net sequentially (not `Promise.all`). Session code locks **settings before procs** so `status()` cannot deadlock with start/stop. Without that, overlapping sync IPC on the main thread caused a macOS beachball at launch.
