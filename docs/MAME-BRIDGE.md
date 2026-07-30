# MAME + Bramble MegaFlash bridge

Operator guide for **megaflash-vm**: link MAME’s Apple //c (rev 4, `apple2c4`) to MegaFlash running under **Bramble**. Slot‑4 soft‑switches `$C0C0–$C0C3` are forwarded over TCP to Bramble’s Apple-bus injector (`-a2bus-bridge`).

**Project split:** anything *inside* the virtual GPIO / Apple-bus pins lives in Bramble (`a2bus`, bridge server, guest stubs). This repo owns the MAME plugin, launcher, bring-up stub, and probes *outside* those pins. See [README.md](../README.md).

## Prerequisites

1. **Sibling checkouts:** `../Bramble`, `../MegaFlash` (build tree), and this repo
2. **Build Bramble:** in `../Bramble`: `cmake -B build && make -C build bramble bramble_tests`
3. **Guest firmware in this repo:** `firmware/megaflash.uf2` (and optional `.elf`) — run `./scripts/sync-firmware-from-megaflash.sh` after building MegaFlash
4. **MegaFlash IIc ROM:** `iic.bin` (32 KiB) at this repo root
5. **SPI volumes:** `flash/spi-flash1.bin` / `spi-flash2.bin` (SmartPort boot from volume 1)
6. **MAME:** `brew install mame` (or set `MAME=/path/to/mame`)
7. **Stock companion dumps** (CHR, keyboard, Votrax `sc01a.bin`). On macOS with Ample, the launcher copies them from
   `~/Library/Application Support/Ample/roms` into `./roms/` and stages **`iic.bin` as `3410445b.256`**.

**Important:** rompath is **local `./roms` only**. If Ample stays on the rompath, MAME prefers the CRC-matching stock `3410445b.256` and silently ignores MegaFlash’s ROM — you get stock Slinky (“unable to start from memory card”) and no MegaFlash boot menu.

Expect a **WRONG CHECKSUM** warning for `3410445b.256`; that means MegaFlash ROM is loaded.

## Run (integrated)

From this repo:

```bash
./scripts/run-megaflash-mame.sh
```

What it does:

1. Starts Bramble (`BRAMBLE_ROOT`, default `../Bramble`) with `-arch m33 -clock 150 -cores 2 -a2bus-bridge 19765` and `scripts/megaflash-mame.stub` (PHI0 + core1 launch).
2. Waits for a TCP `PING` on `127.0.0.1:19765`.
3. Starts `mame apple2c4` with `-plugin megaflash_bridge` (Lua taps on `$C0C0–$C0C3`).

**Do not** pass `-ramsize` above the default **128K**. Extra RAM enables MAME’s built-in Slinky and conflicts with MegaFlash.

**Do not** combine with `-usb-console` on the Bramble side for this mode (Apple online suppresses the USB UserTerminal).

The launcher sets `-pluginspath` to `scripts/mame_plugins` **plus** the stock MAME plugins directory (needs `boot.lua`). It also passes `-plugins -plugin megaflash_bridge`.

The Lua plugin must open the bridge socket with **READ|WRITE only** (no CREATE). With CREATE, MAME tries to listen on the same port if connect fails and hits `Address already in use` while Bramble holds the port — MegaFlash then appears missing.

Bramble must keep running for the whole MAME session: with `-a2bus-bridge` the usual 1B-instruction safety exit is disabled (same idea as stdin/GDB interactive mode). If you see `Instruction limit reached (1B)` then MegaFlash dies under MAME.

SPI flash for MAME uses absolute paths under **this repo’s** `flash/spi-flash*.bin` (SmartPort boot volume 1 = first 32 MB of `spi-flash1.bin`). Without populated flash, detect may still pass but there is nothing to boot. User settings mirror is `flash/megaflash-user-config.bin` (Bramble opens that relative path; the launcher `cd`s here before start). Bramble’s `flash/` is a symlink to this directory so USB-console runs share the same volumes.

MAME is started **without** a floppy/hard-disk image. The //c is expected to boot through MegaFlash SmartPort (PR#4 / “Boot MegaFlash”) from **flash volume 1** (first 32 MB of `flash/spi-flash1.bin`). That volume currently holds ProDOS **`A2.DESKTOP`** (same contents as `A2DeskTop.hdv`). Upload/replace via the USB console XMODEM path if needed.

Flash-resident `ldr.w pc,[pc]` veneers to SRAM are Thumb-broken in Bramble when `r0≠0`. The a2bus path rewrites `__TranslateUnitNum_veneer` and `__CheckWriteEnableKey_veneer` (option 7 / DriveMapping) so those calls reach SRAM.

