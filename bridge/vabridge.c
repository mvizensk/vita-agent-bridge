/* vabridge: user-mode TCP front end for vakern. vakern injects it into SceShell
 * (ksceKernelLoadStartModuleForPid), which the system never evicts -- hosted in
 * an ordinary background app it died the moment a big game launched.
 * All kernel work is vakern syscalls; any failure here is an error code or at
 * worst kills this user process, never the kernel.
 *
 * Port 1348, one command per connection, "AUTH <token> <command>\n":
 *   status | shot [half] | hold <mask> <ms> | stick <lx> <ly> <rx> <ry> <ms>
 *   awake <0|1> | kill-fg | ls <path> | get <path> | reboot | df <device>
 *   launch <TITLEID> [force] | close <TITLEID> | unlock | battery
 * 1.1 file management (fileops.c), works while a game runs:
 *   put <size> <path> + raw bytes | stat | mkdir | cp|mv <src>\t<dst> | rm
 *   sha256 | fetch <url>\t<path>[\t<sha256>] | job [cancel]
 * ls replies "d|f <size> <name>" lines then "OK ls <count>"; get replies
 * "OK get <size>\n" then the raw bytes. put/get run on their own threads (two
 * at most; a third gets "ERR busy"), cp/mv/rm/sha256/fetch on one background
 * job thread. Writes are limited by fileops.c: user partitions only, never
 * tai/, never a device root or top-level folder, always via .part + rename.
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
#include "fileops.h"

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
static char req[1200];                     /* two full paths for cp/mv */

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

/* put/get on their own threads so a long transfer never holds the control
 * connection. Two at most: three streams with big socket buffers exhausted
 * SceShell's network pool and dropped Wi-Fi for minutes (fileops.c keeps the
 * buffers at 128 KB for the same reason). */
#define MAX_XFERS 2
static volatile int xfers;
typedef struct { int s, put; unsigned long long size; char path[FO_PATH_MAX]; } Xfer;

static int xfer_main(SceSize len, void *argp) {
    (void)len;
    Xfer *x = argp;                 /* sceKernelStartThread copied it onto our stack */
    char out[160];
    if (x->put) {
        fo_put(x->s, x->path, x->size, out, sizeof(out));
        send_all(x->s, out, sceClibStrnlen(out, sizeof(out)));
    } else {
        fo_get(x->s, x->path);
    }
    sceNetSocketClose(x->s);
    __sync_sub_and_fetch(&xfers, 1);
    return sceKernelExitDeleteThread(0);
}

/* 0 = the socket now belongs to a transfer thread. */
static int xfer_start(int s, int put, unsigned long long size, const char *path) {
    if (__sync_add_and_fetch(&xfers, 1) > MAX_XFERS) { __sync_sub_and_fetch(&xfers, 1); return -1; }
    Xfer x;
    x.s = s; x.put = put; x.size = size;
    int n = (int)sceClibStrnlen(path, FO_PATH_MAX - 1);
    sceClibMemcpy(x.path, path, n);
    x.path[n] = 0;
    SceUID t = sceKernelCreateThread("vabridge_xfer", xfer_main, 0x10000100, 0x6000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, sizeof(x), &x) < 0) {
        if (t >= 0) sceKernelDeleteThread(t);
        __sync_sub_and_fetch(&xfers, 1);
        return -1;
    }
    return 0;
}

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

/* The lock screen: PS first (by hand the swipe only took after one), then
 * drag from the page-curl corner. */
static void unlock_screen(void) {
    hold_buttons(0x10000, 120);
    sceKernelDelayThread(1200 * 1000);
    for (int i = 0; i <= 50; ++i) {
        vakTouch(1760 + (400 - 1760) * i / 50, 220 + (1000 - 220) * i / 50, 1);
        sceKernelDelayThread(16 * 1000);
    }
    vakTouch(400, 1000, 0);
    sceKernelDelayThread(1500 * 1000);
}

static int app_running(const char *tid) {
    SceUID pid = -1;
    return sceAppMgrGetIdByName(&pid, tid) >= 0 && pid > 0;
}

static int app_on_screen(void) {
    VakInfo a;
    return vakGetPlaneInfo(0, &a) >= 0 && a.result >= 0 && a.pid > 0;
}

/* A plain URI launch misses in three ways: right after an app exits it only
 * opens the LiveArea gate (a second launch starts it), the lock screen blocks
 * it, and another running app makes the shell ask "the following application
 * will close". The first two are handled here; the third only with "force",
 * which closes whatever is on screen first, because that may be someone's
 * unsaved game. */
