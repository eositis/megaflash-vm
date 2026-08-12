#!/usr/bin/env python3
"""Apply Operator color/scale prefs to MAME cfg + launch env.

Operator settings.json is the source of truth so a stale .app cannot
leave MEGAFLASH_A2_MONITOR unset (which previously defaulted to color).
"""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
SETTINGS = (
    Path.home()
    / "Library/Application Support/MegaFlashOperator/settings.json"
)
CFG_SRC = ROOT / "scripts/mame_cfg/apple2c4.cfg"
RUN = ROOT / ".run"
CFG_DIR = RUN / "mame_cfg"
CFG = CFG_DIR / "apple2c4.cfg"
ENV_OUT = RUN / "mame-display.env"
INI_DIR = RUN / "mame-ini"
INI = INI_DIR / "mame.ini"

MONITOR_VAL = {
    "bw": 4,
    "mono": 4,
    "b&w": 4,
    "b/w": 4,
    "green": 5,
    "amber": 6,
    "color": 0,
}


def load_prefs() -> tuple[str, int]:
    mode = ""
    scale = 0
    if SETTINGS.is_file():
        data = json.loads(SETTINGS.read_text(encoding="utf-8"))
        mode = str(data.get("colorMode") or "").strip().lower()
        try:
            scale = int(data.get("screenScale") or 0)
        except (TypeError, ValueError):
            scale = 0
        print(
            f"[mame] display from {SETTINGS}: "
            f"colorMode={data.get('colorMode')} screenScale={data.get('screenScale')}"
        )
    if not mode:
        mode = os.environ.get("MEGAFLASH_A2_MONITOR", "").strip().lower()
    if scale < 1:
        scale_s = os.environ.get("MEGAFLASH_A2_SCALE", "").strip()
        scale = int(scale_s) if scale_s.isdigit() else 0
    if not mode:
        mode = "color"
    if scale < 1:
        scale = 1
    if scale > 8:
        scale = 8
    return mode, scale


def patch_cfg(mode: str) -> None:
    val = MONITOR_VAL.get(mode, 0)
    port = (
        f'<port tag=":a2video:a2_video_config" type="CONFIG" '
        f'mask="7" defvalue="0" value="{val}" />'
    )
    CFG_DIR.mkdir(parents=True, exist_ok=True)
    text = CFG_SRC.read_text(encoding="utf-8")
    if CFG.is_file():
        # Keep last-run mixer/input from MAME, but always refresh Monitor type.
        text = CFG.read_text(encoding="utf-8")
    pat = re.compile(r'<port tag=":a2video:a2_video_config"[^>]*/>')
    if pat.search(text):
        text = pat.sub(port, text, count=1)
    elif "</input>" in text:
        text = text.replace("</input>", f"            {port}\n        </input>", 1)
    else:
        print("[mame] warning: apple2c4.cfg has no </input>; Monitor type not written", file=sys.stderr)
        CFG.write_text(text, encoding="utf-8")
        return
    CFG.write_text(text, encoding="utf-8")
    print(f"[mame] {CFG} Monitor type value={val} ({mode})")


def write_launch_files(mode: str, scale: int) -> None:
    w, h = 560 * scale, 384 * scale
    RUN.mkdir(parents=True, exist_ok=True)
    INI_DIR.mkdir(parents=True, exist_ok=True)
    ENV_OUT.write_text(
        f"export MEGAFLASH_A2_MONITOR={mode}\n"
        f"export MEGAFLASH_A2_SCALE={scale}\n"
        f"export MAME_RESOLUTION={w}x{h}\n"
        f"export MAME_INI_PATH={INI_DIR}\n"
        f"export MAME_CFG_PATH={CFG_DIR}\n",
        encoding="utf-8",
    )
    INI.write_text(
        "\n".join(
            [
                "window                    1",
                "maximize                  0",
                f"intscalex                 {scale}",
                f"intscaley                 {scale}",
                f"resolution                {w}x{h}",
                f"resolution0               {w}x{h}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    print(f"[mame] display apply: monitor={mode} scale={scale}x window={w}x{h} intscale={scale}")


def main() -> int:
    if not CFG_SRC.is_file():
        print(f"[mame] missing {CFG_SRC}", file=sys.stderr)
        return 1
    mode, scale = load_prefs()
    patch_cfg(mode)
    write_launch_files(mode, scale)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
