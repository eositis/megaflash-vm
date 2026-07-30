#!/usr/bin/env python3
"""Probe MegaFlash GETUSERSETTINGS / SAVEUSERSETTINGS / READBLOCK over a2bus."""
import argparse
import socket
import sys
import time

OPS = dict(PING=0x00, PHI=0x01, READ=0x02, WRITE=0x03, PEEK=0x04)


def connect(port: int) -> socket.socket:
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def rpc(s: socket.socket, op: int, *payload: int) -> int:
    s.sendall(bytes((op,) + payload))
    rsp = s.recv(2)
    if len(rsp) != 2 or rsp[0] != 0:
        raise OSError(f"bad rsp {rsp!r} for op={op:#x}")
    return rsp[1]


def wait_idle(s: socket.socket, timeout: float = 5.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = rpc(s, OPS["PEEK"], 0)
        if (st & 0x80) == 0:
            return
        time.sleep(0.001)
    raise TimeoutError("STATUS still BUSY")


def cmd(s: socket.socket, c: int, idle_timeout: float = 5.0) -> None:
    rpc(s, OPS["WRITE"], 0, c & 0xFF)
    wait_idle(s, timeout=idle_timeout)


def activate(s: socket.socket) -> None:
    rpc(s, OPS["READ"], 2)
    rpc(s, OPS["READ"], 0)
    rpc(s, OPS["READ"], 0)
    rpc(s, OPS["READ"], 3)
    rpc(s, OPS["READ"], 1)
    for _ in range(20):
        v = rpc(s, OPS["PEEK"], 3)
        if v in (0x96, 0x69):
            print(f"activated id={v:#x}")
            return
        time.sleep(0.05)
    print(f"WARN id still {rpc(s, OPS['PEEK'], 3):#x}")


def read_data(s: socket.socket, n: int) -> bytes:
    out = bytearray()
    for _ in range(n):
        out.append(rpc(s, OPS["READ"], 2))
    return bytes(out)


def write_param(s: socket.socket, *vals: int) -> None:
    for v in vals:
        rpc(s, OPS["WRITE"], 1, v & 0xFF)


def write_data(s: socket.socket, payload: bytes) -> None:
    for b in payload:
        rpc(s, OPS["WRITE"], 2, b)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19765)
    args = ap.parse_args()
    s = connect(args.port)
    print(f"PING={rpc(s, OPS['PING']):#x}")
    activate(s)

    cmd(s, 0x03)  # MODELINEAR
    cmd(s, 0x21)  # GETUSERSETTINGS
    raw = read_data(s, 7)
    print("GETUSERSETTINGS:", raw.hex(" "), list(raw))
    ver, chk, c1, c2, tzv, tzi, fd = raw
    ok = ver == 2 and chk == (2 ^ 0x5A) and tzv == 1
    print(f"validate={'OK' if ok else 'FAIL'} checkbyte={chk:#x} expect={2 ^ 0x5A:#x}")

    # SAVEUSERSETTINGS with FPU+NTP bits (as in CP UI)
    cmd(s, 0x00)
    write_param(s, 0x71)  # WE_KEY
    new_c1 = c1 | 0x18  # NTPCLIENTFLAG|FPUFLAG
    payload = bytes([2, 2 ^ 0x5A, new_c1, c2, 1, tzi, fd])
    cmd(s, 0x03)  # MODELINEAR
    write_data(s, payload)
    t0 = time.time()
    cmd(s, 0x20)  # SAVEUSERSETTINGS
    elapsed = time.time() - t0
    st = rpc(s, OPS["PEEK"], 0)
    print(f"SAVEUSERSETTINGS done in {elapsed:.3f}s status={st:#x}")

    cmd(s, 0x21)
    raw2 = read_data(s, 7)
    print("GET after SAVE:", raw2.hex(" "), list(raw2))
    save_ok = raw2[2] == new_c1 and raw2[4] == 1 and elapsed < 2.0 and (st & 0x80) == 0
    print(f"save_roundtrip={'OK' if save_ok else 'FAIL'}")

    cmd(s, 0x00)
    write_param(s, 1, 0, 0, 0)
    cmd(s, 0x15)
    cmd(s, 0x04)
    head = read_data(s, 8)
    print("READBLOCK0 interleaved head:", head.hex(" "))
    expect = bytes([0x01, 0x91, 0x38, 0x60, 0xB0, 0xC8, 0x03, 0xD0])
    blk_ok = head == expect
    print(f"block0 match={'OK' if blk_ok else 'FAIL'}")

    # TESTWIFI (0x09): empty SSID → NETERR_SSIDNOTSET(3) via a2bus fast-fail
    # (avoids C++ throw/EH hang). Configured SSID uses real CYW43 join.
    cmd(s, 0x00)
    write_param(s, 0x71)
    t0 = time.time()
    tw = -1
    st = 0x80
    try:
        cmd(s, 0x09, idle_timeout=30.0)
        elapsed = time.time() - t0
        tw = rpc(s, OPS["READ"], 1)
        st = rpc(s, OPS["PEEK"], 0)
    except TimeoutError:
        elapsed = time.time() - t0
        try:
            st = rpc(s, OPS["PEEK"], 0)
            tw = rpc(s, OPS["READ"], 1)
        except OSError:
            pass
        print(f"TESTWIFI still BUSY after {elapsed:.3f}s (cyw43 bring-up WIP)")
    else:
        print(f"TESTWIFI err={tw} status={st:#x} in {elapsed:.3f}s")
    # Empty SSID: SSIDNOTSET(3). Non-empty: join/DHCP against Bramble -wifi.
    wifi_ok = tw == 3 and (st & 0x80) == 0
    print(f"testwifi={'OK' if wifi_ok else 'WIP'} "
          f"(err={tw}; expect 3=SSIDNOTSET when SSID empty)")

    return 0 if (ok and save_ok and blk_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
