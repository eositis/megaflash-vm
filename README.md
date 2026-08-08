# megaflash-vm

Integration project that links **MAME** (Apple //c) to **MegaFlash** firmware running under **Bramble**, plus USB CDC diagnostic console orchestration.

## Boundary

| Layer | Project | Responsibility |
|-------|---------|----------------|
| Inside the virtual GPIO / Apple-bus pins (inject) | **this repo’s overlay `bramble`** | `a2bus` pin inject, `-a2bus-bridge`, temporary MegaFlash guest stubs |
| Pico emulator core + SPI flash files | [Bramble](../Bramble) | RP2350/M33 CPU, PIO, USB/UART, `-spi-flash*`, weak `bramble_ext_*` hooks |
| Outside the pins (host glue) | **this repo** | MAME plugin, launchers, USB console runners, orchestration, firmware/flash assets, docs |

Build the overlay binary (required for MAME; also preferred for USB console):

```bash
cmake -B build && make -C build bramble
```

## Components

| Path | Role |
|------|------|
| [`app/`](app/) | **MegaFlash Operator** macOS UI (Tauri) — Pico console + //c handoff |
| [`scripts/run-megaflash-mame.sh`](scripts/run-megaflash-mame.sh) | Start overlay Bramble + wait for BusLoop + launch `mame apple2c4` |
| [`scripts/run-megaflash-usb-console.sh`](scripts/run-megaflash-usb-console.sh) | USB CDC UserTerminal (PTY/TCP) — **no** Apple-bus stub |
| [`scripts/connect-usb-console.sh`](scripts/connect-usb-console.sh) | Attach `screen`/`nc` to the USB console |
| [`bramble-overlay/`](bramble-overlay/) | Apple-bus + MegaFlash hooks linked into `./bramble` |
| [`scripts/mame_plugins/megaflash_bridge/`](scripts/mame_plugins/megaflash_bridge/) | Lua plugin: forward `$C0C0–$C0C3` over TCP; mute DS1216E NSC |
| [`scripts/megaflash-mame.stub`](scripts/megaflash-mame.stub) | Bramble script: PHI0 + `core1launch` for MAME sessions |
| [`scripts/a2bus-probe-settings.py`](scripts/a2bus-probe-settings.py) | Host probe of MegaFlash commands over the TCP bridge |
| [`firmware/`](firmware/) | Guest `megaflash.uf2` / `.elf` (gitignored binaries) |
| [`flash/`](flash/) | SPI volume backing files (gitignored `*.bin`) |
| [`docs/OPERATOR-APP.md`](docs/OPERATOR-APP.md) | Operator desktop app guide |
| [`docs/MAME-BRIDGE.md`](docs/MAME-BRIDGE.md) | MAME operator guide |
| [`docs/USB-CONSOLE.md`](docs/USB-CONSOLE.md) | USB CDC diagnostic terminal guide |
| [`docs/TIO-CONSOLE.md`](docs/TIO-CONSOLE.md) | tio attach / XMODEM / quit |

## Sibling checkouts (default layout)

```
GitHub/
  Bramble/          # Pico/RP2350 emulator
  MegaFlash/        # firmware *source* / build tree
  megaflash-vm/     # this project — runtime assets + MAME/USB glue
```

## Runtime assets (this repo)

| Path | Role |
|------|------|
| `firmware/megaflash.uf2` (+ `.elf`) | Guest firmware loaded by Bramble |
| `flash/spi-flash*.bin` | SPI volume backing (SmartPort boot / XMODEM) |
| `iic.bin` | MegaFlash-patched Apple //c system ROM for MAME |
| `A2DeskTop.hdv` | Reference ProDOS image for volume 1 (optional) |

Refresh UF2/ELF after a MegaFlash rebuild: `./scripts/sync-firmware-from-megaflash.sh`

## Quick start — Operator app (macOS)

```bash
cd app && npm install && npm run tauri dev
# or build: npm run tauri build
```

See [`docs/OPERATOR-APP.md`](docs/OPERATOR-APP.md).

## Quick start — MAME

```bash
./scripts/run-megaflash-mame.sh
```

Requires: overlay `./bramble`, `firmware/megaflash.uf2`, `flash/spi-flash*.bin`, `iic.bin`, MAME, Ample companion dumps for CHR/keyboard/speech.

## Quick start — USB console

```bash
# Terminal 1
./scripts/run-megaflash-usb-console.sh
# Terminal 2
./scripts/connect-usb-console.sh
```

See [`docs/USB-CONSOLE.md`](docs/USB-CONSOLE.md). Do **not** pass an Apple-bus stub script — that suppresses the USB menu.

Environment overrides: `BRAMBLE`, `BRAMBLE_ROOT`, `MEGAFLASH_UF2`, `MEGAFLASH_ELF`, `IIC_BIN`, `MAME`, `BRAMBLE_A2BUS_PORT`, `USB_CONSOLE_PORT`, `USB_CONSOLE_TCP` — see the docs above.

## Attributions

| Work | Origin |
|------|--------|
| Bramble RP2040/RP2350 emulator | [Night-Traders-Dev/Bramble](https://github.com/Night-Traders-Dev/Bramble) (eositis MegaFlash bring-up fork) |
| MegaFlash firmware | [eositis/MegaFlash](https://github.com/eositis/MegaFlash) (and upstream credits in that tree) |
| MAME | [mamedev/mame](https://github.com/mamedev/mame) |
| Ample ROM packaging (staging source) | Ample / community Apple II dumps — not redistributed here |
| megaflash_bridge plugin + launchers | eositis (this project) |

## License

Plugin and scripts in this repository: same terms as the eositis Bramble fork work unless a file states otherwise. Bramble, MegaFlash, and MAME remain under their own licenses.
