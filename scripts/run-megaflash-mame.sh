#!/usr/bin/env bash
# Start Bramble (MegaFlash) + MAME Apple //c (rev 4) with MegaFlash ROM and
# $C0C0-$C0C3 TCP bridge. Keep default 128K RAM (do not pass -ramsize).
#
# This script lives in megaflash-vm (outside the virtual GPIO). Bramble provides
# the Pico/RP2350 guest and -a2bus-bridge; MAME + this plugin talk over TCP.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BRAMBLE_ROOT="${BRAMBLE_ROOT:-$ROOT/../Bramble}"
# Prefer overlay binary from this repo (Apple-bus + MegaFlash hooks).
if [[ -z "${BRAMBLE:-}" ]]; then
  if [[ -x "$ROOT/bramble" ]]; then
    BRAMBLE="$ROOT/bramble"
  elif [[ -x "$ROOT/build/bramble" ]]; then
    BRAMBLE="$ROOT/build/bramble"
  else
    BRAMBLE="$BRAMBLE_ROOT/bramble"
  fi
fi
# Guest assets live in this repo (not Bramble / not the MegaFlash build tree by default).
UF2="${MEGAFLASH_UF2:-$ROOT/firmware/megaflash.uf2}"
ELF="${MEGAFLASH_ELF:-$ROOT/firmware/megaflash.elf}"
IIC_BIN="${IIC_BIN:-$ROOT/iic.bin}"
STUB="$ROOT/scripts/megaflash-mame.stub"
PORT="${BRAMBLE_A2BUS_PORT:-19765}"
PLUGINPATH="$ROOT/scripts/mame_plugins"
# Keep stock MAME plugins (boot.lua etc.) on the path; only appending our dir
# replaces the default and the plugin system may not start.
MAME_STOCK_PLUGINS="${MAME_STOCK_PLUGINS:-}"
if [[ -z "$MAME_STOCK_PLUGINS" ]]; then
  for d in \
    /opt/homebrew/share/mame/plugins \
    /opt/homebrew/Cellar/mame/*/share/mame/plugins \
    /usr/local/share/mame/plugins
  do
    # shellcheck disable=SC2086
    for dd in $d; do
      if [[ -d "$dd" && -f "$dd/boot.lua" ]]; then
        MAME_STOCK_PLUGINS="$dd"
        break 2
      fi
    done
  done
fi
if [[ -n "$MAME_STOCK_PLUGINS" ]]; then
  PLUGINPATH="$PLUGINPATH;$MAME_STOCK_PLUGINS"
fi
MAME_BIN="${MAME:-mame}"
LOCAL_ROMS="${MAME_LOCAL_ROMS:-$ROOT/roms}"
AMPLE_ROMS="${AMPLE_ROMS:-$HOME/Library/Application Support/Ample/roms}"
STAGE_DIR="$LOCAL_ROMS/apple2c4"
VOX_DIR="$LOCAL_ROMS/votrsc01a"

if [[ ! -x "$BRAMBLE" ]]; then
  BRAMBLE="$BRAMBLE_ROOT/build/bramble"
fi
if [[ ! -x "$BRAMBLE" ]]; then
  echo "overlay bramble not found" >&2
  echo "  build with: cmake -B build && make -C build bramble  (in megaflash-vm)" >&2
  echo "  or set BRAMBLE= to an overlay-built binary" >&2
  exit 1
fi
if [[ ! -f "$UF2" ]]; then
  echo "UF2 not found: $UF2" >&2
  echo "  Copy from MegaFlash build: ./scripts/sync-firmware-from-megaflash.sh" >&2
  echo "  or set MEGAFLASH_UF2=" >&2
  exit 1
fi
if [[ ! -f "$IIC_BIN" ]]; then
  echo "MegaFlash IIc ROM not found: $IIC_BIN (place iic.bin in $ROOT or set IIC_BIN)" >&2
  exit 1
fi
if ! command -v "$MAME_BIN" >/dev/null 2>&1; then
  echo "mame not found in PATH (brew install mame, or set MAME=...)" >&2
  exit 1
fi

mkdir -p "$STAGE_DIR" "$VOX_DIR"

# Stage a self-contained romset. IMPORTANT: do not leave Ample on the rompath —
# MAME prefers CRC-matching dumps and will silently ignore MegaFlash iic.bin if
# stock 3410445b.256 is also visible.
stage_from_ample() {
  local zip="$1" member="$2" dest="$3"
  if [[ -f "$dest" ]]; then
    return 0
  fi
  if [[ ! -f "$zip" ]]; then
    return 1
  fi
  unzip -o -j -qq "$zip" "$member" -d "$(dirname "$dest")"
}

APPLE2C_ZIP="$AMPLE_ROMS/apple2c.zip"
VOTR_ZIP="$AMPLE_ROMS/votrsc01a.zip"

if ! stage_from_ample "$APPLE2C_ZIP" "341-0265-a.chr" "$STAGE_DIR/341-0265-a.chr" || \
   ! stage_from_ample "$APPLE2C_ZIP" "342-0132-c.e12" "$STAGE_DIR/342-0132-c.e12"; then
  if [[ ! -f "$STAGE_DIR/341-0265-a.chr" || ! -f "$STAGE_DIR/342-0132-c.e12" ]]; then
    echo "[mame] missing CHR/keyboard dumps under $STAGE_DIR" >&2
    echo "  Need 341-0265-a.chr and 342-0132-c.e12 (from Ample apple2c.zip or your dumps)" >&2
    exit 1
  fi
fi
if ! stage_from_ample "$VOTR_ZIP" "sc01a.bin" "$VOX_DIR/sc01a.bin"; then
  if [[ ! -f "$VOX_DIR/sc01a.bin" ]]; then
    echo "[mame] missing $VOX_DIR/sc01a.bin (from Ample votrsc01a.zip)" >&2
    exit 1
  fi
fi

# Always refresh MegaFlash system ROM into the expected dump name.
cp -f "$IIC_BIN" "$STAGE_DIR/3410445b.256"
echo "[mame] staged MegaFlash ROM -> $STAGE_DIR/3410445b.256 (CRC will warn; that is expected)"

if [[ -n "${MAME_ROMPATH:-}" ]]; then
  ROMPATH="$MAME_ROMPATH"
else
  ROMPATH="$LOCAL_ROMS"
fi

# Soft preflight: required files present (do not require verifyroms OK — iic.bin CRC differs).
for f in \
  "$STAGE_DIR/3410445b.256" \
  "$STAGE_DIR/341-0265-a.chr" \
  "$STAGE_DIR/342-0132-c.e12" \
  "$VOX_DIR/sc01a.bin"
do
  if [[ ! -f "$f" ]]; then
    echo "[mame] missing required ROM file: $f" >&2
    exit 1
  fi
done

REGS_ARGS=()
if [[ -f "$ELF" ]]; then
  REGS_ARGS+=(-symbols "$ELF")
fi

BRAMBLE_PID=""
BRAMBLE_ELEVATED=0
ASKPASS="$ROOT/scripts/macos-sudo-askpass.sh"
HOST_NET_PREP="$ROOT/scripts/macos-host-net-prep.sh"

# Kill may need sudo when Bramble was started elevated for utun.
kill_bramble() {
  local pid="$1"
  if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
    return 0
  fi
  if kill "$pid" 2>/dev/null; then
    wait "$pid" 2>/dev/null || true
    return 0
  fi
  if [[ "$BRAMBLE_ELEVATED" -eq 1 ]] && [[ -x "$ASKPASS" ]]; then
    SUDO_ASKPASS="$ASKPASS" sudo -A kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
}

cleanup() {
  kill_bramble "$BRAMBLE_PID"
  BRAMBLE_PID=""
}
trap cleanup EXIT INT TERM

# SPI backing files live in this project (outside the GPIO).
FLASH_DIR="${MEGAFLASH_FLASH_DIR:-$ROOT/flash}"
mkdir -p "$FLASH_DIR"
SPI_FLASH1="${SPI_FLASH1:-$FLASH_DIR/spi-flash1.bin}"
SPI_FLASH2="${SPI_FLASH2:-$FLASH_DIR/spi-flash2.bin}"

SPI_FLASH_ARGS=()
if [[ -z "${NO_SPI_FLASH:-}" ]]; then
  SPI_FLASH_ARGS+=(-spi-flash1 "$SPI_FLASH1" -spi-flash2 "$SPI_FLASH2")
  if [[ ! -f "$SPI_FLASH1" ]] || ! python3 - "$SPI_FLASH1" <<'PY'
import sys
with open(sys.argv[1], "rb") as f:
    sample = f.read(65536)
sys.exit(0 if any(b not in (0, 0xFF) for b in sample) else 1)
PY
  then
    echo "[mame] WARNING: $SPI_FLASH1 missing or empty — SmartPort boot needs a populated volume in $FLASH_DIR" >&2
    echo "  (upload via USB/XMODEM, or restore spi-flash1.bin / A2DeskTop.hdv contents)" >&2
  fi
fi

# MegaFlash Test Wifi / NTP use real CYW43 + hostif (-wifi -tap): guest lwIP
# DHCP/DNS/NTP/TFTP through the radio. Pass NO_WIFI=1 to disable radio; NO_HOST_NET=1
# for -wifi only (no utun/pf); BRAMBLE_A2BUS_STUB_WIFI=1 to stub cyw43_arch_init.
WIFI_ARGS=()
HOST_NET=0
if [[ -z "${NO_WIFI:-}" ]]; then
  WIFI_ARGS+=(-wifi)
  if [[ -z "${NO_HOST_NET:-}" ]]; then
    WIFI_ARGS+=(-tap "${BRAMBLE_TAP_NAME:-bramble0}")
    HOST_NET=1
  fi
fi

run_bramble() {
  # cwd = this repo so relative flash/megaflash-user-config.bin resolves here.
  (
    cd "$ROOT"
    exec env BRAMBLE_ESCALATED="${BRAMBLE_ESCALATED:-}" \
      "$BRAMBLE" "$UF2" \
      -arch m33 \
      -clock 150 \
      -cores 2 \
      -a2bus-bridge "$PORT" \
      -script "$STUB" \
      "${SPI_FLASH_ARGS[@]}" \
      "${WIFI_ARGS[@]}" \
      ${TIMEOUT:+-timeout "$TIMEOUT"} \
      "${REGS_ARGS[@]}" \
      "$@"
  )
}

echo "[mame] Bramble=$BRAMBLE  stub=$STUB  port=$PORT"
echo "[mame] flash1=$SPI_FLASH1"
if [[ "$HOST_NET" -eq 1 ]]; then
  echo "[mame] host network: -wifi -tap (utun + pf NAT for 192.168.4.0/24)"
fi
echo "[mame] starting Bramble MegaFlash bridge on 127.0.0.1:$PORT"

if [[ "$HOST_NET" -eq 1 ]] && [[ "$(uname -s)" == "Darwin" ]]; then
  NET_HELPER="${MEGAFLASH_NET_HELPER:-/usr/local/libexec/megaflash-net-helper.sh}"
  USE_PASSWORDLESS=0
  if [[ -x "$NET_HELPER" ]] && sudo -n "$NET_HELPER" status >/dev/null 2>&1; then
    USE_PASSWORDLESS=1
    echo "[mame] using passwordless network helper: $NET_HELPER"
    sudo -n "$NET_HELPER" enable || true
  fi

  RUN_DIR="$ROOT/.run"
  mkdir -p "$RUN_DIR"
  RUNNER="$RUN_DIR/start-bramble-hostnet.sh"
  {
    echo '#!/usr/bin/env bash'
    echo 'set -euo pipefail'
    echo "export BRAMBLE_ESCALATED=1"
    echo "export BRAMBLE_ROOT=$(printf '%q' "$BRAMBLE_ROOT")"
    echo "export PATH=$(printf '%q' "$PATH")"
    if [[ "$USE_PASSWORDLESS" -eq 0 ]]; then
      echo "$(printf '%q' "$HOST_NET_PREP") enable || true"
    fi
    echo "cd $(printf '%q' "$ROOT")"
    printf 'exec'
    printf ' %q' "$BRAMBLE" "$UF2" \
      -arch m33 -clock 150 -cores 2 \
      -a2bus-bridge "$PORT" \
      -script "$STUB"
    if ((${#SPI_FLASH_ARGS[@]})); then printf ' %q' "${SPI_FLASH_ARGS[@]}"; fi
    if ((${#WIFI_ARGS[@]})); then printf ' %q' "${WIFI_ARGS[@]}"; fi
    if [[ -n "${TIMEOUT:-}" ]]; then printf ' %q' -timeout "$TIMEOUT"; fi
    if ((${#REGS_ARGS[@]})); then printf ' %q' "${REGS_ARGS[@]}"; fi
    if (($#)); then printf ' %q' "$@"; fi
    echo
  } >"$RUNNER"
  chmod +x "$RUNNER"

  if [[ "$USE_PASSWORDLESS" -eq 1 ]]; then
    # Elevate Bramble for utun without interactive askpass when possible.
    if sudo -n true 2>/dev/null; then
      sudo -n -E "$RUNNER" &
      BRAMBLE_PID=$!
      BRAMBLE_ELEVATED=1
    else
      USE_PASSWORDLESS=0
    fi
  fi

  if [[ "$USE_PASSWORDLESS" -eq 0 ]] || [[ -z "${BRAMBLE_PID:-}" ]]; then
    if [[ ! -f "$ASKPASS" ]]; then
      echo "askpass helper missing: $ASKPASS" >&2
      exit 1
    fi
    chmod +x "$ASKPASS" "$HOST_NET_PREP" 2>/dev/null || true
    export SUDO_ASKPASS="$ASKPASS"
    export SUDO_ASKPASS_PROMPT="MegaFlash needs administrator access to create a utun interface and enable pf NAT so guest Wi‑Fi (192.168.4.0/24) can reach DNS/NTP on the internet."
    echo "[mame] requesting macOS admin approval for utun + pf NAT (dialog)…"
    sudo -A -E "$RUNNER" &
    BRAMBLE_PID=$!
    BRAMBLE_ELEVATED=1
  fi
elif [[ "$HOST_NET" -eq 1 ]]; then
  # Linux: Bramble sudo-reexeces for TAP; iptables/nft is separate.
  echo "[mame] host network: Bramble may prompt for sudo to create TAP"
  run_bramble "$@" &
  BRAMBLE_PID=$!
else
  run_bramble "$@" &
  BRAMBLE_PID=$!
fi

python3 - "$PORT" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
deadline = time.time() + 90
last_err = None
sock = None

def connect():
    global sock, last_err
    if sock is not None:
        return sock
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=2)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sock
    except OSError as e:
        last_err = e
        sock = None
        return None

def rpc(op, *payload):
    global sock, last_err
    s = connect()
    if s is None:
        raise OSError(last_err or "connect failed")
    try:
        s.sendall(bytes((op,) + payload))
        rsp = s.recv(2)
    except OSError as e:
        last_err = e
        try:
            s.close()
        except OSError:
            pass
        sock = None
        raise
    if len(rsp) != 2 or rsp[0] != 0:
        raise OSError(f"bad rsp {rsp!r}")
    return rsp[1]

while time.time() < deadline:
    try:
        pong = rpc(0x00)
        print(f"[mame] bridge PING ok (data=0x{pong:02x})")
        break
    except OSError as e:
        last_err = e
        time.sleep(0.2)
else:
    print(f"[mame] bridge not ready: {last_err}", file=sys.stderr)
    sys.exit(1)

while time.time() < deadline:
    try:
        r2 = rpc(0x04, 2)
        if r2 == 0xF0:
            print("[mame] MegaFlash BusLoopSlinky ready (registers[2]=0xf0)")
            if sock is not None:
                sock.close()
            sys.exit(0)
    except OSError as e:
        last_err = e
    time.sleep(0.3)
print(f"[mame] firmware bus loop not ready: {last_err}", file=sys.stderr)
sys.exit(1)
PY

export BRAMBLE_A2BUS_PORT="$PORT"
export BRAMBLE_IIC_BIN="$IIC_BIN"

echo "[mame] launching apple2c4 (rompath=$ROMPATH; MegaFlash maincpu staged)"
echo "[mame] expect WRONG CHECKSUM warning for 3410445b.256 — MegaFlash ROM, not stock"
echo "[mame] pluginspath=$PLUGINPATH"
# Do not exec: the EXIT trap must kill Bramble when MAME exits.
set +e
# scripts/mame_cfg: Open-Apple = Left Option or Left ⌘ (physical left solid-apple).
# Optional extras from MegaFlash Operator (resolution / B&W tweaks).
# shellcheck disable=SC2206
MAME_EXTRA=( ${MAME_EXTRA_ARGS:-} )
"$MAME_BIN" apple2c4 \
  -rompath "$ROMPATH" \
  -cfg_directory "$ROOT/scripts/mame_cfg" \
  -pluginspath "$PLUGINPATH" \
  -plugins \
  -plugin megaflash_bridge \
  -skip_gameinfo \
  -window \
  "${MAME_EXTRA[@]}"
mame_rc=$?
set -e
exit "$mame_rc"
