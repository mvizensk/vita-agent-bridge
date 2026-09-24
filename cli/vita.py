#!/usr/bin/env python3
"""Drive a PS Vita running vita-agent-bridge: look at the screen, press things.

    export VITA_HOST=192.168.1.50          # or pass --host
    vita.py status                          # version, foreground app, screen size
    vita.py shot screen.png [--half]        # both planes; .app/.shell written too
    vita.py press cross                     # one button
    vita.py keys down down cross            # a sequence
    vita.py hold up,cross 600               # hold a mask for N ms
    vita.py stick 0 128 128 128 500         # analog: lx ly rx ry ms
    vita.py tap 480 272 [ms]                # touch, screen pixels (960x544)
    vita.py swipe 880 110 200 500 800       # drag
    vita.py launch TITLEID [force]          # waits until it runs; force closes what is open
    vita.py close TITLEID                   # end an app by title ID
    vita.py kill-fg                         # close the foreground app
    vita.py unlock                          # swipe the lock screen away
    vita.py awake 0|1                       # hold the screen on
    vita.py battery                         # percent, charging, temperature
    vita.py df ux0:                         # free space
    vita.py reboot                          # cold reset (works with a wedged shell)

  Files (work while a game runs; every upload is SHA-256 checked on the Vita):
    vita.py ls ux0:data                     vita.py stat ux0:path
    vita.py get ux0:path [local]            vita.py put local ux0:path   (folders recurse)
    vita.py cp SRC DST    vita.py mv SRC DST    vita.py rm PATH    vita.py mkdir PATH
    vita.py sha256 PATH                     vita.py job [cancel]
    vita.py fetch URL ux0:path [--sha256 HEX]   # the Vita downloads it itself

  Writes are refused under tai/, and device roots and top-level folders cannot
  be removed. cp/mv/rm/sha256/fetch run as one background job on the Vita.

The token is the 64 hex characters in ur0:data/vita-agent-bridge/token.txt on the
console. Point VITA_TOKEN_FILE at your local copy (default ~/.vita-agent-token),
or set VITA_TOKEN. Plaintext over TCP: trusted LAN only, never port-forwarded.
"""
import argparse
import hashlib
import os
import time
import socket
import struct
import sys
import zlib
from pathlib import Path