`apple2c4` always includes a Dallas DS1216E no-slot clock. The Lua plugin mutes it on `$C100–$CFFF` so ProDOS/time come from MegaFlash (`CMD_GETTIMESTR` / clockdriver).

**Test Wifi / NTP** under a2bus are synthetic by default (real gSPI JOIN still WIP). The launcher passes `-wifi` by default (`NO_WIFI=1` to disable).

With an **empty SSID**, a2bus fails fast with `NETERR_SSIDNOTSET` (3) so the control panel does not hang: empty-SSID paths throw C++ exceptions that Bramble’s EH still cannot unwind. With a **configured SSID**, a2bus completes Test Wifi with synthetic `NETERR_NONE` and display strings `192.168.4.2` / `255.255.255.0` / `192.168.4.1` / `192.168.4.1`. When **NTP** is enabled in settings (`NTPCLIENTFLAG`), `GetNetworkTime` seeds MegaFlash `rtcRunning` from the host clock and `aon_timer_get_time_calendar` returns host local time — so the CP clock and ProDOS timestamps advance without a live NTP server. Optional `BRAMBLE_A2BUS_SEED_WIFI=1` seeds `BrambleNet`/`password` for bring-up without Save Settings.

**Control-Reset:** Thumb-2 `TBH` in newlib `_svfprintf_r` must not be decoded as `LDRD` (bit6 collision); that HardFault locked core1 and tripped MAME-session shutdown.

Firmware `[u2macraw]` telemetry from `U2_Net_Poll` is suppressed on a2bus stderr by default (`BRAMBLE_A2BUS_U2MACRAW=1` to show).

When MAME exits, Bramble shuts down: the launcher no longer `exec`s MAME (so its EXIT trap can kill Bramble), and the bridge also requests exit after a client that issued READ/WRITE disconnects (preflight PING/PEEK alone does not).

**Real `cyw43_arch_init` is off by default under a2bus.** Concurrent InitPicoLed gSPI + BusLoopSlinky HardFaults core1 (`PC≈0x1FFF8F6C` during `clmload_status`), which freezes Slinky registers and makes MAME report MegaFlash not found / no boot. Default stubs: `cyw43_arch_init`, `cyw43_arch_gpio_put`, `InitCyw43`, `ConnectWifi`. Set `BRAMBLE_A2BUS_REAL_WIFI=1` only to exercise real gSPI (FEEDBEAD works; BusLoop still dies — WIP).

**Host NAT / real internet:** Bramble supports `-tap <if>` and `-net` (TAP + IP forward + masquerade) on **Linux only**. macOS builds print `TAP interface only supported on Linux`. The MAME launcher does not pass `-tap`/`-net`. Until gSPI join works, Test Wifi is not a live host link.

Bring-up notes (a2bus):

- Skip `stdio_usb_init` so core0 reaches `InitPicoLed`; stub `cyw43_arch_init` unless `BRAMBLE_A2BUS_REAL_WIFI=1`.
- Host-format `__wrap_printf`, stub `hw_claim_*` / `check_alloc`, skip firmware `multicore_launch` when the script already started core1.
- Force `CheckPicoW()==true` when `-wifi` is on.
- Optional `-tap` / `-net` (via script `"$@"`) for a real host network once join works.

`CMD_COLDSTART` must finish before the Apple reads `configbyte1`. The bridge pump waits for a full STATUS **BUSY→idle** cycle (no early exit if BUSY was never seen). A premature return left `configbyte1=0`, which clears `AUTOBOOTFLAG` and makes the IIc firmware skip slot 4 (`NEXTBOOTSLOT=$C6`).

## Environment

