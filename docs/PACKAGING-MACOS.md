# macOS packaging feasibility (Operator + Bramble + MAME)

Planning note (2026-08-15). Not implemented. Goal: install MegaFlash Operator on a **second Mac** without a GitHub checkout, and still run USB console + Apple //c.

## Verdict

| Piece | Ship in installer? | Notes |
|-------|-------------------|--------|
| Operator `.app` | Yes | Already produced by `npm run tauri build` (aarch64 DMG). Sandbox is **off** (needs spawn, TCP, Accessibility, sudo helper). |
| Overlay `bramble` | Yes | ~742 KiB, links only `libSystem`. Copy next to Resources. |
| Scripts, Lua plugin, stub, UF2, `iic.bin` | Yes | Required. Packaged `MEGAFLASH_VM_ROOT` must point at these, not `~/Documents/GitHub/…`. |
| Bramble `macos-cyw43-pf-nat.sh` | Yes (copy in) | Helper install currently requires sibling `../Bramble`. |
| SPI flash images | **Yes (demo volumes)** | Staged from the build machine Operator profile (`spi-flash*.bin` + user config). |
| **MAME binary + Homebrew dylibs** | **No (not as default)** | See below. First-run **download/install** is the practical path. |
| Apple CHR / keyboard / Votrax dumps | **No** | Copyrighted. Keep current Ample-or-pick-files flow. |
| Homebrew itself | **Yes (first launch)** | Official installer + `brew install python3` if no runnable python3. Admin dialog. |

A **single DMG of Operator + overlay runtime** is feasible. A **single legal installer that also contains MAME + Apple ROMs** is not.

## What the app assumes today

Packaged Operator still behaves like a **dev tree**:

- `default_vm_root()` walks for `scripts/run-megaflash-mame.sh`, else `~/Documents/GitHub/megaflash-vm`.
- Overlay bramble is `root/bramble` or `../Bramble/build/bramble`.
- MAME is `/opt/homebrew/bin/mame` (plus PATH prepend for GUI apps).
- Stock plugins: `/opt/homebrew/share/mame/plugins` (`boot.lua`).
- Companion dumps staged from Ample `~/Library/Application Support/Ample/roms`.
- Network helper copies Bramble’s pf script from `../Bramble/scripts/macos-cyw43-pf-nat.sh`.
- XMODEM uses `python3` (`/usr/bin/python3` is enough).
- Accessibility for docking the SDL window; admin once for pf/utun helper.

None of that works on a clean Mac if you only copy the current `.app`.

## MAME: why not bundle by default

Homebrew `mame` **0.288** on this machine is **~491 MiB** in Cellar and is **not relocatable**: `otool -L` shows Homebrew dylibs (`libSDL3`, `pugixml`, `jpeg-turbo`, `zstd`, `flac`, `utf8proc`, `sqlite`, `portaudio`, `portmidi`). Copying `/opt/homebrew/bin/mame` onto another Mac fails without those kegs.

MAME is **GPL**. Redistributing a binary is allowed if you also offer corresponding source, but notarizing and shipping a half-gigabyte third-party emulator inside Operator is a poor fit (size, updates, Apple Silicon vs Intel, SDL3 ABI).

**Do not** rely on “just zip MAME into Resources” unless you invest in `dylibbundler` / a relocatable MAME build and a GPL source offer. That is a later phase, not v1.

## Recommended product shape

```
MegaFlash Operator.app
  Contents/MacOS/megaflash-operator
  Contents/Resources/runtime/
    bramble                  # overlay
    iic.bin
    firmware/megaflash.uf2
    scripts/run-megaflash-mame.sh
    scripts/mame_plugins/…
    scripts/megaflash-mame.stub
    scripts/test-xmodem-upload.py
    scripts/macos-cyw43-pf-nat.sh   # vendored copy
    scripts/macos-sudo-askpass.sh
    roms/                    # empty dirs; user-supplied dumps
    flash/                   # demo spi-flash*.bin from build profile

~/Library/Application Support/MegaFlashOperator/
  settings.json
  mame/                      # downloaded or linked mame + plugins
  roms/                      # staged CHR/keyboard/Votrax
  flash/                     # writable SPI images (if not in-bundle)
```

Installer = **signed DMG** (already built) that copies the `.app` to `/Applications`. The DMG also contains **Install MAME.command** (`brew install mame`). First launch:

