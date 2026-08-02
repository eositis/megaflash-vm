# Upstream patch scope (Phase 3)

When proposing Bramble improvements to Night-Traders / upstream, **do not include** Apple II / MegaFlash VM glue.

## Include in upstream patch

- M33 / Thumb-2 correctness (e.g. TBB/TBH vs LDRD)
- Dual-core / PIO / general emulator fixes
- WFI/WFE wall-clock TIMER advance (dual-core sleep_until)
- POWMAN AON timer (generic RP2350)
- CYW43 gSPI / CLM / `clmload_status` fidelity
- USB/UART console bridges (generic host I/O)
- **SPI flash host backing** (`-spi-flash1/2`) as a general feature
- Weak optional `bramble_ext_*` hooks (empty stubs only — no Apple implementations)

## Keep in megaflash-vm (not upstream)

- `bramble-overlay/` (`a2bus*`, bridge, MegaFlash PC stubs)
- MAME plugin / launcher / stubs
- USB CDC console runners (`run-megaflash-usb-console.sh`, connect helpers, XMODEM test scripts)
- Temporary empty-SSID fail-fast and `BRAMBLE_A2BUS_STUB_WIFI` (debt to delete once EH/BusLoop are solid)
- Post-radio `.data` restore + deferred BusLoop launch (MegaFlash RAM layout)
- MegaFlash firmware, `iic.bin`, SPI volume images

Stock `../Bramble` builds without Apple code. The MAME path uses this repo’s overlay binary. USB console uses `-usb-console` from Bramble plus this repo’s firmware/flash assets.

