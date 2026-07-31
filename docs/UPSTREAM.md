# Upstream patch scope (Phase 3)

When proposing Bramble improvements to Night-Traders / upstream, **do not include** Apple II / MegaFlash VM glue.

## Include in upstream patch

- M33 / Thumb-2 correctness (e.g. TBB/TBH vs LDRD)
- Dual-core / PIO / general emulator fixes
- USB/UART console bridges (generic host I/O)
- **SPI flash host backing** (`-spi-flash1/2`) as a general feature
- Weak optional `bramble_ext_*` hooks (empty stubs only — no Apple implementations)

## Keep in megaflash-vm (not upstream)

- `bramble-overlay/` (`a2bus*`, bridge, MegaFlash PC stubs)
- MAME plugin / launcher / stubs
- Temporary wifi/RTC a2bus stubs (until CYW43 ↔ macOS network)
- MegaFlash firmware, `iic.bin`, SPI volume images

Stock `../Bramble` builds without Apple code. The MAME path uses this repo’s overlay binary.
