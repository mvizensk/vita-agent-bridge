# Draft post for r/vitahacks

Reddit is blocked for the assistant, so this is the text to paste in yourself.

Attach `docs/hero.png`. Suggested title:

> **I gave an AI agent eyes and hands on my Vita: screen capture + input over TCP, with an MCP server [open source]**

Body:

---

I wanted an AI agent to set up and test homebrew on my Vita without me holding
it. vitacompanion already does input injection and FTP, but nothing could send
back **what is on the screen**, and without that an agent is typing blind.

So: **vita-agent-bridge**. A kernel plugin that reads the display planes and
injects buttons/analog/touch, plus a small TCP service that vakern injects into
**SceShell**, which means it survives launching a game, unlike a background app
that gets evicted the moment RetroArch starts.

The loop is just:

```
vita.py shot screen.png     # look
vita.py tap 654 444         # act
vita.py shot screen.png     # look again
```

**What it does**
- Screen capture, both planes (app + shell), full or half res
- Buttons, analog, touch, swipes
- Launch by title id, close the foreground app
- Read-only `ls` / `get`: pull a log off the card *while a game is running*
- `df` for free space
- `reboot` from inside SceShell, which got me out of a wedged shell without the
  30-second power-button hold
- An **MCP server** (12 tools, standard library only) so Claude Desktop, Claude
  Code or anything else that speaks MCP can drive the console directly

**Why you might want it even if you do not care about AI**
Automated testing on real hardware. Boot your homebrew, press through a menu,
screenshot, diff against a reference, with no capture card. Same for pulling crash
dumps while the app is still up.

**Security, up front:** it is plaintext TCP and whoever holds the token can read
files, press anything and reboot your console. Token is generated per install
and read from a file on the card (no token file, no open port), and file access
is read-only. Trusted LAN only, never port-forward it.

Tested on a PCH-2000 on 3.65 Ensō. MIT. Kernel code is small and the privileged
work is all syscalls, so a bug in the network service kills a user process rather
than the kernel. I did still crash the console three times getting there.

Repo: https://github.com/mvizensk/vita-agent-bridge

Some quirks I hit, in case they save someone time:
- The shell ignores injected buttons entirely, except PS. System dialogs and
  LiveArea need touch.
- The console confirms with ○ in the shell but ✕ inside apps.
- A kernel module exporting syscalls can never be unloaded. Each revision needs
  a fresh module name, which is why mine carries a number.
- A LiveArea "Start" button sometimes ignores synthetic taps and I still do not
  know why. A real finger always works. If anyone knows, I would like to.

---

Notes before posting:
- r/vitahacks tends to prefer specifics over pitch; the quirks list is the part
  that earns replies.
- Consider cross-posting to r/PSVita later with the same image but a lighter
  body, and to GBAtemp's Vita homebrew forum.
- Be ready for "why not just use vitacompanion". The answer is screen capture
  and surviving game launches; the README has the table.
