#!/usr/bin/env bash
# Open the Bramble USB CDC virtual serial port in a new macOS Terminal window.
set -euo pipefail

PTY_PATH="${USB_CONSOLE_PTY_PATH:-/tmp/bramble-usb-console}"
BAUD="${USB_CONSOLE_BAUD:-115200}"

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
echo "Opening $PTY_PATH ($REAL) at ${BAUD} baud (CDC ignores baud rate)."

if [[ -n "${TERM_PROGRAM:-}" && -t 1 ]]; then
  exec screen "$PTY_PATH" "$BAUD"
fi

osascript <<APPLESCRIPT
tell application "Terminal"
  activate
  do script "screen ${PTY_PATH} ${BAUD}"
end tell
APPLESCRIPT
