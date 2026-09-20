# Wire protocol

TCP, port 1348, one client at a time, one command per connection:

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
| `launch <TITLEID>` | `OK launch <id> rc=0x...`, nine chars A-Z0-9 |
| `kill-fg` | `OK kill-fg rc=0x...`, never kills the shell itself |
| `awake <0\|1>` | `OK awake <n>` |
| `ls <path>` | `d\|f <size> <name>` per line, then `OK ls <count>` |
| `get <path>` | `OK get <size>\n` then raw bytes |
| `df <device>` | `OK df <dev> free=<bytes> total=<bytes>` |
| `reboot` | `OK reboot`, then the console resets ~0.3 s later |
| `debug` | hook counters, for diagnosing input that does not land |

Paths must be device-qualified (`ux0:...`) and may not contain `..`.

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
