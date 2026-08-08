# MegaFlash Operator (macOS)

Desktop app for running MegaFlash under Bramble: **Pico USB console** first, then hand off to **Apple //c + MAME**.

## Requirements

- macOS (Apple Silicon or Intel)
- Node.js 20+
- Rust stable (`rustup`)
- Overlay Bramble built in this repo: `cmake -B build && make -C build bramble`
- Firmware: `firmware/megaflash.uf2` (or sync via the app / `./scripts/sync-firmware-from-megaflash.sh`)
- For //c mode: MAME (`brew install mame`), `iic.bin`, Ample companion ROM dumps as documented in [MAME-BRIDGE.md](MAME-BRIDGE.md)

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

## Features (v1)

| Area | Behavior |
|------|----------|
| Pico | Pick UF2, 0–2 SPI flash chips, Start/Stop overlay Bramble with `-usb-console pty:…` |
| Console | Embedded xterm.js; select/copy; paste; optional file log |
| XMODEM | Buttons print host-side `sx`/`rx`/`tio` hints into the console (firmware menu still drives transfer) |
| //c handoff | **Stop Pico & launch //c** stops USB Bramble and runs `scripts/run-megaflash-mame.sh` with ROM / color / scale / Wi‑Fi settings |
| Network helper | One-time admin install → `/usr/local/libexec/megaflash-net-helper.sh` + sudoers NOPASSWD for pf NAT |
| Concurrent windows | UI toggle present but **disabled** (future) |

Settings persist in:

`~/Library/Application Support/MegaFlashOperator/settings.json`

## Network helper

1. In the app, click **Install network helper…** and approve the admin dialog once.
2. That installs the helper and a sudoers rule for passwordless `enable|disable|status`.
3. **Enable NAT** / MAME Wi‑Fi path can then call the helper without a password prompt.

Creating a utun for Bramble may still require elevation the first time unless sudoers is extended to cover the generated `.run/start-bramble-hostnet.sh` runner.

## Architecture note

USB console mode must **not** use `-a2bus-bridge` / Apple stubs (firmware skips the USB menu when Apple is “connected”). MAME mode uses the opposite. Concurrent USB + MAME needs two Bramble processes and firmware/host support; scaffolded only in v1.
