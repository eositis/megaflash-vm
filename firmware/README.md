# Guest firmware for Bramble (MegaFlash UF2 + optional ELF symbols).

Default launcher paths:

- `megaflash.uf2` — required
- `megaflash.elf` — optional (`-symbols` for BSS resolve)

Refresh from a MegaFlash build tree:

```bash
./scripts/sync-firmware-from-megaflash.sh
# or: MEGAFLASH_BUILD=/path/to/pico2_debug ./scripts/sync-firmware-from-megaflash.sh
```

These binaries are gitignored (build artifacts / redistribution).
