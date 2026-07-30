# megaflash-vm

Integration project that links **MAME** (Apple //c) to **MegaFlash** firmware running under **Bramble**.

## Boundary

| Layer | Project | Responsibility |
|-------|---------|----------------|
| Inside the virtual GPIO / Apple-bus pins | [Bramble](../Bramble) | RP2350/M33 CPU, PIO, `a2bus` pin inject, `-a2bus-bridge` TCP endpoint that drives those pins, MegaFlash guest stubs |
| Outside the pins | **this repo** | MAME plugin, launch/orchestration, bus bring-up stub passed *into* Bramble, probe tools, operator docs |

Bramble remains a general Pico / RP2350 emulator. This repo owns the MegaFlash + MAME *virtual machine* experience around it.

## Components

| Path | Role |
|------|------|
| [`scripts/run-megaflash-mame.sh`](scripts/run-megaflash-mame.sh) | Start Bramble + wait for BusLoop + launch `mame apple2c4` |
| [`scripts/mame_plugins/megaflash_bridge/`](scripts/mame_plugins/megaflash_bridge/) | Lua plugin: forward `$C0C0–$C0C3` over TCP; mute DS1216E NSC |
| [`scripts/megaflash-mame.stub`](scripts/megaflash-mame.stub) | Bramble script: PHI0 + `core1launch` for MAME sessions |
| [`scripts/a2bus-probe-settings.py`](scripts/a2bus-probe-settings.py) | Host probe of MegaFlash commands over the TCP bridge |
| [`docs/MAME-BRIDGE.md`](docs/MAME-BRIDGE.md) | Operator guide |

## Sibling checkouts (default layout)

```
GitHub/
  Bramble/          # Pico/RP2350 emulator (+ -a2bus-bridge)
  MegaFlash/        # megaflash.uf2 / .elf
  megaflash-vm/     # this project
```

## Quick start

```bash
# From this repo
./scripts/run-megaflash-mame.sh
```

Requires: built `../Bramble/build/bramble` (or `../Bramble/bramble`), MegaFlash UF2, `iic.bin` (see docs), MAME, Ample companion dumps for CHR/keyboard/speech.

Environment overrides: `BRAMBLE`, `BRAMBLE_ROOT`, `MEGAFLASH_UF2`, `MEGAFLASH_ELF`, `IIC_BIN`, `MAME`, `BRAMBLE_A2BUS_PORT` — see [`docs/MAME-BRIDGE.md`](docs/MAME-BRIDGE.md).

## Attributions

| Work | Origin |
|------|--------|
| Bramble RP2040/RP2350 emulator | [Night-Traders-Dev/Bramble](https://github.com/Night-Traders-Dev/Bramble) (eositis MegaFlash bring-up fork) |
| MegaFlash firmware | [eositis/MegaFlash](https://github.com/eositis/MegaFlash) (and upstream credits in that tree) |
| MAME | [mamedev/mame](https://github.com/mamedev/mame) |
| Ample ROM packaging (staging source) | Ample / community Apple II dumps — not redistributed here |
| megaflash_bridge plugin + launcher | eositis (this project) |

## License

Plugin and scripts in this repository: same terms as the eositis Bramble fork work unless a file states otherwise. Bramble, MegaFlash, and MAME remain under their own licenses.
