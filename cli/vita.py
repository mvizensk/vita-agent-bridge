#!/usr/bin/env python3
"""Drive a PS Vita running vita-agent-bridge: look at the screen, press things.

    export VITA_HOST=192.168.1.28          # or pass --host
    vita.py status                          # version, foreground app, screen size
    vita.py shot screen.png [--half]        # both planes; .app/.shell written too
    vita.py press cross                     # one button
    vita.py keys down down cross            # a sequence
    vita.py hold up,cross 600               # hold a mask for N ms
    vita.py stick 0 128 128 128 500         # analog: lx ly rx ry ms
    vita.py tap 480 272 [ms]                # touch, screen pixels (960x544)
    vita.py swipe 880 110 200 500 800       # drag
    vita.py launch TITLEID                  # psgm: launch
    vita.py kill-fg                         # close the foreground app
    vita.py awake 0|1                       # hold the screen on
    vita.py ls ux0:data                     # read-only listing
    vita.py get ux0:path/file [local]       # read-only download
    vita.py df ux0:                         # free space
    vita.py reboot                          # cold reset (works with a wedged shell)

The token is the 64 hex characters in ur0:data/vita-agent-bridge/token.txt on the
console. Point VITA_TOKEN_FILE at your local copy (default ~/.vita-agent-token),
or set VITA_TOKEN. Plaintext over TCP: trusted LAN only, never port-forwarded.
"""
import argparse
import os
import socket
import struct
import sys
import zlib
from pathlib import Path

PORT = 1348
BUTTONS = {
    "select": 0x1, "l3": 0x2, "r3": 0x4, "start": 0x8, "up": 0x10, "right": 0x20,
    "down": 0x40, "left": 0x80, "l": 0x100, "l2": 0x100, "r": 0x200, "r2": 0x200,
    "l1": 0x400, "r1": 0x800, "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000,
    "square": 0x8000, "ps": 0x10000, "volup": 0x100000, "voldown": 0x200000,
}


def token():
    if os.environ.get("VITA_TOKEN"):
        return os.environ["VITA_TOKEN"].strip()
    path = Path(os.environ.get("VITA_TOKEN_FILE", Path.home() / ".vita-agent-token"))
    try:
        value = path.read_text().strip()
    except OSError:
        raise SystemExit("No token: set VITA_TOKEN, or put it in %s" % path)
    if len(value) != 64:
        raise SystemExit("Token in %s must be exactly 64 characters" % path)
    return value


def connect(host, command, timeout=15):
    conn = socket.create_connection((host, PORT), timeout=timeout)
    conn.sendall(("AUTH " + token() + " " + command + "\n").encode())
    return conn


def line(host, command):
    with connect(host, command) as c:
        return c.makefile("rb").readline(512).decode(errors="replace").strip()


def write_png(path, width, height, rgba):
    raw = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(tag, data):
        body = tag + data
        return struct.pack("!I", len(data)) + body + struct.pack("!I", zlib.crc32(body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    Path(path).write_bytes(png)


def shot(host, out, half):
    """Two planes come back: the app's, then the shell's. Composite them."""
    with connect(host, "shot half" if half else "shot", timeout=60) as c:
        f = c.makefile("rb")
        planes = []
        for _ in range(2):
            head = f.read(24)
            if len(head) < 24:
                break
            magic, w, h, pitch, fmt, size = struct.unpack("<4sIIIII", head)
            if magic != b"VSHT" or not size:
                planes.append(None)
                continue
            planes.append((w, h, f.read(size)))
    app, shell = (planes + [None, None])[:2]
    base = app or shell
    if not base:
        raise SystemExit("no pixels came back")
    w, h, pixels = base
    if app and shell and app[:2] == shell[:2]:       # shell overlays the app
        merged = bytearray(app[2])
        over = shell[2]
        for i in range(0, len(merged), 4):
            alpha = over[i + 3]
            if alpha:
                for ch in range(3):
                    merged[i + ch] = (over[i + ch] * alpha + merged[i + ch] * (255 - alpha)) // 255
        pixels = bytes(merged)
    write_png(out, w, h, pixels)
    stem = out[:-4] if out.endswith(".png") else out
    for name, plane in (("app", app), ("shell", shell)):
        if plane:
            write_png("%s.%s.png" % (stem, name), plane[0], plane[1], plane[2])
    print("%s %dx%d app=%s shell=%s" % (out, w, h, bool(app), bool(shell)))


def mask(names):
    total = 0
    for name in names.split(","):
        if name not in BUTTONS:
            raise SystemExit("unknown button %r (have: %s)" % (name, ", ".join(sorted(BUTTONS))))
        total |= BUTTONS[name]
    return total


def main(argv):
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--host", default=os.environ.get("VITA_HOST", ""))
    ap.add_argument("--half", action="store_true")
    ap.add_argument("command", nargs="?")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args(argv)
    if not a.command or a.command in ("help", "-h", "--help"):
        print(__doc__)
        return 0
    if not a.host:
        raise SystemExit("Set VITA_HOST or pass --host")
    cmd, rest = a.command, a.args

    if cmd == "shot":
        shot(a.host, rest[0] if rest else "vita.png", a.half)
    elif cmd in ("status", "kill-fg", "reboot"):
        print(line(a.host, cmd))
    elif cmd == "press":
        print(line(a.host, "hold %d %s" % (mask(rest[0]), rest[1] if len(rest) > 1 else "100")))
    elif cmd == "keys":
        for name in rest:
            print(line(a.host, "hold %d 100" % mask(name)))
    elif cmd == "hold":
        print(line(a.host, "hold %d %s" % (mask(rest[0]), rest[1])))
    elif cmd == "stick":
        print(line(a.host, "stick " + " ".join(rest)))
    elif cmd == "tap":
        print(line(a.host, "touch %s %s %s" % (rest[0], rest[1], rest[2] if len(rest) > 2 else "120")))
    elif cmd == "swipe":
        print(line(a.host, "swipe " + " ".join(rest)))
    elif cmd in ("launch", "awake", "df"):
        print(line(a.host, cmd + " " + " ".join(rest)))
    elif cmd == "ls":
        with connect(a.host, "ls " + rest[0]) as c:
            for raw in c.makefile("rb"):
                text = raw.decode(errors="replace").rstrip("\n")
                if text.startswith(("OK ls", "ERR")):
                    print(text)
                    break
                kind, size, name = text.split(" ", 2)
                print("%s %10s %s" % ("d" if kind == "d" else "-", size, name))
    elif cmd == "get":
        with connect(a.host, "get " + rest[0], timeout=120) as c:
            f = c.makefile("rb")
            head = f.readline(128).decode(errors="replace").strip()
            if not head.startswith("OK get "):
                raise SystemExit(head or "no reply")
            size = int(head.split()[2])
            data = f.read(size)
        out = rest[1] if len(rest) > 1 else rest[0].rsplit("/", 1)[-1]
        Path(out).write_bytes(data)
        print("%s %d bytes" % (out, len(data)))
    else:
        raise SystemExit("unknown command %r (try help)" % cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
