# SPI flash volumes (host backing files)

| File | Role |
|------|------|
| `spi-flash1.bin` | Chip #1 (default 64 MB). SmartPort boot volume 1 = first 32 MB |
| `spi-flash2.bin` | Chip #2 |
| `megaflash-user-config.bin` | Mirrored MegaFlash user settings (written by Bramble a2bus path) |

Gitignored (do not commit 64 MB images). `scripts/stage-operator-runtime.sh` copies the **current Operator profile** volumes (`~/Library/Application Support/MegaFlashOperator/flash/`, else this directory) into the packaged `.app` so a demo Mac boots the same SmartPort disks. First launch copies those into Application Support if the dest files are missing or still empty (all-zero header). Later user writes are not overwritten.

`../A2DeskTop.hdv` is a reference ProDOS image matching volume 1 contents (not loaded by MAME directly).