1. Create Application Support dirs; if `megaflash_vm_root` is still the GitHub path, retarget to `Contents/Resources/runtime`.
2. **MAME check** (in order): env `MAME`, bundled relocatable mame (if we add it later), `/opt/homebrew/bin/mame`, `/usr/local/bin/mame`, `~/Library/Application Support/MegaFlashOperator/mame/mame`, Ample’s embedded mame if present.
3. If MAME is missing: double-click **Install MAME.command** (`brew install mame`).
4. **ROM check**: if `roms/apple2c4` lacks CHR/keyboard, offer *Copy from Ample* or *Choose files*. Never download Apple dumps.
5. First launch: admin dialog for tun/pf helper; one Accessibility TCC prompt and Privacy settings.

### Download MAME (convenient location)

Prefer **user-owned** `~/Library/Application Support/MegaFlashOperator/mame/`:

- If Homebrew exists: **Install MAME.command** runs `brew install mame`.
- Else: fetch a **macOS arm64/x64 MAME binary** from a pinned [MAMEdev](https://github.com/mamedev/mame/releases) asset (or a mirror you control), unpack there, and set `MAME_STOCK_PLUGINS` to that tree’s `plugins/` (must contain `boot.lua`). Show license/GPL notice in the dialog.
- Fallback: file picker for an already-installed `mame`.

Pin a **tested version** (e.g. 0.288, same as this host). Lua plugins and `apple2c4` are stable across nearby versions, but SDL/plugin paths move.

### Architecture

Current `tauri build` produced **aarch64** only. A second Intel Mac needs a separate build or a universal Operator + universal/overlay `bramble`. Treat as a second artifact, not an afterthought.

## Work to make packaging real (phased)

**P0 — packaged runtime (implemented 2026-08-15)**  
- `scripts/stage-operator-runtime.sh` copies overlay bramble, UF2, `iic.bin`, launch scripts, Lua plugin, vendored `macos-cyw43-pf-nat.sh` into `app/src-tauri/runtime/` (gitignored).  
- Tauri `bundle.resources` embeds that tree; packaged `default_vm_root()` is `Contents/Resources/runtime`.  
- Writable flash/roms/`.run` live under `~/Library/Application Support/MegaFlashOperator/` (demo `spi-flash*.bin` copied from the bundle on first launch if dest is empty).  
- Network helper install uses `runtime/scripts/macos-cyw43-pf-nat.sh` (no `../Bramble`).  
- `MAME_STOCK_PLUGINS` also searches Application Support `mame/plugins`.  
- Notarize still needs an Apple Developer ID (not done in P0).

**P1 — first-launch install (implemented 2026-08-15)**  
- Packaged (or incomplete) launch: admin dialog for tun/pf helper; one Accessibility TCC prompt + open Privacy settings.  
- If `python3` is missing or is only the Xcode stub: install **Homebrew** (admin) then `brew install python3` (`scripts/macos-ensure-homebrew-python.sh`). Launch //c’s a2bus wait needs a real interpreter.  
- MAME via **`brew install mame`** (current Homebrew formula). Existing `mame` on PATH is used.

**P2 — optional relocatable MAME**  
- Only if download-from-GitHub is too flaky: `dylibbundler` a Homebrew mame into `runtime/mame/` plus GPL `COPYING` and source URL. Size ~0.5 GiB.

**P3 — installer polish**  
- DMG layout (Applications symlink + **Install MAME.command**). Optional `.pkg` is unnecessary if P0+P1 are solid.  
- Demo SPI volumes are already staged from the build profile (not empty templates).

## Dependencies that stay out of band

| Need | How the other Mac gets it |
|------|---------------------------|
| Apple CHR / keyboard / Votrax | Ample or user’s dumps (unchanged) |
| Accessibility | System Settings, once |
| pf NAT helper | In-app install, once |
| Python 3 | First-launch Homebrew `python3` (not the `/usr/bin/python3` CLT stub) |
| Node/Rust | **Build machine only**, not the target |

## Risk summary

- **Legal:** MegaFlash `iic.bin` and firmware are yours to ship; Apple dump files are not; MAME is GPL (offer source if you ever embed the binary).  
- **Gatekeeper:** unsigned `.app` from a DMG will be blocked on a stranger’s Mac until notarized.  
- **Wi‑Fi TAP:** still needs the helper; utun may prompt. Pico USB console does not.  
- **A2Stream / W5100:** unrelated to install; still host-bridge quality, not a packaging blocker.
