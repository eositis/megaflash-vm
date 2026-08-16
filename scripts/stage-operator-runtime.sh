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
copy_exec "$ROOT/scripts/macos-ensure-homebrew-python.sh" "$DEST/scripts/"
copy_exec "$ROOT/scripts/Install MAME.command" "$DEST/scripts/"
cp -p "$ROOT/scripts/megaflash-mame.stub" "$DEST/scripts/"
cp -p "$ROOT/scripts/test-xmodem-upload.py" "$DEST/scripts/"
cp -p "$ROOT/scripts/apply-mame-display.py" "$DEST/scripts/"
cp -R "$ROOT/scripts/mame_plugins/megaflash_bridge" "$DEST/scripts/mame_plugins/"
cp -p "$ROOT/scripts/mame_cfg/"*.cfg "$DEST/scripts/mame_cfg/" 2>/dev/null || true

# Companion dumps (CHR/keyboard/Votrax). Do not copy 3410445b.256 — launcher
# overlays MegaFlash iic.bin as that name.
stage_dump() {
  local dest="$1"
  shift
  mkdir -p "$(dirname "$dest")"
  local src
  for src in "$@"; do
    if [[ -f "$src" ]]; then
      cp -p "$src" "$dest"
      return 0
    fi
  done
  return 1
}
AS_ROMS="$HOME/Library/Application Support/MegaFlashOperator/roms"
AMPLE="$HOME/Library/Application Support/Ample/roms"
stage_dump "$DEST/roms/apple2c4/341-0265-a.chr" \
  "$AS_ROMS/apple2c4/341-0265-a.chr" \
  "$ROOT/roms/apple2c4/341-0265-a.chr" || true
stage_dump "$DEST/roms/apple2c4/342-0132-c.e12" \
  "$AS_ROMS/apple2c4/342-0132-c.e12" \
  "$ROOT/roms/apple2c4/342-0132-c.e12" || true
stage_dump "$DEST/roms/votrsc01a/sc01a.bin" \
  "$AS_ROMS/votrsc01a/sc01a.bin" \
  "$ROOT/roms/votrsc01a/sc01a.bin" || true
if [[ ! -f "$DEST/roms/apple2c4/341-0265-a.chr" && -f "$AMPLE/apple2c.zip" ]]; then
  unzip -o -j -qq "$AMPLE/apple2c.zip" "341-0265-a.chr" -d "$DEST/roms/apple2c4" || true
  unzip -o -j -qq "$AMPLE/apple2c.zip" "342-0132-c.e12" -d "$DEST/roms/apple2c4" || true
fi
if [[ ! -f "$DEST/roms/votrsc01a/sc01a.bin" && -f "$AMPLE/votrsc01a.zip" ]]; then
  unzip -o -j -qq "$AMPLE/votrsc01a.zip" "sc01a.bin" -d "$DEST/roms/votrsc01a" || true
fi
if [[ ! -f "$DEST/roms/apple2c4/341-0265-a.chr" || ! -f "$DEST/roms/apple2c4/342-0132-c.e12" ]]; then
  echo "warning: CHR/keyboard dumps not found for staging (//c launch will try Ample or download)" >&2
fi

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