| Variable | Default | Meaning |
|----------|---------|---------|
| `BRAMBLE_ROOT` | `../Bramble` | Bramble checkout (binary under `bramble` or `build/bramble`) |
| `BRAMBLE` | `$BRAMBLE_ROOT/bramble` | Emulator binary override |
| `BRAMBLE_A2BUS_PORT` | `19765` | TCP port (Bramble listen + MAME plugin) |
| `MEGAFLASH_UF2` | `./firmware/megaflash.uf2` | Guest firmware |
| `MEGAFLASH_ELF` | `./firmware/megaflash.elf` | Resolves `registers` BSS address |
| `IIC_BIN` | `./iic.bin` | MegaFlash-patched system ROM |
| `MEGAFLASH_FLASH_DIR` | `./flash` | SPI volume directory |
| `SPI_FLASH1` / `SPI_FLASH2` | `$MEGAFLASH_FLASH_DIR/spi-flash{1,2}.bin` | Absolute backing paths passed to Bramble |
| `MAME_ROMPATH` | `./roms` (staged) | Must **not** include Ample if you want MegaFlash maincpu |
| `AMPLE_ROMS` | `~/Library/Application Support/Ample/roms` | Source for CHR/keyboard/speech staging only |
| `BRAMBLE_IIC_BIN` | set by launcher to `IIC_BIN` | Optional Lua re-overlay path |
| `MAME` | `mame` | Emulator binary |
| `TIMEOUT` | unset (none) | Bramble `-timeout` seconds |
| `BRAMBLE_A2BUS_REAL_WIFI` | unset | `1` = real `cyw43_arch_init` (breaks BusLoop until fixed) |
| `BRAMBLE_A2BUS_SEED_WIFI` | unset | `1` = seed BrambleNet SSID/password in config |
| `BRAMBLE_A2BUS_U2MACRAW` | unset | `1` = print firmware `[u2macraw]` poll telemetry |

## Protocol (Bramble `-a2bus-bridge`)

Client (MAME plugin) → server (Bramble):

| Op | Bytes | Meaning |
|----|-------|---------|
| `0x00` | op | PING |
| `0x01` | op | PHI0 pulse |
| `0x02` | op, nibble | Inject READ `$C0C0+nibble`; return **pre-cycle** register shadow (bus byte Apple sees) |
| `0x03` | op, nibble, data | Inject WRITE |
| `0x04` | op, nibble | PEEK register shadow (no bus inject) |

Reply: `status` (0 = ok), `data`.

Firmware boots in **Slinky** mode (`registers[2] == 0xf0`). MegaFlash ROM (or the activation read sequence `$C0C2,$C0C0,$C0C0,$C0C3,$C0C1`) switches to native mode; then `$C0C3` ID is **`$96`** (reads toggle with `~` per MegaFlash).

After ID is live (`$96`/`$69`) and STATUS is idle, Bramble mirrors BusLoop DATA/PARAM/ID side-effects in host SRAM **without** a guest pump per RPC. That keeps `CMD_LOAD_CPANEL` (~58×256 `$C0C2` reads) interactive. CMD writes and BUSY cycles still inject+pump core1. BSS addresses are tied to `pico2_debug/megaflash.elf`.

Host-side DATA pointer advance must read `dataBufferTransferMode` as an **8-bit** BSS field. A 32-bit load pulled adjacent flags (`dhcp_pcb_refcount`, …) and treated `MODE_LINEAR` as interleaved, which corrupted `CMD_GETUSERSETTINGS` (control panel **Unexpected Error:0**) and any other linear transfer.

Script `core1launch` starts BusLoop on core1 while core0 may still be in crt0/`InitSpi`. a2bus hooks late-seed `configBuffer` on `GetUserSettings` so option 7 validation sees `timezoneidver=1` even when `LoadAllConfigs` never ran on core0.

Control-panel **Save** must not run real SPI security-register programming under a2bus (`EncryptWriteConfigToFlash` would leave STATUS BUSY and freeze on **Saving...**). Settings are applied in SRAM and mirrored to `flash/megaflash-user-config.bin`.


## Smoke test

1. Launcher reaches “BusLoopSlinky ready”.
2. In MAME with `iic.bin`, boot far enough for MegaFlash cold-start / activation.
3. `$C0C3` should show MegaFlash ID behavior (`$96` / `$69` alternating on successive reads).

CRC: expect **WRONG CHECKSUM** for `3410445b.256` when MegaFlash `iic.bin` is staged. If verifyroms is “good” with no warning, MAME is still using stock ROM4.

## Files

**This repo (outside the pins):**

- [`scripts/mame_plugins/megaflash_bridge/`](../scripts/mame_plugins/megaflash_bridge/) — MAME Lua plugin
- [`scripts/run-megaflash-mame.sh`](../scripts/run-megaflash-mame.sh) — launcher
- [`scripts/megaflash-mame.stub`](../scripts/megaflash-mame.stub) — PHI / core1 bring-up (fed to Bramble `-script`)
- [`scripts/a2bus-probe-settings.py`](../scripts/a2bus-probe-settings.py) — host probe over TCP

**Bramble (inside / pin face):**

- `../Bramble/src/a2bus_bridge.c` — TCP server that drives virtual Apple-bus GPIO
- `../Bramble/src/a2bus.c` — bus inject / PHI0 / register shadow
