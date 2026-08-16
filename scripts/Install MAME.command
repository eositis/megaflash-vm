#!/usr/bin/env bash
# Double-click in Finder. Installs Homebrew's current MAME:
#   brew install mame
set -euo pipefail

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

echo "MegaFlash Operator — brew install mame"
echo

pause() {
  echo
  read -r -p "Press Return to close. " _ || true
}

die() {
  echo "ERROR: $*" >&2
  pause
  exit 1
}

BREW=""
if [[ -x /opt/homebrew/bin/brew ]]; then
  BREW=/opt/homebrew/bin/brew
elif [[ -x /usr/local/bin/brew ]]; then
  BREW=/usr/local/bin/brew
fi
[[ -n "$BREW" ]] || die "Homebrew not found. Open MegaFlash Operator once (it installs Homebrew), then run this again."

if command -v mame >/dev/null 2>&1; then
  echo "mame is already on PATH: $(command -v mame)"
  mame -help 2>/dev/null | head -1 || true
  pause
  exit 0
fi

echo "Running: $BREW install mame"
echo "(this can take several minutes)"
"$BREW" install mame

command -v mame >/dev/null 2>&1 || die "brew install mame finished but mame is not on PATH"
mame -help 2>/dev/null | head -1 || true
echo
echo "MAME is installed. Launch //c from MegaFlash Operator."
pause