static int launch_title(const char *tid, int force) {
    char uri[48];
    sceClibSnprintf(uri, sizeof(uri), "psgm:play?titleid=%s", tid);
    if (force && !app_running(tid)) {
        for (int i = 0; i < 20 && app_on_screen(); ++i) {
            if (i % 10 == 0) vakKillForeground();
            sceKernelDelayThread(200 * 1000);
        }
        sceKernelDelayThread(800 * 1000);
    }
    int rc = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt) unlock_screen();
        rc = sceAppMgrLaunchAppByUri(0xFFFFF, uri);
        sceKernelDelayThread(1500 * 1000);
        if (!app_running(tid)) rc = sceAppMgrLaunchAppByUri(0xFFFFF, uri);
        for (int i = 0; i < 20 && !app_running(tid); ++i) sceKernelDelayThread(200 * 1000);
        if (app_running(tid)) return rc;
    }
    return rc < 0 ? rc : (int)0x80AB0010;   /* launched, never started: see the screen */
}

static int valid_tid(const char *p) {
    for (int i = 0; i < 9; ++i)
        if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) return 0;
    return p[9] == 0 || p[9] == ' ';
}

static int handle(int s) {
    int used = 0, waits = 0;
    while (used < (int)sizeof(req) - 1) {
        int n = sceNetRecv(s, req + used, 1, 0);
        if (n == 1) { if (req[used++] == '\n') break; continue; }
        if (n < 0 && ++waits < 500) { sceKernelDelayThread(10000); continue; }
        return 0;
    }
    req[used] = 0;
    while (used && (req[used - 1] == '\n' || req[used - 1] == '\r')) req[--used] = 0;
    unsigned int diff = used < 70;
    if (!diff) {
        diff |= sceClibStrncmp(req, "AUTH ", 5) != 0;
        for (int i = 0; i < 64; ++i) diff |= (unsigned char)req[5 + i] ^ (unsigned char)token[i];
        diff |= req[69] != ' ';
    }
    if (diff) { say(s, "ERR auth\n"); return 0; }
    const char *cmd = req + 70, *p;
    char out[512];
    unsigned int v[5];
    if (starts(cmd, "status")) {
        VakInfo a, b;
        int ra = vakGetPlaneInfo(0, &a), rb = vakGetPlaneInfo(1, &b);
        sceClibSnprintf(out, sizeof(out),
            "OK vita-agent-bridge 1.1 vakern=0x%04X app=%08X pid=0x%08X %ux%u shell=%08X pid=0x%08X\n",
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
            /* Re-send the finger every frame: a single report is often read
             * as nothing, so taps did not land on some screens. */
            int rc, held = 0;
            do { rc = vakTouch(v[0] * 2, v[1] * 2, 1); sceKernelDelayThread(16 * 1000); held += 16; }
            while (held < (int)v[2] && rc >= 0);
            vakTouch(v[0] * 2, v[1] * 2, 0);
            sceClibSnprintf(out, sizeof(out), "%s touch rc=0x%08X\n", rc < 0 ? "ERR" : "OK", rc);
            say(s, out);
        }
    } else if (starts(cmd, "launch")) {
        /* launch <TITLEID> [force]: returns once the app is running. Without
         * force, another open app still makes the shell ask to close it. */
        p = cmd + 6;
        while (*p == ' ') ++p;
        if (!valid_tid(p) || (p[9] && sceClibStrncmp(p + 9, " force", 7))) say(s, "ERR launch <TITLEID> [force]\n");
        else {
            char tid[10];
            sceClibMemcpy(tid, p, 9);
            tid[9] = 0;
            int rc = launch_title(tid, p[9] != 0);
            sceClibSnprintf(out, sizeof(out), "%s launch %s rc=0x%08X\n", rc < 0 ? "ERR" : "OK", tid, rc);
            say(s, out);
        }
    } else if (starts(cmd, "close")) {
        /* close <TITLEID>: ends an app by title ID, even one that draws nothing. */
        p = cmd + 5;
        while (*p == ' ') ++p;
        if (!valid_tid(p) || p[9]) say(s, "ERR close <TITLEID>\n");
        else {
            int rc = sceAppMgrDestroyAppByName(p);
            sceClibSnprintf(out, sizeof(out), "%s close %s rc=0x%08X\n", rc < 0 ? "ERR" : "OK", p, rc);
            say(s, out);
        }
    } else if (starts(cmd, "unlock")) {
        unlock_screen();
        say(s, "OK unlock\n");
    } else if (starts(cmd, "battery")) {
        sceClibSnprintf(out, sizeof(out),
            "OK battery %d%% charging=%d plugged=%d minutes=%d temp=%d.%02dC\n",
            scePowerGetBatteryLifePercent(), scePowerIsBatteryCharging(),
            scePowerIsPowerOnline(), scePowerGetBatteryLifeTime(),
            scePowerGetBatteryTemp() / 100, scePowerGetBatteryTemp() % 100);
        say(s, out);
    } else if (starts(cmd, "put")) {
        /* put <size> <path>: size first, because the path may contain spaces. */
        p = cmd + 3;
        while (*p == ' ') ++p;
        unsigned long long size = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 13) { size = size * 10 + (unsigned int)(*p++ - '0'); ++digits; }
        const char *path = digits && *p == ' ' ? path_arg(p) : NULL;
        if (!path) say(s, "ERR put <size> <path>\n");
        else if (xfer_start(s, 1, size, path) == 0) return 1;
        else say(s, "ERR busy: transfers running, retry\n");
    } else if (starts(cmd, "stat")) {
        const char *path = path_arg(cmd + 4);
        if (!path) say(s, "ERR stat <path>\n");
        else { fo_stat_line(path, out, sizeof(out)); say(s, out); }
    } else if (starts(cmd, "mkdir")) {
        const char *path = path_arg(cmd + 5);
        int rc = path ? fo_mkdirs(path) : -1;
        sceClibSnprintf(out, sizeof(out), rc < 0 ? "ERR mkdir rc=0x%08X\n" : "OK mkdir\n", rc);
        say(s, out);
    } else if (starts(cmd, "cp") || starts(cmd, "mv") || starts(cmd, "rm") || starts(cmd, "sha256")) {
        int kind = cmd[0] == 'c' ? FO_JOB_COPY : cmd[0] == 'm' ? FO_JOB_MOVE :
                   cmd[0] == 'r' ? FO_JOB_REMOVE : FO_JOB_SHA256;
        static char a[600], b[600];
        const char *arg = cmd + (kind == FO_JOB_SHA256 ? 6 : 2);
        while (*arg == ' ') ++arg;
        int n = 0;
        while (arg[n] && arg[n] != '\t' && n < (int)sizeof(a) - 1) { a[n] = arg[n]; ++n; }
        a[n] = 0;
        b[0] = 0;
        if (arg[n] == '\t') {
            int m = 0;
            for (const char *q = arg + n + 1; *q && m < (int)sizeof(b) - 1; ++q) b[m++] = *q;
            b[m] = 0;
        }
        int two = kind == FO_JOB_COPY || kind == FO_JOB_MOVE;
        const char *src = path_arg(a), *dst = two ? path_arg(b) : NULL;
        if (!src || (two && !dst) || (!two && b[0])) {
            say(s, two ? "ERR cp|mv <src><TAB><dst>\n" : "ERR rm|sha256 <path>\n");
        } else {
            int rc = fo_job_start(kind, src, dst);
            sceClibSnprintf(out, sizeof(out), rc < 0 ? "ERR job rc=0x%08X\n" : "OK job %d started\n", rc);
            say(s, out);
        }
    } else if (starts(cmd, "fetch")) {
        static char url[600], dst[600], sha[80];
        const char *q = cmd + 5;
        while (*q == ' ') ++q;
        char *fields[3] = {url, dst, sha};
        int sizes[3] = {sizeof(url), sizeof(dst), sizeof(sha)};
        for (int f = 0; f < 3; ++f) {
            int m = 0;
            while (*q && *q != '\t' && m < sizes[f] - 1) fields[f][m++] = *q++;
            fields[f][m] = 0;
            if (*q == '\t') ++q;
        }
        const char *path = path_arg(dst);
        if (!url[0] || !path) say(s, "ERR fetch <url><TAB><path>[<TAB><sha256>]\n");
        else {
            int rc = fo_job_start_fetch(url, path, sha[0] ? sha : NULL);
            sceClibSnprintf(out, sizeof(out), rc < 0 ? "ERR job rc=0x%08X\n" : "OK job %d started\n", rc);
            say(s, out);
        }
    } else if (starts(cmd, "job")) {
        p = cmd + 3;
        while (*p == ' ') ++p;
        if (starts(p, "cancel")) say(s, fo_job_cancel() < 0 ? "ERR job: nothing running\n" : "OK job cancelling\n");
        else { fo_job_status(out, sizeof(out)); say(s, out); }
    } else if (starts(cmd, "ls") || starts(cmd, "get")) {
        const char *path = path_arg(cmd + (cmd[0] == 'l' ? 2 : 3));
        if (!path) say(s, "ERR path must be like ux0:dir/file, no ..\n");
        else if (cmd[0] == 'l') cmd_ls(s, path);
        else if (xfer_start(s, 0, 0, path) == 0) return 1;
        else say(s, "ERR busy: transfers running, retry\n");
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
    return 0;
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
            if (!handle(c)) sceNetSocketClose(c);
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
    /* Nothing may still run our code when it unloads. */
    if (fo_shutdown() < 0) return SCE_KERNEL_STOP_FAIL;
    for (int i = 0; i < 400 && xfers > 0; ++i) sceKernelDelayThread(50 * 1000);
    if (xfers > 0) return SCE_KERNEL_STOP_FAIL;
    running = 0;
    SceUInt timeout = 15000000;
    if (sceKernelWaitThreadEnd(thread, NULL, &timeout) < 0) { running = 1; return SCE_KERNEL_STOP_FAIL; }
    sceKernelDeleteThread(thread);
    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
