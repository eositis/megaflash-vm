#!/usr/bin/env bash
# Double-click in Finder to install MAME 0.288 (the build Operator is tested with).
# Homebrew's `mame` formula is now newer; this extracts 0.288 into a local tap.
set -euo pipefail

export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export HOMEBREW_NO_AUTO_UPDATE=1
PINNED="0.288"
TAP="local/mame288"
AS_MAME="$HOME/Library/Application Support/MegaFlashOperator/mame"

echo "MegaFlash Operator — install MAME ${PINNED}"
echo

pause() {
  echo
  echo "You can close this window."
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
if [[ -z "$BREW" ]]; then
  die "Homebrew not found. Open MegaFlash Operator once (it installs Homebrew + python3), then run this again."
fi

mame_ver() {
  local bin="$1"
  [[ -x "$bin" ]] || return 1
  "$bin" -help 2>/dev/null | head -1 | grep -oE 'v[0-9]+\.[0-9]+' | head -1 | tr -d v
}

already="$(mame_ver /opt/homebrew/bin/mame || true)"
if [[ "$already" != "$PINNED" ]]; then
  already="$(mame_ver /usr/local/bin/mame || true)"
fi
if [[ "$already" != "$PINNED" ]]; then
  already="$(mame_ver "$AS_MAME/mame" || true)"
fi

link_into_operator() {
  local bin="$1"
  mkdir -p "$AS_MAME"
  ln -sf "$bin" "$AS_MAME/mame"
  echo "Linked $bin -> $AS_MAME/mame"
}

if [[ "$already" == "$PINNED" ]]; then
  for c in "$AS_MAME/mame" /opt/homebrew/bin/mame /usr/local/bin/mame; do
    if [[ "$(mame_ver "$c" || true)" == "$PINNED" ]]; then
      link_into_operator "$c"
      "$BREW" pin mame >/dev/null 2>&1 || true
      echo "MAME ${PINNED} is already installed."
      pause
      exit 0
    fi
  done
fi

echo "Extracting Homebrew formula mame ${PINNED} into tap ${TAP}…"
if ! "$BREW" tap-info "$TAP" >/dev/null 2>&1; then
  "$BREW" tap-new "$TAP"
fi

FORMULA_FILE="$("$BREW" --repository "$TAP")/Formula/mame@${PINNED}.rb"
if [[ ! -f "$FORMULA_FILE" ]]; then
  "$BREW" extract --version="$PINNED" mame "$TAP"
fi
[[ -f "$FORMULA_FILE" ]] || die "brew extract did not create $FORMULA_FILE"

echo "Installing ${TAP}/mame@${PINNED} (this can take several minutes)…"
"$BREW" install "${TAP}/mame@${PINNED}"

PREFIX="$("$BREW" --prefix "${TAP}/mame@${PINNED}")"
BIN="$PREFIX/bin/mame"
[[ -x "$BIN" ]] || die "mame binary missing at $BIN"
got="$(mame_ver "$BIN" || true)"
[[ "$got" == "$PINNED" ]] || die "installed mame reports version '${got:-unknown}', expected ${PINNED}"

link_into_operator "$BIN"
"$BREW" pin "mame@${PINNED}" >/dev/null 2>&1 || true

echo
echo "MAME ${PINNED} is ready. Launch //c from MegaFlash Operator."
pause
