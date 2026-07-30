#!/usr/bin/env bash
# Copy MegaFlash pico2_debug UF2/ELF into this repo's firmware/ for the VM launcher.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${MEGAFLASH_BUILD:-$ROOT/../MegaFlash/pico/pico2_debug}"
DEST="${MEGAFLASH_FIRMWARE_DIR:-$ROOT/firmware}"

mkdir -p "$DEST"
for f in megaflash.uf2 megaflash.elf; do
  if [[ ! -f "$SRC/$f" ]]; then
    echo "missing $SRC/$f (set MEGAFLASH_BUILD=...)" >&2
    exit 1
  fi
  cp -f "$SRC/$f" "$DEST/$f"
  echo "copied $SRC/$f -> $DEST/$f"
done
