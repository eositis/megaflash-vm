#!/usr/bin/env bash
# Prepare macOS host networking for Bramble CYW43 (-wifi -tap): enable pf NAT
# for 192.168.4.0/24. Intended to run under sudo (or via the MAME launcher's
# sudo -A askpass path).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BRAMBLE_ROOT="${BRAMBLE_ROOT:-$ROOT/../Bramble}"
if [[ -z "${PF_SCRIPT:-}" ]]; then
  if [[ -x "$ROOT/scripts/macos-cyw43-pf-nat.sh" ]]; then
    PF_SCRIPT="$ROOT/scripts/macos-cyw43-pf-nat.sh"
  else
    PF_SCRIPT="$BRAMBLE_ROOT/scripts/macos-cyw43-pf-nat.sh"
  fi
fi

if [[ ! -x "$PF_SCRIPT" ]]; then
  echo "pf helper not found: $PF_SCRIPT" >&2
  exit 1
fi

cmd="${1:-enable}"
case "$cmd" in
  enable|disable|status)
    exec "$PF_SCRIPT" "$cmd"
    ;;
  *)
    echo "Usage: $0 enable|disable|status" >&2
    exit 1
    ;;
esac
