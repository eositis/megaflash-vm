#!/usr/bin/env bash
# Stage overlay runtime into app/src-tauri/runtime for Tauri bundle.resources.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/app/src-tauri/runtime"
BRAMBLE="$ROOT/bramble"
[[ -x "$BRAMBLE" ]] || BRAMBLE="$ROOT/build/bramble"
if [[ ! -x "$BRAMBLE" ]]; then
  echo "overlay bramble missing; cmake -B build && make -C build bramble" >&2
  exit 1
fi
if [[ ! -f "$ROOT/firmware/megaflash.uf2" ]]; then
  echo "missing $ROOT/firmware/megaflash.uf2" >&2
  exit 1
fi
if [[ ! -f "$ROOT/iic.bin" ]]; then
  echo "missing $ROOT/iic.bin" >&2
  exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST/firmware" "$DEST/scripts/mame_plugins" "$DEST/scripts/mame_cfg" \
  "$DEST/roms/apple2c4" "$DEST/roms/votrsc01a" "$DEST/flash"

cp -p "$BRAMBLE" "$DEST/bramble"
chmod 755 "$DEST/bramble"
cp -p "$ROOT/iic.bin" "$DEST/iic.bin"
cp -p "$ROOT/firmware/megaflash.uf2" "$DEST/firmware/"
if [[ -f "$ROOT/firmware/megaflash.elf" ]]; then
  cp -p "$ROOT/firmware/megaflash.elf" "$DEST/firmware/"
fi

copy_exec() {
  local src="$1" dest="$2"
  cp -p "$src" "$dest"
  chmod 755 "$dest"
}

copy_exec "$ROOT/scripts/run-megaflash-mame.sh" "$DEST/scripts/"
copy_exec "$ROOT/scripts/macos-cyw43-pf-nat.sh" "$DEST/scripts/"
copy_exec "$ROOT/scripts/macos-host-net-prep.sh" "$DEST/scripts/"
copy_exec "$ROOT/scripts/macos-sudo-askpass.sh" "$DEST/scripts/"
cp -p "$ROOT/scripts/megaflash-mame.stub" "$DEST/scripts/"
cp -p "$ROOT/scripts/test-xmodem-upload.py" "$DEST/scripts/"
cp -p "$ROOT/scripts/apply-mame-display.py" "$DEST/scripts/"
cp -R "$ROOT/scripts/mame_plugins/megaflash_bridge" "$DEST/scripts/mame_plugins/"
cp -p "$ROOT/scripts/mame_cfg/"*.cfg "$DEST/scripts/mame_cfg/" 2>/dev/null || true

# Demo SPI volumes: Operator profile first (what you boot today), then repo flash/.
FLASH_SRC="${MEGAFLASH_DEMO_FLASH:-}"
if [[ -z "$FLASH_SRC" ]]; then
  FLASH_SRC="$HOME/Library/Application Support/MegaFlashOperator/flash"
fi
if [[ ! -f "$FLASH_SRC/spi-flash1.bin" ]]; then
  FLASH_SRC="$ROOT/flash"
fi
if [[ ! -f "$FLASH_SRC/spi-flash1.bin" ]]; then
  echo "missing demo spi-flash1.bin (set MEGAFLASH_DEMO_FLASH or populate $HOME/Library/Application Support/MegaFlashOperator/flash)" >&2
  exit 1
fi
mkdir -p "$DEST/flash"
cp -p "$FLASH_SRC/spi-flash1.bin" "$DEST/flash/"
if [[ -f "$FLASH_SRC/spi-flash2.bin" ]]; then
  cp -p "$FLASH_SRC/spi-flash2.bin" "$DEST/flash/"
fi
if [[ -f "$FLASH_SRC/megaflash-user-config.bin" ]]; then
  cp -p "$FLASH_SRC/megaflash-user-config.bin" "$DEST/flash/"
fi

echo "staged $DEST"
