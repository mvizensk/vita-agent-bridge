# Wire protocol

TCP, port 1348, one command per connection. Control commands are handled one at
a time; `put` and `get` run on their own threads (two at most, a third gets
`ERR busy: transfers running, retry`):

```
AUTH <64-hex-token> <command>\n
```

The token is compared in full before the command is looked at. Anything that
fails gets `ERR auth\n` and nothing else happens.

| Command | Reply |
|---|---|
| `status` | `OK vita-agent-bridge <ver> vakern=0x%04X app=%08X pid=0x%08X <w>x<h> shell=%08X pid=0x%08X` |
| `shot [half]` | two planes, app then shell (see below) |
| `hold <mask> <ms>` | `OK hold`, mask is the SceCtrl button bits |
| `stick <lx> <ly> <rx> <ry> <ms>` | `OK stick`, 0-255 each |
| `touch <x> <y> <ms>` | `OK touch rc=0x...`, screen pixels in 960x544 space |
| `swipe <x1> <y1> <x2> <y2> <ms>` | `OK swipe rc=0x...`, linear drag, 16-10000 ms |
| `launch <TITLEID> [force]` | `OK launch <id> rc=0x...` once it is running; `0x80AB0010` = never started. `force` closes the app on screen first |
| `close <TITLEID>` | `OK close <id> rc=0x...`, ends an app by title ID |
| `unlock` | `OK unlock`, PS then a page-curl swipe |
| `battery` | `OK battery <n>% charging=<0\|1> plugged=<0\|1> minutes=<n> temp=<c>C` |
| `kill-fg` | `OK kill-fg rc=0x...`, never kills the shell itself |
| `awake <0\|1>` | `OK awake <n>` |
| `ls <path>` | `d\|f <size> <name>` per line, then `OK ls <count>` |
| `get <path>` | `OK get <size>\n` then raw bytes |
| `put <size> <path>` | send exactly `<size>` raw bytes after the line; reply `OK put <size> sha256=<hex>` (compare it with your own hash) |
| `stat <path>` | `OK stat d\|f <size> <YYYY-MM-DDTHH:MM:SS>` (UTC) |
| `mkdir <path>` | `OK mkdir`, parents included |
| `cp <src>\t<dst>` / `mv <src>\t<dst>` | `OK job <id> started`; tab between paths, since names contain spaces |
| `rm <path>` / `sha256 <path>` | `OK job <id> started` |
| `fetch <url>\t<path>[\t<sha256>]` | `OK job <id> started`; HTTP(S) download on the console |
| `job` | `OK job <id> <kind> <state> <phase> files=a/b bytes=c/d rc=0x... <note>`; note is the sha256 when a hash or fetch is done, or where it failed |
| `job cancel` | `OK job cancelling`; a half-written file is removed |
| `df <device>` | `OK df <dev> free=<bytes> total=<bytes>` |
| `reboot` | `OK reboot`, then the console resets ~0.3 s later |
| `debug` | hook counters, for diagnosing input that does not land |

Paths must be device-qualified (`ux0:...`) and may not contain `..`.

One background job runs at a time; poll `job` until its state is not `running`.
Error codes of the form `0x80AB00xx` are the bridge's own: `01` refused by the
write rules, `02` a job is already running, `03` no memory for a buffer, `09`
copying a folder into itself, `10` launched but never started, `11-16` fetch
(bad URL, HTTP status, sha256 mismatch, cancelled, short write, certificate
refused with no sha256 given).

## Screenshots

`shot` writes two planes back to back: the foreground app's, then the shell's.
Each is a 24-byte header followed by RGBA rows:

```c
struct { char magic[4]; /* "VSHT" */
         uint32_t width, height, pitch, format, size; };
```

A plane that is not present comes back as a header with `size == 0`. `half`
samples every second pixel, a quarter of the bytes, and is usually enough for an
agent to decide what to do. The shell plane carries alpha: composite it over the
app plane to get what a person would see (the CLI does this).

## Notes

- Buttons injected this way reach apps but **not** SceShell (except PS), so use
  `touch`/`swipe` for the system UI.
- Every privileged action is a vakern syscall. The bridge is a user module: if it
  faults, SceShell's process takes it, not the kernel.
