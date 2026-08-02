#!/usr/bin/env bash
# Full 32MB XMODEM upload test. Do not pkill bramble in the same command as start.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BRAMBLE_ROOT="${BRAMBLE_ROOT:-$ROOT/../Bramble}"
IMAGE="${1:-$ROOT/A2OSX.STABLE.32MB.po}"
LOG="${TMPDIR:-/tmp}/bramble-32mb-test.log"

if [[ ! -e "$IMAGE" ]]; then
  # Fall back to sibling Bramble checkout if image lived there historically
  if [[ -e "$BRAMBLE_ROOT/A2OSX.STABLE.32MB.po" ]]; then
    IMAGE="$BRAMBLE_ROOT/A2OSX.STABLE.32MB.po"
  else
    echo "Image not found: $IMAGE" >&2
    exit 1
  fi
fi

if [[ ! -x "$ROOT/bramble" && ! -x "$ROOT/build/bramble" ]]; then
  echo "Building overlay bramble..." >&2
  cmake -B "$ROOT/build" && make -C "$ROOT/build" bramble
fi

rm -f "$ROOT/flash/spi-flash1.bin"
mkdir -p "$ROOT/flash"
env CORES=1 TIMEOUT=7200 "$ROOT/scripts/run-megaflash-usb-console.sh" </dev/null >"$LOG" 2>&1 &
BPID=$!
trap 'kill "$BPID" 2>/dev/null || true' EXIT

for _ in $(seq 1 90); do
  kill -0 "$BPID" 2>/dev/null || { echo "bramble exited early; see $LOG" >&2; exit 1; }
  if [[ -e /tmp/bramble-usb-console ]] && grep -q 'Please Select' "$LOG" 2>/dev/null; then
    break
  fi
  sleep 1
done

export XMODEM_TEST_BYTES=0
export XMODEM_ACK_TIMEOUT=300
exec python3 "$ROOT/scripts/test-xmodem-upload.py" "$IMAGE"