PORT = 1348
BUTTONS = {
    "select": 0x1, "l3": 0x2, "r3": 0x4, "start": 0x8, "up": 0x10, "right": 0x20,
    "down": 0x40, "left": 0x80, "l2": 0x100, "r2": 0x200,
    # the physical shoulders report as L1/R1; the trigger bits are the DS3 mapping
    "l": 0x400, "l1": 0x400, "r": 0x800, "r1": 0x800, "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000,
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


def line(host, command, timeout=15):
    with connect(host, command, timeout) as c:
        return c.makefile("rb").readline(512).decode(errors="replace").strip()


def _retry_busy(fn, *args):
    """Two transfers run at once; a third is told 'ERR busy' and retries."""
    for attempt in range(600):
        try:
            return fn(*args)
        except OSError as e:
            if "busy" not in str(e) or attempt == 599:
                raise
            time.sleep(1)


def _put(host, local, remote):
    size = os.path.getsize(local)
    h = hashlib.sha256()
    with connect(host, "put %d %s" % (size, remote), timeout=60) as c, open(local, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            try:
                c.sendall(chunk)
            except OSError:
                c.settimeout(3)
                try:
                    why = c.makefile("rb").readline(512).decode(errors="replace").strip()
                except OSError:
                    why = ""
                raise OSError("put %s: %s" % (remote, why or "connection dropped"))
            h.update(chunk)
        reply = c.makefile("rb").readline(512).decode(errors="replace").strip()
    if not reply.startswith("OK put"):
        raise OSError(reply or "put: no reply")
    if reply.split("sha256=")[-1] != h.hexdigest():
        raise OSError("put %s: checksum mismatch (%s)" % (remote, reply))
    return reply


def put(host, local, remote):
    """Upload a file, or a folder recursively. The Vita hashes what it wrote."""
    if os.path.isdir(local):
        line(host, "mkdir " + remote, 60)
        files = sorted(os.path.join(d, f) for d, _, fs in os.walk(local) for f in fs)
        for path in files:
            rel = os.path.relpath(path, local).replace(os.sep, "/")
            _retry_busy(_put, host, path, remote.rstrip("/") + "/" + rel)
        return "OK put %d files to %s" % (len(files), remote)
    return _retry_busy(_put, host, local, remote)


def _get(host, remote, local):
    with connect(host, "get " + remote, timeout=60) as c:
        f = c.makefile("rb")
        head = f.readline(128).decode(errors="replace").strip()
        if not head.startswith("OK get "):
            raise OSError(head or "get: no reply")
        size, got = int(head.split()[2]), 0
        with open(local, "wb") as out:
            while got < size:
                chunk = f.read(min(1 << 20, size - got))
                if not chunk:
                    raise OSError("get %s: short read %d/%d" % (remote, got, size))
                out.write(chunk)
                got += len(chunk)
    return size


def get(host, remote, local):
    """Download a file, or a folder recursively, streaming to disk."""
    if line(host, "stat " + remote, 60).startswith("OK stat d"):
        os.makedirs(local, exist_ok=True)
        n = 0
        for is_dir, _, name in listing(host, remote):
            n += get(host, remote.rstrip("/") + "/" + name, os.path.join(local, name))
        return n
    return _retry_busy(_get, host, remote, local)


def listing(host, path):
    out = []
    with connect(host, "ls " + path) as c:
        for raw in c.makefile("rb"):
            text = raw.decode(errors="replace").rstrip("\n")
            if text.startswith("OK ls"):
                return out
            if text.startswith("ERR"):
                raise OSError(text)
            kind, size, name = text.split(" ", 2)
            out.append((kind == "d", int(size), name))
    raise OSError("ls: connection closed early")


def job_run(host, command, poll=0.5):
    """Start a background job on the Vita and wait for it; returns its status line."""
    first = line(host, command, 60)
    if not first.startswith("OK job"):
        raise OSError(first)
    job_id = first.split()[2]
    while True:
        status = line(host, "job", 60)
        parts = status.split()
        if len(parts) > 4 and parts[2] == job_id and parts[4] != "running":
            if parts[4] != "done":
                raise OSError(status)
            return status
        time.sleep(poll)


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
    elif cmd == "launch":
        print(line(a.host, "launch " + " ".join(rest), 45))
    elif cmd in ("awake", "df", "close", "stat", "mkdir"):
        print(line(a.host, cmd + " " + " ".join(rest), 60))
    elif cmd in ("battery", "unlock"):
        print(line(a.host, cmd, 30))
    elif cmd == "ls":
        for is_dir, size, name in listing(a.host, rest[0] if rest else "ux0:"):
            print("%s %10s %s" % ("d" if is_dir else "-", "" if is_dir else size, name))
    elif cmd == "get":
        out = rest[1] if len(rest) > 1 else rest[0].rstrip("/").rsplit("/", 1)[-1].split(":")[-1]
        print("%s %d bytes" % (out, get(a.host, rest[0], out)))
    elif cmd == "put":
        print(put(a.host, rest[0], rest[1]))
    elif cmd in ("cp", "mv"):
        print(job_run(a.host, "%s %s\t%s" % (cmd, rest[0], rest[1])))
    elif cmd in ("rm", "sha256"):
        print(job_run(a.host, "%s %s" % (cmd, rest[0])))
    elif cmd == "fetch":
        sha = ""
        if "--sha256" in rest:
            i = rest.index("--sha256")
            sha, rest = rest[i + 1].lower(), rest[:i] + rest[i + 2:]
        print(job_run(a.host, "fetch %s\t%s%s" % (rest[0], rest[1], "\t" + sha if sha else "")))
    elif cmd == "job":
        print(line(a.host, "job cancel" if rest and rest[0] == "cancel" else "job"))
    else:
        raise SystemExit("unknown command %r (try help)" % cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
