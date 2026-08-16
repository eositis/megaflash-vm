#!/usr/bin/env bash
# First-launch: Homebrew + a real python3 (not the Xcode CLT stub).
# Homebrew's installer must not run as root; wrap sudo with -A + askpass.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASKPASS="${SUDO_ASKPASS:-$ROOT/scripts/macos-sudo-askpass.sh}"
export SUDO_ASKPASS="$ASKPASS"
export SUDO_ASKPASS_PROMPT="${SUDO_ASKPASS_PROMPT:-MegaFlash Operator needs administrator access to install Homebrew (python3 for //c a2bus wait, and MAME).}"
export NONINTERACTIVE=1
export HOMEBREW_NO_AUTO_UPDATE=1
export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

python_ok() {
  local c
  for c in /opt/homebrew/bin/python3 /usr/local/bin/python3; do
    if [[ -x "$c" ]] && "$c" -c 'import sys' >/dev/null 2>&1; then
      echo "$c"
      return 0
    fi
  done
  return 1
}

if python_ok >/dev/null; then
  python_ok
  exit 0
fi

if [[ ! -x "$ASKPASS" ]]; then
  echo "askpass helper missing: $ASKPASS" >&2
  exit 1
fi
chmod +x "$ASKPASS" 2>/dev/null || true

WRAP="$(mktemp -d "${TMPDIR:-/tmp}/megaflash-sudowrap.XXXXXX")"
cleanup() { rm -rf "$WRAP"; }
trap cleanup EXIT
cat >"$WRAP/sudo" <<'EOF'
#!/bin/bash
exec /usr/bin/sudo -A "$@"
EOF
chmod 755 "$WRAP/sudo"
export PATH="$WRAP:$PATH"

if [[ ! -x /opt/homebrew/bin/brew && ! -x /usr/local/bin/brew ]]; then
  echo "[setup] installing Homebrew (admin dialog)…"
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi

BREW=""
if [[ -x /opt/homebrew/bin/brew ]]; then
  BREW=/opt/homebrew/bin/brew
elif [[ -x /usr/local/bin/brew ]]; then
  BREW=/usr/local/bin/brew
fi
if [[ -z "$BREW" ]]; then
  echo "Homebrew install finished but brew was not found" >&2
  exit 1
fi

if py=$(python_ok); then
  echo "$py"
  exit 0
fi

echo "[setup] brew install python3…"
"$BREW" install python3
if py=$(python_ok); then
  echo "$py"
  exit 0
fi
echo "python3 still not runnable after brew install python3" >&2
exit 1
