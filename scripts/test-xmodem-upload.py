#!/usr/bin/env python3
"""Drive MegaFlash USB menu and XMODEM-CRC upload over the Bramble PTY (stdlib only)."""

from __future__ import annotations

import fcntl
import os
import select
import sys
import termios
import time
import tty

PTY = os.environ.get("USB_CONSOLE_PTY_PATH", "/tmp/bramble-usb-console")
IMAGE = sys.argv[1] if len(sys.argv) > 1 else "A2OSX.STABLE.32MB.po"
MAX_BYTES = int(os.environ.get("XMODEM_TEST_BYTES", "0"))
ACK_TIMEOUT = float(os.environ.get("XMODEM_ACK_TIMEOUT", "120"))


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


class Pty:
    def __init__(self, path: str) -> None:
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        self.old = termios.tcgetattr(self.fd)
        raw = termios.tcgetattr(self.fd)
        raw[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK |
                    termios.ISTRIP | termios.INLCR | termios.IGNCR |
                    termios.ICRNL | termios.IXON)
        raw[1] &= ~termios.OPOST
        raw[3] &= ~(termios.ICANON | termios.ECHO | termios.ECHOE |
                    termios.ISIG | termios.IEXTEN)
        raw[6][termios.VMIN] = 0
        raw[6][termios.VTIME] = 1
        termios.tcsetattr(self.fd, termios.TCSANOW, raw)
        fl = fcntl.fcntl(self.fd, fcntl.F_GETFL)
        fcntl.fcntl(self.fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

    def close(self) -> None:
        termios.tcsetattr(self.fd, termios.TCSANOW, self.old)
        os.close(self.fd)

    def read_until(self, needle: bytes, timeout: float) -> bytes:
        buf = b""
        deadline = time.time() + timeout
        while needle not in buf and time.time() < deadline:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    buf += os.read(self.fd, 4096)
                except BlockingIOError:
                    pass
        return buf

    def write(self, data: bytes) -> None:
        off = 0
        while off < len(data):
            try:
                n = os.write(self.fd, data[off:])
            except BlockingIOError:
                _, w, _ = select.select([], [self.fd], [], 0.5)
                if not w:
                    raise RuntimeError(f"PTY write stalled at {off}/{len(data)}")
                continue
            if n <= 0:
                raise RuntimeError(f"PTY write stalled at {off}/{len(data)}")
            off += n

    def read1(self, timeout: float = 1.0) -> bytes:
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError:
            return b""


def read_ack(pty: Pty, timeout: float = 30.0) -> int:
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = pty.read1(0.5)
        for byte in b:
            if byte in (0x06, 0x15):
                return byte
    return -1


def drain_pty(pty: Pty, timeout: float = 0.15) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not pty.read1(0.02):
            break


def send_xmodem_crc(pty: Pty, path: str, max_bytes: int) -> int:
    with open(path, "rb") as f:
        payload = f.read() if max_bytes <= 0 else f.read(max_bytes)

    print(f"Sending {len(payload)} bytes from {path}", flush=True)
    deadline = time.time() + 90.0
    while time.time() < deadline:
        b = pty.read1(0.5)
        if b"C" in b:
            break
    else:
        raise TimeoutError("Timed out waiting for receiver 'C'")
    drain_pty(pty)

    block = 1
    offset = 0
    sent = 0

    while offset < len(payload):
        size = 1024
        chunk = payload[offset : offset + size]
        if len(chunk) < size:
            size = 128
            chunk = payload[offset : offset + size]
            if len(chunk) < size:
                chunk = chunk + bytes([0x1A] * (size - len(chunk)))
        header = 0x02 if size == 1024 else 0x01
        body = bytes([block & 0xFF, (~block) & 0xFF]) + chunk
        csum = crc16_xmodem(chunk)
        frame = bytes([header]) + body + bytes([(csum >> 8) & 0xFF, csum & 0xFF])
        pty.write(frame)
        ack = read_ack(pty, timeout=ACK_TIMEOUT)
        if ack != 0x06:
            raise RuntimeError(f"NAK/timeout on block {block}: got {ack!r}")
        offset += size
        sent += size
        block = (block + 1) & 0xFF
        if block % 16 == 0 or (block & 0xFF) == 0:
            print(f"  block {(block - 1) & 0xFF}, {offset}/{len(payload)}", flush=True)

    pty.write(b"\x04")
    ack = read_ack(pty)
    if ack != 0x06:
        print(f"Warning: final ACK was {ack!r}", flush=True)
    return sent


def main() -> int:
    if not os.path.exists(IMAGE):
        print(f"Image not found: {IMAGE}", file=sys.stderr)
        return 1
    if not os.path.lexists(PTY):
        print(f"PTY not found: {PTY}", file=sys.stderr)
        return 1

    pty = Pty(PTY)
    try:
        time.sleep(0.5)
        pty.read_until(b"Please Select:", timeout=60.0)
        pty.write(b"2")
        pty.read_until(b"Please enter drive number", timeout=30.0)
        pty.write(b"1")
        pty.read_until(b"Type CONFIRM to proceed", timeout=30.0)
        pty.write(b"CONFIRM\n")
        pty.read_until(b"Please start upload", timeout=30.0)
        sent = send_xmodem_crc(pty, IMAGE, MAX_BYTES)
        tail = pty.read_until(b"blocks received", timeout=7200.0)
        print(tail.decode("utf-8", errors="replace"), flush=True)
        print(f"Done ({sent} bytes sent)", flush=True)
        if MAX_BYTES <= 0:
            expected = os.path.getsize(IMAGE)
            with open(f"flash/spi-flash1.bin", "rb") as f:
                got = f.read(expected)
            with open(IMAGE, "rb") as f:
                ref = f.read(expected)
            if got != ref:
                print(f"VERIFY FAIL: flash {len(got)} bytes != image {len(ref)}", file=sys.stderr)
                return 1
            print(f"VERIFY OK: {len(got)} bytes match {IMAGE}", flush=True)
    finally:
        pty.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
