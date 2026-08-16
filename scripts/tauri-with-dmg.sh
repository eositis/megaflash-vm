#!/usr/bin/env bash
# npm run tauri … — after `build`, put the MAME installer on the DMG.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
tauri "$@"
if [[ "${1:-}" == "build" ]]; then
  bash "$HERE/repack-operator-dmg.sh"
fi
