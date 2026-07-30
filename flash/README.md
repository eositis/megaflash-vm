# SPI flash volumes (host backing files)

| File | Role |
|------|------|
| `spi-flash1.bin` | Chip #1 (default 64 MB). SmartPort boot volume 1 = first 32 MB |
| `spi-flash2.bin` | Chip #2 |
| `megaflash-user-config.bin` | Mirrored MegaFlash user settings (written by Bramble a2bus path) |

Gitignored. The MAME launcher passes absolute paths and runs Bramble with cwd = megaflash-vm so relative `flash/megaflash-user-config.bin` resolves here.

`../A2DeskTop.hdv` is a reference ProDOS image matching volume 1 contents (not loaded by MAME directly).
