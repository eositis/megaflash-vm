#!/usr/bin/env bash
# Rebuild the Operator DMG so Finder shows Install MAME.command
# next to the .app (Tauri's DMG is app + Applications only).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE="$ROOT/app/src-tauri/target/release/bundle"
APP="$BUNDLE/macos/MegaFlash Operator.app"
DMG="$BUNDLE/dmg/MegaFlash Operator_0.1.0_aarch64.dmg"
CMD="$ROOT/scripts/Install MAME.command"

if [[ ! -d "$APP" ]]; then
  echo "missing $APP (run npm run tauri build first)" >&2
  exit 1
fi
if [[ ! -f "$CMD" ]]; then
  echo "missing $CMD" >&2
  exit 1
fi

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/megaflash-dmg.XXXXXX")"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cp -p "$CMD" "$STAGE/Install MAME.command"
chmod 755 "$STAGE/Install MAME.command"
cat >"$STAGE/Read Me.txt" <<'EOF'
MegaFlash Operator (Apple Silicon)

1. Drag MegaFlash Operator into Applications.
2. Open Operator once (network helper, Accessibility, Homebrew python3).
3. Double-click "Install MAME.command" on this disk. It runs: brew install mame
EOF

mkdir -p "$(dirname "$DMG")"
rm -f "$DMG"
hdiutil create \
  -volname "MegaFlash Operator" \
  -srcfolder "$STAGE" \
  -ov \
  -format UDZO \
  "$DMG"
echo "wrote $DMG"
ls -lh "$DMG"
