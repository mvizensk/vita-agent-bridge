# vita-agent-bridge

**See and control a PS Vita over the network, and let an AI agent do it.**

A kernel plugin plus a TCP service that lives *inside* SceShell. It captures the
screen, injects buttons and touch, launches apps, reads files and can cold-reset
the console. A CLI drives it from a terminal; an MCP server hands the same
abilities to any agent that speaks MCP (Claude Desktop, Claude Code, and others).

```
$ vita.py shot screen.png     # look
$ vita.py tap 654 444         # act
$ vita.py shot screen.png     # look again
```

That loop is the whole point. With screenshots in the loop an agent can work the
console on its own: install homebrew, walk a menu, verify your app booted, dig a
log off the card.

## Why not vitacompanion?

[vitacompanion](https://github.com/devnoname120/vitacompanion) already gives you
FTP, app launching and input injection, and it is excellent. This adds the parts
it does not have:

| | vitacompanion | vita-agent-bridge |
|---|---|---|
| Buttons / analog / touch | yes | yes |
| **Screen capture** | no | **yes, both planes, full or half res** |
| Survives a game launching | background app, gets evicted | **lives in SceShell** |
| Recover a wedged shell | – | **`reboot` from inside the shell** |
| Read files while a game runs | FTP dies with the app | **`ls` / `get` over the same socket** |
| Free space | – | `df` |
| Agent integration | – | **MCP server included** |

If you only need to push files and launch things, use vitacompanion. If you want
something to *watch the screen and react*, that is this.

## What you can build with it

- **Automated testing on real hardware.** Boot your homebrew, press through a
  menu, screenshot, compare against a reference. No capture card.
- **Remote debugging.** Pull logs and crash dumps off the card while the app is
  still running.
- **Agent-driven setup.** Point an LLM at a fresh console and let it install and
  configure homebrew, checking its own work from the screen.

## Install

Requires HENkaku Ensō (tested on 3.65) and taiHEN.

1. Grab `vakern.skprx` and `vabridge.suprx` from Releases, or build them
   (VitaSDK, then `cmake -S . -B build && cmake --build build`).
2. On the console, create `ur0:data/vita-agent-bridge/` and copy both modules in.
3. Make a token, 64 hex characters, unique to your console:
   ```sh
   python3 -c "import secrets; print(secrets.token_hex(32))" > token.txt
   ```
   Copy `token.txt` to `ur0:data/vita-agent-bridge/token.txt`, and keep the same
   value on your computer at `~/.vita-agent-token`.
   **No token file, no open port.** The service fails closed on purpose.
4. Add the kernel plugin to `ur0:tai/config.txt`:
   ```
   *KERNEL
   ur0:tai/vakern.skprx
   ```
   (or keep it in `ur0:data/vita-agent-bridge/` and load it at runtime from your
   own app with `taiLoadStartKernelModule`).
5. Reboot. `vakern` injects the bridge into SceShell and it listens on **1348**.

```sh
export VITA_HOST=192.168.1.50
python3 cli/vita.py status
```

## MCP server

```json
"mcpServers": {
  "vita": {
    "command": "python3",
    "args": ["/path/to/vita-agent-bridge/mcp/vita_mcp.py"],
    "env": {"VITA_HOST": "192.168.1.50"}
  }
}
```

Twelve tools: `vita_screenshot`, `vita_press`, `vita_tap`, `vita_swipe`,
`vita_launch`, `vita_status`, `vita_list_files`, `vita_read_file`,
`vita_free_space`, `vita_close_app`, `vita_keep_awake`, `vita_reboot`.
Standard library only, no pip install.

## Security, plainly

This opens a plaintext TCP port on a console that will, for anyone holding the
token: read any file, press any button, launch anything, and reboot. There is no
encryption and no rate limiting.

- Trusted LAN only. **Never** port-forward 1348.
- The token is compared in full before any command runs, and it is read from
  your console's own file rather than compiled in, so no two installs share one.
- File access is **read-only**: nothing here writes to the card.
- If that trade is not right for you, do not install it.

## Known quirks (learned the hard way)

- The system shell ignores injected buttons, everything except PS. Shell
  dialogs, LiveArea pages and Settings need **touch**.
- The console confirms with ○ in the shell but ✕ inside apps.
- Launching an app while another runs shows a "will close" dialog: tap 654,444.
- A LiveArea "Start" button sometimes ignores synthetic taps. Nobody has got to
  the bottom of that one; a real finger works.
- The Vita sleeps when idle and takes Wi-Fi with it. `vita_keep_awake` /
  `vita.py awake 1` before long unattended work.
- A kernel module that exports syscalls cannot be unloaded. Each revision needs a
  new module and library name, which is why the names carry a number.

## How it fits together

```
your machine                     PS Vita
------------                     -------
vita.py / MCP server  --TCP 1348--> vabridge.suprx   (inside SceShell)
                                        |  syscalls
                                    vakern.skprx     (kernel)
                                        |
                          SceCtrl + SceTouch hooks, framebuffer, power
```

`vakern` hooks twelve `SceCtrl` entry points and four `ksceTouch` ones (the
ds4vita pattern), reads both display planes, and injects `vabridge` into SceShell
with `ksceKernelLoadStartModuleForPid`. The shell is the one process games never
evict. `vabridge` is just a TCP front end: every privileged action is a syscall,
so a mistake there kills a user process, not the kernel.

See [docs/protocol.md](docs/protocol.md) for the wire format.

## Credits and licence

MIT, see [LICENSE](LICENSE). The touch-hook approach follows
[ds4vita](https://github.com/xerpi/ds4vita); built with
[VitaSDK](https://vitasdk.org) and [taiHEN](https://github.com/yifanlu/taiHEN).

Developed against a PCH-2000 on 3.65 Ensō. Reports from other models and
firmwares are welcome.
