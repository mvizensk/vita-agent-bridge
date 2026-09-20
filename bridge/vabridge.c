/* vabridge: user-mode TCP front end for vakern. vakern injects it into SceShell
 * (ksceKernelLoadStartModuleForPid), which the system never evicts -- hosted in
 * an ordinary background app it died the moment a big game launched.
 * All kernel work is vakern syscalls; any failure here is an error code or at
 * worst kills this user process, never the kernel.
 *
 * Port 1348, one client at a time, "AUTH <token> <command>\n":
 *   status | shot [half] | hold <mask> <ms> | stick <lx> <ly> <rx> <ry> <ms>
 *   awake <0|1> | kill-fg | ls <path> | get <path> | reboot | df <device>
 * ls replies "d|f <size> <name>" lines then "OK ls <count>"; get replies
 * "OK get <size>\n" then the raw bytes. Reads are the only file access here;
 * nothing in this module writes to the card.
 * shot replies with two planes (app, then shell), each a 24-byte VSHT header
 * {magic, width, height, pitch(=width), format, size} followed by RGBA rows. */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/net/net.h>
#include <psp2/appmgr.h>
#include <psp2/power.h>

#include "../kernel/vakern.h"

#define PORT 1348
#define TOKEN_PATH "ur0:data/vita-agent-bridge/token.txt"
#define TOKEN_LEN 64

/* Read at start from TOKEN_PATH so no secret is baked into the binary and each
 * install has its own. No token file, no server: fail closed. */
static char token[TOKEN_LEN];

static int load_token(void) {
    SceUID fd = sceIoOpen(TOKEN_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return fd;
    int n = sceIoRead(fd, token, TOKEN_LEN);
    sceIoClose(fd);
    return n == TOKEN_LEN ? 0 : -1;
}

static volatile int running;
static SceUID thread = -1;
static unsigned char buf[VAK_MAX_READ];
static char req[200];

static void log_line(const char *fmt, int a, int b) {
    char line[128];
    int n = sceClibSnprintf(line, sizeof(line), fmt, a, b);
    SceUID fd = sceIoOpen("ur0:data/vita-agent-bridge/bridge.log", SCE_O_CREAT | SCE_O_APPEND | SCE_O_WRONLY, 0666);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}

static int send_all(int s, const void *data, unsigned int len) {
    const char *p = data;
    int waits = 0;
    while (len) {
        int n = sceNetSend(s, p, len, 0);
        if (n > 0) { p += n; len -= n; waits = 0; continue; }
        if (n < 0 && (n == (int)SCE_NET_ERROR_EAGAIN || *sceNetErrnoLoc() == SCE_NET_EAGAIN) && ++waits < 2000) {
            sceKernelDelayThread(5000); continue;
        }
        return -1;
    }
    return 0;
}
static void say(int s, const char *t) { send_all(s, t, sceClibStrnlen(t, 512)); }

static int parse_uint(const char **p, unsigned int *out) {
    while (**p == ' ') ++*p;
    if (**p < '0' || **p > '9') return -1;
    unsigned int v = 0;
    while (**p >= '0' && **p <= '9') v = v * 10 + (unsigned int)(*(*p)++ - '0');
    *out = v;
    return 0;
}
static int starts(const char *s, const char *w) {
    size_t n = sceClibStrnlen(w, 64);
    return sceClibStrncmp(s, w, n) == 0 && (s[n] == 0 || s[n] == ' ');
}

typedef struct { char magic[4]; unsigned int width, height, pitch, format, size; } ShotHeader;

static int send_plane(int s, int plane, unsigned int step) {
    VakInfo info;
    ShotHeader h = {{'V', 'S', 'H', 'T'}, 0, 0, 0, 0, 0};
    if (vakGetPlaneInfo(plane, &info) < 0 || info.result < 0) return send_all(s, &h, sizeof(h));
    h.width = info.width / step; h.height = info.height / step; h.pitch = h.width;
    h.format = info.format; h.size = h.width * h.height * 4;
    if (send_all(s, &h, sizeof(h))) return -1;
    unsigned int row = h.width * 4, per = sizeof(buf) / row;
    for (unsigned int y = 0; y < h.height; y += per) {
        unsigned int rows = h.height - y < per ? h.height - y : per;
        VakRead r = {plane, step, y, rows, buf};
        int got = vakReadRows(&r);
        if (got < 0) { sceClibMemset(buf, 0, rows * row); log_line("read=%08X y=%d\n", got, y); }
        if (send_all(s, buf, rows * row)) return -1;
    }
    return 0;
}

static const char *path_arg(const char *p) {
    while (*p == ' ') ++p;
    /* device-qualified absolute paths only, no parent references */
    const char *colon = p;
    while (*colon && *colon != ':' && *colon != '/') ++colon;
    if (*colon != ':' || colon == p) return NULL;
    for (const char *q = p; *q; ++q)
        if (q[0] == '.' && q[1] == '.' && (q == p || q[-1] == '/') && (q[2] == 0 || q[2] == '/')) return NULL;
    return p;
}

static void cmd_ls(int s, const char *path) {
    SceUID d = sceIoDopen(path);
    char out[400];
    if (d < 0) { sceClibSnprintf(out, sizeof(out), "ERR ls rc=0x%08X\n", d); say(s, out); return; }
    SceIoDirent e;
    int count = 0;
    for (;;) {
        sceClibMemset(&e, 0, sizeof(e));
        if (sceIoDread(d, &e) <= 0) break;
        sceClibSnprintf(out, sizeof(out), "%c %llu %s\n", SCE_S_ISDIR(e.d_stat.st_mode) ? 'd' : 'f',
                        (unsigned long long)e.d_stat.st_size, e.d_name);
        if (send_all(s, out, sceClibStrnlen(out, sizeof(out)))) break;
        ++count;
    }
    sceIoDclose(d);
    sceClibSnprintf(out, sizeof(out), "OK ls %d\n", count);
    say(s, out);
}

static void cmd_get(int s, const char *path) {
    char out[64];
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) { sceClibSnprintf(out, sizeof(out), "ERR get rc=0x%08X\n", fd); say(s, out); return; }
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size < 0) { sceIoClose(fd); sceClibSnprintf(out, sizeof(out), "ERR get size=0x%08X\n", (int)size); say(s, out); return; }
    sceClibSnprintf(out, sizeof(out), "OK get %llu\n", (unsigned long long)size);
    say(s, out);
    SceOff left = size;
    while (left > 0) {
        int want = left > (SceOff)sizeof(buf) ? (int)sizeof(buf) : (int)left;
        int n = sceIoRead(fd, buf, want);
        if (n <= 0) break;  /* short file: client sees fewer bytes than promised */
        if (send_all(s, buf, n)) break;
        left -= n;
    }
    sceIoClose(fd);
}

