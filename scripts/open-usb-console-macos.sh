#!/usr/bin/env bash
# Open the Bramble USB CDC virtual serial port in a new macOS Terminal window.
# Prefers tio (built-in XMODEM: Ctrl-T then x) when installed; else screen.
# Override: USB_CONSOLE_CLIENT=tio|screen
set -euo pipefail

PTY_PATH="${USB_CONSOLE_PTY_PATH:-/tmp/bramble-usb-console}"
BAUD="${USB_CONSOLE_BAUD:-115200}"
CLIENT="${USB_CONSOLE_CLIENT:-}"

if [[ "$(uname -s)" != Darwin ]]; then
  echo "This helper is for macOS only. Use: ./scripts/connect-usb-console.sh" >&2
  exit 1
fi

if [[ ! -e "$PTY_PATH" ]]; then
  echo "Serial port not found: $PTY_PATH" >&2
  echo "Start Bramble first: ./scripts/run-megaflash-usb-console.sh" >&2
  exit 1
fi

REAL="$PTY_PATH"
if [[ -L "$PTY_PATH" ]]; then
  REAL="$(readlink "$PTY_PATH")"
fi

pick_client() {
  if [[ -n "$CLIENT" ]]; then
    echo "$CLIENT"
    return
  fi
  if command -v tio >/dev/null 2>&1; then
    echo tio
    return
  fi
  echo screen
}

CLIENT="$(pick_client)"

case "$CLIENT" in
  tio)
    if ! command -v tio >/dev/null 2>&1; then
      echo "tio not found (brew install tio). Falling back to screen." >&2
      CLIENT=screen
    fi
    ;;
  screen)
    ;;
  *)
    echo "USB_CONSOLE_CLIENT must be tio or screen (got: $CLIENT)" >&2
    exit 1
    ;;
esac

if [[ "$CLIENT" == tio ]]; then
  CMD="tio ${PTY_PATH}"
  echo "Opening $PTY_PATH ($REAL) with tio."
  echo "XMODEM: MegaFlash menu 2 → unit → CONFIRM → wait for C → Ctrl-T then x (prefer XMODEM-1K)."
  echo "Quit tio: Ctrl-T then q."
else
  CMD="screen ${PTY_PATH} ${BAUD}"
  echo "Opening $PTY_PATH ($REAL) with screen (no built-in XMODEM)."
  echo "For uploads use tio instead: USB_CONSOLE_CLIENT=tio $0"
  echo "  or: tio ${PTY_PATH}   (see docs/TIO-CONSOLE.md)"
fi

if [[ -n "${TERM_PROGRAM:-}" && -t 1 ]]; then
  exec ${CMD}
fi

# Quote for AppleScript "do script"
AS_CMD="${CMD//\\/\\\\}"
AS_CMD="${AS_CMD//\"/\\\"}"

osascript <<APPLESCRIPT
tell application "Terminal"
  activate
  do script "${AS_CMD}"
end tell
APPLESCRIPT
