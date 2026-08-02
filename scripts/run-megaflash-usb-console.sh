#!/usr/bin/env bash
# Run MegaFlash under Bramble for USB CDC UserTerminal testing.
# Lives in megaflash-vm (orchestration + firmware/flash assets).
#
# Do NOT use megaflash-bus.stub / megaflash-mame.stub here — a2phi / Apple-bus
# events make IsAppleConnected() true and firmware skips the USB diagnostic menu.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BRAMBLE_ROOT="${BRAMBLE_ROOT:-$ROOT/../Bramble}"
# Prefer overlay binary from this repo; fall back to sibling Bramble.
if [[ -z "${BRAMBLE:-}" ]]; then
  if [[ -x "$ROOT/bramble" ]]; then
    BRAMBLE="$ROOT/bramble"
  elif [[ -x "$ROOT/build/bramble" ]]; then
    BRAMBLE="$ROOT/build/bramble"
  else
    BRAMBLE="$BRAMBLE_ROOT/bramble"
  fi
fi
UF2="${MEGAFLASH_UF2:-$ROOT/firmware/megaflash.uf2}"
PORT="${USB_CONSOLE_PORT:-5555}"
PTY_PATH="${USB_CONSOLE_PTY_PATH:-/tmp/bramble-usb-console}"

if [[ ! -x "$BRAMBLE" ]]; then
  BRAMBLE="$BRAMBLE_ROOT/build/bramble"
fi
if [[ ! -x "$BRAMBLE" ]]; then
  echo "bramble not found" >&2
  echo "  build overlay: cmake -B build && make -C build bramble  (in megaflash-vm)" >&2
  echo "  or set BRAMBLE= / build sibling Bramble" >&2
  exit 1
fi
if [[ ! -f "$UF2" ]]; then
  echo "UF2 not found: $UF2" >&2
  echo "  sync: ./scripts/sync-firmware-from-megaflash.sh" >&2
  echo "  or set MEGAFLASH_UF2=" >&2
  exit 1
fi

# macOS: default to virtual serial (PTY) so screen/cu/Serial.app can attach.
# Set USB_CONSOLE_TCP=1 to keep the legacy TCP socket mode instead.
USE_PTY=0
if [[ "${USB_CONSOLE_TCP:-0}" == 1 ]]; then
  USE_PTY=0
elif [[ "${USB_CONSOLE_PTY:-0}" == 1 ]]; then
  USE_PTY=1
elif [[ "$(uname -s)" == Darwin ]]; then
  USE_PTY=1
fi

USB_CONSOLE_ARG=(-usb-console "$PORT")
if [[ "$USE_PTY" == 1 ]]; then
  USB_CONSOLE_ARG=(-usb-console "pty:${PTY_PATH}")
fi

SPI_FLASH_ARGS=()
if [[ -n "${SPI_FLASH1:-}" ]]; then
  SPI_FLASH_ARGS+=(-spi-flash1 "$SPI_FLASH1")
elif [[ -f "$ROOT/flash/spi-flash1.bin" ]]; then
  SPI_FLASH_ARGS+=(-spi-flash1 "$ROOT/flash/spi-flash1.bin")
else
  SPI_FLASH_ARGS+=(-spi-flash1)
fi
if [[ -n "${SPI_FLASH1_SIZE:-}" ]]; then
  SPI_FLASH_ARGS+=(-spi-flash1-size "$SPI_FLASH1_SIZE")
fi
if [[ -n "${SPI_FLASH2:-}" ]]; then
  SPI_FLASH_ARGS+=(-spi-flash2 "$SPI_FLASH2")
elif [[ -f "$ROOT/flash/spi-flash2.bin" ]]; then
  SPI_FLASH_ARGS+=(-spi-flash2 "$ROOT/flash/spi-flash2.bin")
else
  SPI_FLASH_ARGS+=(-spi-flash2)
fi
if [[ -n "${SPI_FLASH2_SIZE:-}" ]]; then
  SPI_FLASH_ARGS+=(-spi-flash2-size "$SPI_FLASH2_SIZE")
fi

STDIO_ARGS=()
if [[ "${USB_CONSOLE_STDIO:-0}" == 1 ]]; then
  STDIO_ARGS=(-usb-stdio)
fi

exec "$BRAMBLE" "$UF2" \
  -arch m33 \
  -clock 150 \
  -cores "${CORES:-1}" \
  "${USB_CONSOLE_ARG[@]}" \
  ${STDIO_ARGS+"${STDIO_ARGS[@]}"} \
  "${SPI_FLASH_ARGS[@]}" \
  -timeout "${TIMEOUT:-7200}" \
  "$@"