static void hold_buttons(unsigned int mask, unsigned int ms) {
    SceUInt64 end = sceKernelGetProcessTimeWide() + (SceUInt64)ms * 1000;
    do { vakButtons(mask, 32); sceKernelDelayThread(8000); }
    while (sceKernelGetProcessTimeWide() < end);
    vakButtons(0, 32);
}

static void hold_stick(unsigned int packed, unsigned int ms) {
    SceUInt64 end = sceKernelGetProcessTimeWide() + (SceUInt64)ms * 1000;
    do { vakAnalog(packed, 32); sceKernelDelayThread(8000); }
    while (sceKernelGetProcessTimeWide() < end);
    vakAnalog(0x80808080, 0);
}

static void handle(int s) {
    int used = 0, waits = 0;
    while (used < (int)sizeof(req) - 1) {
        int n = sceNetRecv(s, req + used, 1, 0);
        if (n == 1) { if (req[used++] == '\n') break; continue; }
        if (n < 0 && ++waits < 500) { sceKernelDelayThread(10000); continue; }
        return;
    }
    req[used] = 0;
    while (used && (req[used - 1] == '\n' || req[used - 1] == '\r')) req[--used] = 0;
    unsigned int diff = used < 70;
    if (!diff) {
        diff |= sceClibStrncmp(req, "AUTH ", 5) != 0;
        for (int i = 0; i < 64; ++i) diff |= (unsigned char)req[5 + i] ^ (unsigned char)token[i];
        diff |= req[69] != ' ';
    }
    if (diff) { say(s, "ERR auth\n"); return; }
    const char *cmd = req + 70, *p;
    char out[512];
    unsigned int v[5];
    if (starts(cmd, "status")) {
        VakInfo a, b;
        int ra = vakGetPlaneInfo(0, &a), rb = vakGetPlaneInfo(1, &b);
        sceClibSnprintf(out, sizeof(out),
            "OK vita-agent-bridge 1.0 vakern=0x%04X app=%08X pid=0x%08X %ux%u shell=%08X pid=0x%08X\n",
            vakGetVersion(), ra, a.pid, a.width, a.height, rb, b.pid);
        say(s, out);
    } else if (starts(cmd, "shot")) {
        unsigned int step = sceClibStrncmp(cmd, "shot half", 9) == 0 ? 2 : 1;
        if (send_plane(s, 0, step) == 0) send_plane(s, 1, step);
    } else if (starts(cmd, "hold")) {
        p = cmd + 4;
        if (parse_uint(&p, &v[0]) || parse_uint(&p, &v[1]) || v[1] > 10000) say(s, "ERR hold <mask> <ms<=10000>\n");
        else if (vakButtons(v[0], 1) < 0) say(s, "ERR button mask refused\n");
        else { hold_buttons(v[0], v[1]); say(s, "OK hold\n"); }
    } else if (starts(cmd, "stick")) {
        p = cmd + 5;
        int bad = 0;
        for (int i = 0; i < 5; ++i) bad |= parse_uint(&p, &v[i]);
        if (bad || v[0] > 255 || v[1] > 255 || v[2] > 255 || v[3] > 255 || v[4] > 10000)
            say(s, "ERR stick <lx> <ly> <rx> <ry> <ms<=10000>\n");
        else { hold_stick(v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3], v[4]); say(s, "OK stick\n"); }
    } else if (starts(cmd, "awake")) {
        p = cmd + 5;
        if (parse_uint(&p, &v[0]) || vakAwake(v[0]) < 0) say(s, "ERR awake <0|1>\n");
        else say(s, v[0] ? "OK awake 1\n" : "OK awake 0\n");
    } else if (starts(cmd, "debug")) {
        unsigned int d[41];
        int rc = vakDebug(d);
        int n = sceClibSnprintf(out, sizeof(out), "OK debug rc=%08X", rc);
        for (int i = 0; i < 12 && rc >= 0; ++i)
            n += sceClibSnprintf(out + n, sizeof(out) - n, " %d:%u/%u/%u", i, d[i], d[12 + i], d[24 + i]);
        if (rc >= 0) n += sceClibSnprintf(out + n, sizeof(out) - n, " touch %u/%u/%u/%u patched=%u", d[36], d[37], d[38], d[39], d[40]);
        sceClibSnprintf(out + n, sizeof(out) - n, "\n");
        say(s, out);
    } else if (starts(cmd, "swipe")) {
        /* swipe <x1> <y1> <x2> <y2> <ms>: screen pixels, finger slides linearly. */
        p = cmd + 5;
        int bad = 0;
        for (int i = 0; i < 5; ++i) bad |= parse_uint(&p, &v[i]);
        if (bad || v[0] >= 960 || v[2] >= 960 || v[1] >= 544 || v[3] >= 544 || v[4] < 16 || v[4] > 10000)
            say(s, "ERR swipe <x1> <y1> <x2> <y2> <16<=ms<=10000>\n");
        else {
            unsigned int steps = v[4] / 16;
            int rc = 0;
            for (unsigned int i = 0; i <= steps && rc >= 0; ++i) {
                int x = (int)v[0] + ((int)v[2] - (int)v[0]) * (int)i / (int)steps;
                int y = (int)v[1] + ((int)v[3] - (int)v[1]) * (int)i / (int)steps;
                rc = vakTouch(x * 2, y * 2, 1);
                sceKernelDelayThread(16000);
            }
            vakTouch(v[2] * 2, v[3] * 2, 0);
            sceClibSnprintf(out, sizeof(out), "%s swipe rc=0x%08X\n", rc < 0 ? "ERR" : "OK", rc);
            say(s, out);
        }
    } else if (starts(cmd, "touch")) {
        /* touch <x> <y> <ms>: screen pixels (960x544), held for ms then lifted. */
        p = cmd + 5;
        if (parse_uint(&p, &v[0]) || parse_uint(&p, &v[1]) || parse_uint(&p, &v[2]) ||
            v[0] >= 960 || v[1] >= 544 || v[2] > 10000) say(s, "ERR touch <x<960> <y<544> <ms<=10000>\n");
        else {
            int rc = vakTouch(v[0] * 2, v[1] * 2, 1);
            sceKernelDelayThread(v[2] * 1000);
            vakTouch(v[0] * 2, v[1] * 2, 0);
            sceClibSnprintf(out, sizeof(out), "%s touch rc=0x%08X\n", rc < 0 ? "ERR" : "OK", rc);
            say(s, out);
        }
    } else if (starts(cmd, "launch")) {
        /* launch <TITLEID>: 9 chars [A-Z0-9]. If another app is open the shell
         * asks to close it; answer that with touch (OK button). */
        p = cmd + 6;
        while (*p == ' ') ++p;
        int ok = 1;
        for (int i = 0; i < 9; ++i) ok &= (p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9');
        if (!ok || p[9]) say(s, "ERR launch <TITLEID>\n");
        else {
            char uri[48];
            sceClibSnprintf(uri, sizeof(uri), "psgm:play?titleid=%s", p);
            int rc = sceAppMgrLaunchAppByUri(0xFFFFF, uri);
            sceClibSnprintf(out, sizeof(out), "%s launch %s rc=0x%08X\n", rc < 0 ? "ERR" : "OK", p, rc);
            say(s, out);
        }
    } else if (starts(cmd, "ls") || starts(cmd, "get")) {
        const char *path = path_arg(cmd + (cmd[0] == 'l' ? 2 : 3));
        if (!path) say(s, "ERR path must be like ux0:dir/file, no ..\n");
        else if (cmd[0] == 'l') cmd_ls(s, path);
        else cmd_get(s, path);
    } else if (starts(cmd, "df")) {
        /* Free space before a big copy: "df ux0:" -> free/total bytes. */
        const char *dev = path_arg(cmd + 2);
        uint64_t total = 0, free_bytes = 0;
        int rc = dev ? sceAppMgrGetDevInfo(dev, &total, &free_bytes) : -1;
        if (rc < 0) sceClibSnprintf(out, sizeof(out), "ERR df rc=0x%08X\n", rc);
        else sceClibSnprintf(out, sizeof(out), "OK df %s free=%llu total=%llu\n", dev,
                             (unsigned long long)free_bytes, (unsigned long long)total);
        say(s, out);
    } else if (starts(cmd, "reboot")) {
        /* 4.1: our thread keeps running when SceShell's UI thread wedges (a
         * stuck system dialog, 2026-09-19), and then nothing can launch the
         * Recovery app that normally owns reboot.suprx. This is the way back
         * in without holding the power button. */
        say(s, "OK reboot\n");
        sceKernelDelayThread(300 * 1000);
        scePowerRequestColdReset();
    } else if (starts(cmd, "kill-fg")) {
        int rc = vakKillForeground();
        sceClibSnprintf(out, sizeof(out), "%s kill-fg rc=0x%08X\n", rc < 0 ? "ERR" : "OK", rc);
        say(s, out);
    } else say(s, "ERR unknown command\n");
}

