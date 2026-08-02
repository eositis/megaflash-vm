# Using tio with the MegaFlash USB console

[tio](https://github.com/tio/tio) is a simple serial console for the Bramble PTY. One session covers the MegaFlash menu and built-in XMODEM send.

Install if needed:

```bash
brew install tio
```

Only **one** program may open `/tmp/bramble-usb-console` at a time. Quit tio before starting minicom, `sz`, or `./scripts/test-32mb-xmodem.sh`.

## 1. Start the emulator

**Terminal 1:**

```bash
cd /path/to/megaflash-vm
CORES=1 TIMEOUT=7200 ./scripts/run-megaflash-usb-console.sh
```

Wait until stderr shows something like:

```text
[USB] CDC console symlink: /tmp/bramble-usb-console -> /dev/ttysNNN
```

Leave this terminal running for the whole session.

## 2. Attach tio

**Terminal 2:**

```bash
tio /tmp/bramble-usb-console
```

Baud is unused for USB CDC; tio’s default is fine. You should see the MegaFlash banner and:

```text
Please Select:
```

If the PTY is missing, Bramble is not running yet (or exited). If the screen is blank, confirm only one client is attached and that Bramble logged `guest reached UserTerminal`.

Prefix key for tio commands is **Ctrl-T**. Press **Ctrl-T** then **?** for the in-session help list.

## 3. Trigger an XMODEM upload (menu item 2)

In the MegaFlash menu (normal typing, not Ctrl-T):

1. Press **`2`** — Upload ProDOS Image file to MegaFlash  
2. Press **`1`** — drive / unit (SPI flash unit 1 → `flash/spi-flash1.bin`)  
3. When prompted, type **`CONFIRM`** and press Enter  
4. Wait until MegaFlash prints that upload can start and sends **`C`** repeatedly (XMODEM-CRC receiver ready)

Then start the file send from tio:

1. Press **Ctrl-T**, then **`x`**  
2. Choose **XMODEM-1K** if offered (preferred for large `.po` files; XMODEM-CRC is 128-byte blocks)  
3. Pick / enter the path to your disk image (e.g. `A2OSX.STABLE.32MB.po`)

Progress is shown in tio. A full ~32MB image can take on the order of **1–2 minutes** under emulation. When finished, MegaFlash should report blocks received and return to the menu.

Uploaded data persists in the SPI backing file (default `flash/spi-flash1.bin`) across Bramble restarts when using the usual `-spi-flash1` run script.

## 4. Exit tio

Press **Ctrl-T**, then **`q`**.

That quits tio only. Bramble in Terminal 1 keeps running until you stop it (Ctrl-C) or it hits `-timeout`.

## Tips and limits

| Topic | Detail |
|-------|--------|
| Long timeout | Use `TIMEOUT=7200` (or `0`) so Bramble does not exit mid-upload |
| One core | `CORES=1` reduces load during XMODEM |
| ACK timing | tio’s XMODEM sender waits ~1s per ACK. Emulated flash can be slower; if you see `?` / `N` / send failures on a large image, use `./scripts/test-32mb-xmodem.sh` or minicom/`sz` (see [USB-CONSOLE.md](USB-CONSOLE.md)) |
| Restart cleanly | Quit tio first, then restart Bramble, then attach tio again |

## Related

- Full USB/PTY notes and other clients: [USB-CONSOLE.md](USB-CONSOLE.md)  
- Automated upload + verify: `./scripts/test-32mb-xmodem.sh`
