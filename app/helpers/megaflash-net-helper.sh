#!/usr/bin/env bash
# Privileged helper for MegaFlash Operator: enable/disable pf NAT for CYW43 utun.
# Installed once under /usr/local/libexec and allowed via /etc/sudoers.d.
set -euo pipefail

ROOT_HINT="${MEGAFLASH_VM_ROOT:-}"
SCRIPT=""
if [[ -n "$ROOT_HINT" && -x "$ROOT_HINT/scripts/macos-host-net-prep.sh" ]]; then
  SCRIPT="$ROOT_HINT/scripts/macos-host-net-prep.sh"
elif [[ -x /usr/local/libexec/megaflash-host-net-prep.sh ]]; then
  SCRIPT=/usr/local/libexec/megaflash-host-net-prep.sh
else
  # Fall back to sibling layout relative to this helper when copied beside prep.
  HERE="$(cd "$(dirname "$0")" && pwd)"
  if [[ -x "$HERE/megaflash-host-net-prep.sh" ]]; then
    SCRIPT="$HERE/megaflash-host-net-prep.sh"
  fi
fi

if [[ -z "$SCRIPT" || ! -x "$SCRIPT" ]]; then
  echo "macos-host-net-prep.sh not found" >&2
  exit 1
fi

cmd="${1:-status}"
case "$cmd" in
  enable|disable|status)
    exec "$SCRIPT" "$cmd"
    ;;
  *)
    echo "Usage: $0 enable|disable|status" >&2
    exit 1
    ;;
esac