static int server(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (running) {
        int s = sceNetSocket("vabridge", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
        if (s < 0) { log_line("socket=%08X %d\n", s, 0); sceKernelDelayThread(2000000); continue; }
        int one = 1;
        sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));
        sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
        SceNetSockaddrIn addr;
        sceClibMemset(&addr, 0, sizeof(addr));
        addr.sin_len = sizeof(addr);
        addr.sin_family = SCE_NET_AF_INET;
        addr.sin_port = sceNetHtons(PORT);
        addr.sin_addr.s_addr = SCE_NET_INADDR_ANY;
        int rc = sceNetBind(s, (SceNetSockaddr *)&addr, sizeof(addr));
        if (rc >= 0) rc = sceNetListen(s, 2);
        if (rc < 0) { log_line("bind/listen=%08X %d\n", rc, 0); sceNetSocketClose(s); sceKernelDelayThread(2000000); continue; }
        log_line("listening %d vakern=0x%04X\n", PORT, vakGetVersion());
        while (running) {
            int c = sceNetAccept(s, NULL, NULL);
            if (c < 0) {
                /* EAGAIN is the idle case; anything else (e.g. Wi-Fi dropped and
                 * the socket died) rebuilds the listener. */
                if (c != (int)SCE_NET_ERROR_EAGAIN && *sceNetErrnoLoc() != SCE_NET_EAGAIN) {
                    log_line("accept=%08X rebuilding %d\n", c, 0);
                    break;
                }
                sceKernelDelayThread(50000);
                continue;
            }
            handle(c);
            sceNetSocketClose(c);
        }
        sceNetSocketClose(s);
    }
    return 0;
}

int module_start(SceSize args, void *argp) {
    (void)args; (void)argp;
    if (load_token() < 0) {
        log_line("no token at " TOKEN_PATH " (%d %d)\n", 0, 0);
        return SCE_KERNEL_START_SUCCESS;   /* loaded but deaf: never open a port without auth */
    }
    running = 1;
    thread = sceKernelCreateThread("vabridge", server, 0x10000100, 0x4000, 0, 0, NULL);
    if (thread < 0 || sceKernelStartThread(thread, 0, NULL) < 0) {
        log_line("thread=%08X %d\n", thread, 0);
        running = 0;
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
    (void)args; (void)argp;
    running = 0;
    SceUInt timeout = 15000000;
    if (sceKernelWaitThreadEnd(thread, NULL, &timeout) < 0) { running = 1; return SCE_KERNEL_STOP_FAIL; }
    sceKernelDeleteThread(thread);
    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
