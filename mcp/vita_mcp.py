#!/usr/bin/env python3
"""MCP server for a PS Vita running vita-agent-bridge.

Gives any MCP-speaking agent the loop this project is built around: look at the
screen, press something, look again. Speaks MCP over stdio with no dependencies
beyond the standard library.

Configure (Claude Desktop / Claude Code, claude_desktop_config.json):

    "mcpServers": {
      "vita": {
        "command": "python3",
        "args": ["/path/to/vita-agent-bridge/mcp/vita_mcp.py"],
        "env": {"VITA_HOST": "192.168.1.50"}
      }
    }

The token is read the same way as the CLI: VITA_TOKEN, or VITA_TOKEN_FILE,
or ~/.vita-agent-token.
"""
import base64
import contextlib
import json
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cli"))
import vita as cli  # noqa: E402

HOST = os.environ.get("VITA_HOST", "")
PROTOCOL = "2024-11-05"

TOOLS = [
    {
        "name": "vita_screenshot",
        "description": "Capture the Vita's screen and return it as an image. Use this "
                       "before deciding what to press, and again afterwards to confirm.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "half": {"type": "boolean", "description": "Half resolution (480x272), a quarter of the data."}
            },
        },
    },
    {
        "name": "vita_press",
        "description": "Press buttons in sequence, e.g. ['down','down','cross']. Names: "
                       "cross circle square triangle up down left right l r start select ps "
                       "volup voldown. Games take these; the system shell ignores everything "
                       "but ps, so use vita_tap there.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "buttons": {"type": "array", "items": {"type": "string"}},
                "hold_ms": {"type": "integer", "description": "How long to hold each (default 100)."},
            },
            "required": ["buttons"],
        },
    },
    {
        "name": "vita_tap",
        "description": "Tap the front touchscreen at screen pixels (960x544 space). The only "
                       "way to drive the system shell, its dialogs and LiveArea pages.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "integer"}, "y": {"type": "integer"},
                "hold_ms": {"type": "integer"},
            },
            "required": ["x", "y"],
        },
    },
    {
        "name": "vita_swipe",
        "description": "Drag across the touchscreen from one point to another.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x1": {"type": "integer"}, "y1": {"type": "integer"},
                "x2": {"type": "integer"}, "y2": {"type": "integer"},
                "ms": {"type": "integer", "description": "Duration, 16-10000 (default 400)."},
            },
            "required": ["x1", "y1", "x2", "y2"],
        },
    },
    {
        "name": "vita_launch",
        "description": "Launch an app by title id (nine characters, e.g. RETROVITA) and wait "
                       "until it is running; the lock screen and the LiveArea gate are handled. "
                       "If another app is open the shell asks to close it: either confirm with "
                       "vita_tap at 654,444, or pass force=true to close whatever is on screen "
                       "first (only when nothing unsaved is running).",
        "inputSchema": {
            "type": "object",
            "properties": {"title_id": {"type": "string"}, "force": {"type": "boolean"}},
            "required": ["title_id"],
        },
    },
    {
        "name": "vita_status",
        "description": "Bridge version, whether an app is in the foreground, and screen size.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "vita_list_files",
        "description": "List a directory on the console, e.g. ux0:data.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
        },
    },
    {
        "name": "vita_read_file",
        "description": "Read a file from the console. Text comes back inline; anything else "
                       "is saved locally and the path returned.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}, "max_inline_bytes": {"type": "integer"}},
            "required": ["path"],
        },
    },
    {
        "name": "vita_free_space",
        "description": "Free and total bytes on a device, e.g. ux0:.",
        "inputSchema": {
            "type": "object",
            "properties": {"device": {"type": "string"}},
        },
    },
    {
        "name": "vita_close_app",
        "description": "Close an app: by title_id if given (works even for apps that draw "
                       "nothing), otherwise whatever is in the foreground.",
        "inputSchema": {"type": "object", "properties": {"title_id": {"type": "string"}}},
    },
    {
        "name": "vita_keep_awake",
        "description": "Hold the screen on (true) or let it sleep normally (false). The console "
                       "drops off Wi-Fi when it sleeps, so set this before long unattended work.",
        "inputSchema": {
            "type": "object",
            "properties": {"on": {"type": "boolean"}},
            "required": ["on"],
        },
    },
    {
        "name": "vita_reboot",
        "description": "Cold reset the console. Works even when the system shell is wedged. "
                       "Ask the operator first: anything unsaved is lost.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "vita_upload_file",
        "description": "Copy a local file or folder onto the console. Works while a game runs. "
                       "The console hashes what it wrote and a mismatch is an error. Refused "
                       "under tai/ and outside the user partitions (ux0, ur0, uma0, imc0...).",
        "inputSchema": {
            "type": "object",
            "properties": {"local_path": {"type": "string"}, "remote_path": {"type": "string"}},
            "required": ["local_path", "remote_path"],
        },
    },
    {
        "name": "vita_download_file",
        "description": "Copy a file or folder from the console to a local path.",
        "inputSchema": {
            "type": "object",
            "properties": {"remote_path": {"type": "string"}, "local_path": {"type": "string"}},
            "required": ["remote_path", "local_path"],
        },
    },
    {
        "name": "vita_file_info",
        "description": "Whether a path is a file or folder, its size and modified time (UTC).",
        "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    },
    {
        "name": "vita_make_folder",
        "description": "Create a folder and any missing parents.",
        "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    },
    {
        "name": "vita_copy",
        "description": "Copy a file or folder on the console. Runs on the console and waits.",
        "inputSchema": {
            "type": "object",
            "properties": {"src": {"type": "string"}, "dst": {"type": "string"}},
            "required": ["src", "dst"],
        },
    },
    {
        "name": "vita_move",
        "description": "Move or rename a file or folder on the console. Across partitions it "
                       "copies and deletes the source only once the copy landed.",
        "inputSchema": {
            "type": "object",
            "properties": {"src": {"type": "string"}, "dst": {"type": "string"}},
            "required": ["src", "dst"],
        },
    },
    {
        "name": "vita_delete",
        "description": "Delete a file or folder (recursively). Cannot be undone. Device roots, "
                       "top-level folders like ux0:app, and tai/ are refused.",
        "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    },
    {
        "name": "vita_sha256",
        "description": "SHA-256 of a file, computed on the console.",
        "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]},
    },
    {
        "name": "vita_fetch_url",
        "description": "Have the console download a URL straight to a path. Certificates are "
                       "verified; the console's TLS is from 2018 and many modern HTTPS hosts "
                       "refuse it, in which case download locally and use vita_upload_file. "
                       "Giving sha256 lets a download through a certificate the console cannot "
                       "check, trusted by content instead.",
        "inputSchema": {
            "type": "object",
            "properties": {"url": {"type": "string"}, "remote_path": {"type": "string"},
                           "sha256": {"type": "string"}},
            "required": ["url", "remote_path"],
        },
    },
    {
        "name": "vita_battery",
        "description": "Battery percent, charging, external power, minutes left, temperature.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "vita_unlock",
        "description": "Swipe the lock screen away (PS, then the page-curl drag).",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def text(body):
    return {"content": [{"type": "text", "text": body}]}


def call(name, args):
    """stdout is the MCP transport: anything the CLI prints must go to stderr."""
    with contextlib.redirect_stdout(sys.stderr):
        return _call(name, args)


def _call(name, args):
    if not HOST:
        return text("VITA_HOST is not set: point it at the console's IP.")
    if name == "vita_screenshot":
        out = Path(tempfile.gettempdir()) / "vita-mcp-shot.png"
        cli.shot(HOST, str(out), bool(args.get("half")))
        data = base64.b64encode(out.read_bytes()).decode()
        return {"content": [{"type": "image", "data": data, "mimeType": "image/png"}]}
    if name == "vita_press":
        hold = int(args.get("hold_ms", 100))
        return text("\n".join(cli.line(HOST, "hold %d %d" % (cli.mask(b), hold)) for b in args["buttons"]))
    if name == "vita_tap":
        return text(cli.line(HOST, "touch %d %d %d" % (args["x"], args["y"], int(args.get("hold_ms", 120)))))
    if name == "vita_swipe":
        return text(cli.line(HOST, "swipe %d %d %d %d %d" % (
            args["x1"], args["y1"], args["x2"], args["y2"], int(args.get("ms", 400)))))
    if name == "vita_launch":
        return text(cli.line(HOST, "launch %s%s" % (args["title_id"], " force" if args.get("force") else ""), 45))
    if name == "vita_status":
        return text(cli.line(HOST, "status"))
    if name == "vita_close_app":
        if args.get("title_id"):
            return text(cli.line(HOST, "close " + args["title_id"]))
        return text(cli.line(HOST, "kill-fg"))
    if name == "vita_upload_file":
        return text(cli.put(HOST, args["local_path"], args["remote_path"]))
    if name == "vita_download_file":
        n = cli.get(HOST, args["remote_path"], args["local_path"])
        return text("%d bytes to %s" % (n, args["local_path"]))
    if name == "vita_file_info":
        return text(cli.line(HOST, "stat " + args["path"], 60))
    if name == "vita_make_folder":
        return text(cli.line(HOST, "mkdir " + args["path"], 60))
    if name in ("vita_copy", "vita_move"):
        return text(cli.job_run(HOST, "%s %s\t%s" % ("cp" if name == "vita_copy" else "mv",
                                                       args["src"], args["dst"])))
    if name == "vita_delete":
        return text(cli.job_run(HOST, "rm " + args["path"]))
    if name == "vita_sha256":
        return text(cli.job_run(HOST, "sha256 " + args["path"]).split()[-1])
    if name == "vita_fetch_url":
        sha = args.get("sha256", "")
        return text(cli.job_run(HOST, "fetch %s\t%s%s" % (args["url"], args["remote_path"],
                                                          "\t" + sha if sha else "")))
    if name == "vita_battery":
        return text(cli.line(HOST, "battery", 30))
    if name == "vita_unlock":
        return text(cli.line(HOST, "unlock", 30))
    if name == "vita_keep_awake":
        return text(cli.line(HOST, "awake " + ("1" if args["on"] else "0")))
    if name == "vita_reboot":
        return text(cli.line(HOST, "reboot"))
    if name == "vita_free_space":
        return text(cli.line(HOST, "df " + args.get("device", "ux0:")))
    if name == "vita_list_files":
        rows = []
        with cli.connect(HOST, "ls " + args["path"]) as c:
            for raw in c.makefile("rb"):
                entry = raw.decode(errors="replace").rstrip("\n")
                if entry.startswith(("OK ls", "ERR")):
                    rows.append(entry)
                    break
                rows.append(entry)
        return text("\n".join(rows))
    if name == "vita_read_file":
        with cli.connect(HOST, "get " + args["path"], timeout=120) as c:
            f = c.makefile("rb")
            head = f.readline(128).decode(errors="replace").strip()
            if not head.startswith("OK get "):
                return text(head or "no reply")
            data = f.read(int(head.split()[2]))
        limit = int(args.get("max_inline_bytes", 64 * 1024))
        if len(data) <= limit:
            try:
                return text(data.decode())
            except UnicodeDecodeError:
                pass
        out = Path(tempfile.gettempdir()) / Path(args["path"].replace(":", "_")).name
        out.write_bytes(data)
        return text("%d bytes saved to %s" % (len(data), out))
    return text("unknown tool " + name)


def main():
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            continue
        method, mid = msg.get("method"), msg.get("id")
        if method == "initialize":
            result = {"protocolVersion": PROTOCOL,
                      "capabilities": {"tools": {}},
                      "serverInfo": {"name": "vita-agent-bridge", "version": "1.1.0"}}
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            params = msg.get("params", {})
            try:
                result = call(params.get("name", ""), params.get("arguments") or {})
            except Exception as e:                      # a dead console must not kill the server
                result = {"content": [{"type": "text", "text": "%s: %s" % (type(e).__name__, e)}],
                          "isError": True}
        elif mid is None:                               # notification, nothing to answer
            continue
        else:
            result = {}
        sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": mid, "result": result}) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
