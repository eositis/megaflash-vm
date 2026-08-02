#!/usr/bin/env bash
# Attach to Bramble USB CDC console (-usb-console TCP or PTY).
set -euo pipefail

PTY_PATH="${USB_CONSOLE_PTY_PATH:-/tmp/bramble-usb-console}"
BAUD="${USB_CONSOLE_BAUD:-115200}"

USE_PTY=0
if [[ "${USB_CONSOLE_TCP:-0}" == 1 ]]; then
  USE_PTY=0
elif [[ "${USB_CONSOLE_PTY:-0}" == 1 || "${1:-}" == "pty" ]]; then
  USE_PTY=1
elif [[ "$(uname -s)" == Darwin && -e "$PTY_PATH" ]]; then
  USE_PTY=1
fi

if [[ "$USE_PTY" == 1 ]]; then
  if [[ ! -e "$PTY_PATH" ]]; then
    echo "PTY not found: $PTY_PATH" >&2
    echo "Start Bramble first (Terminal 1):" >&2
    echo "  ./scripts/run-megaflash-usb-console.sh" >&2
    echo "On macOS this uses a virtual serial port by default." >&2
    echo "For TCP mode instead: USB_CONSOLE_TCP=1 ./scripts/run-megaflash-usb-console.sh" >&2
    exit 1
  fi
  REAL="$PTY_PATH"
  if [[ -L "$PTY_PATH" ]]; then
    REAL="$(readlink "$PTY_PATH")"
    echo "Serial port: $PTY_PATH -> $REAL" >&2
  fi
  if command -v screen >/dev/null 2>&1; then
    exec screen "$PTY_PATH" "$BAUD"
  fi
  if command -v cu >/dev/null 2>&1; then
    exec cu -l "$PTY_PATH" -s "$BAUD"
  fi
  echo "Need screen or cu to open $PTY_PATH (baud $BAUD is ignored by CDC)" >&2
  exit 1
fi

PORT="${1:-${USB_CONSOLE_PORT:-5555}}"
HOST="${USB_CONSOLE_HOST:-127.0.0.1}"

if command -v nc >/dev/null 2>&1; then
  exec nc "$HOST" "$PORT"
fi
if command -v ncat >/dev/null 2>&1; then
  exec ncat "$HOST" "$PORT"
fi
if command -v socat >/dev/null 2>&1; then
  exec socat -,raw,echo=0 "TCP:${HOST}:${PORT}"
fi

echo "Need nc, ncat, or socat to connect to ${HOST}:${PORT}" >&2
exit 1
