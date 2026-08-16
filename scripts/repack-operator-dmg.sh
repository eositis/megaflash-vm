#!/usr/bin/env bash
# Rebuild the Operator DMG so Finder shows the MAME 0.288 installer
# next to the .app (Tauri's DMG is app + Applications only).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE="$ROOT/app/src-tauri/target/release/bundle"
APP="$BUNDLE/macos/MegaFlash Operator.app"
DMG="$BUNDLE/dmg/MegaFlash Operator_0.1.0_aarch64.dmg"
CMD="$ROOT/scripts/Install MAME 0.288.command"

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
cp -p "$CMD" "$STAGE/Install MAME 0.288.command"
chmod 755 "$STAGE/Install MAME 0.288.command"
cat >"$STAGE/Read Me.txt" <<'EOF'
MegaFlash Operator (Apple Silicon)

1. Drag MegaFlash Operator into Applications.
2. Open Operator once (network helper, Accessibility, Homebrew python3).
3. Double-click "Install MAME 0.288.command" on this disk (needs Homebrew from step 2).

Do not run "brew install mame" — Homebrew currently ships 0.289.
This installer extracts MAME 0.288, the build Operator is tested with.
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
