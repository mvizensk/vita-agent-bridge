/* vakern 2.x: kernel half of the agent remote. Loaded at runtime by the
 * Recovery loader extension (never tai config), so a reboot removes it.
 *
 * Syscall provider plus SceCtrl/SceTouch hooks; no networking. The TCP server
 * is the user module background/vabridge.c, which this module injects into
 * SceShell (2.5; earlier it lived in the background app and got evicted). 1.x ran a kernel TCP server; SceNetPsForDriver buffer handling
 * (C0022005 / flag 0x1000 semantics) crashed the console three times.
 *
 * Every user pointer goes through the checked copies, so a bad argument is an
 * error code, never a kernel fault. Screen rows come from the owning process
 * via ksceKernelCopyFromUserProc (unmapped buffer = error, not fault). */
#include <psp2kern/ctrl.h>
#include <psp2kern/display.h>
#include <psp2kern/appmgr.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/processmgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2/touch.h>
#include <taihen.h>
#include "vakern.h"

#define ERR_ARG ((int)0x80AA0001)
#define ERR_PLANE ((int)0x80AA0002)
#define ERR_BUSY ((int)0x80AA0003)

/* SCE_CTRL_POWER (0x40000000) and anything above the known buttons is refused. */
#define ALLOWED_BUTTONS 0x0031FFFFu

static SceUID lock = -1;
static SceUID tick_thread = -1;
static volatile int running, awake = 1;
static unsigned char kchunk[VAK_MAX_READ];
static void ensure_shell_bridge(void);
static int kill_foreground(void);
static void vlog(const char *fmt, int a, int b);

static int plane_info(int plane, SceDisplayFrameBufInfo *info) {
    if (plane != 0 && plane != 1) return ERR_PLANE;
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    int rc = ksceDisplayGetProcFrameBufInternal(-1, ksceDisplayGetPrimaryHead(), plane, info);
    if (rc < 0) return rc;
    if (!info->framebuf.base || !info->framebuf.width || !info->framebuf.height) return ERR_PLANE;
    return 0;
}

int vakGetVersion(void) { return VAK_VERSION; }

int vakGetPlaneInfo(int plane, VakInfo *u_out) {
    SceDisplayFrameBufInfo info;
    VakInfo k;
    memset(&k, 0, sizeof(k));
    int rc = plane_info(plane, &info);
    k.result = rc;
    if (rc == 0) {
        k.pid = info.pid;
        k.width = info.framebuf.width;
        k.height = info.framebuf.height;
        k.pitch = info.framebuf.pitch;
        k.format = info.framebuf.pixelformat;
    }
    k.shell_pid = ksceKernelSysrootGetShellPid();
    int c = ksceKernelCopyToUser(u_out, &k, sizeof(k));
    return c < 0 ? c : rc;
}

/* Copies rows [y0, y0+rows) of a plane into the caller's buffer.
 * step 1 = full res, 2 = every other pixel of every other row. */
int vakReadRows(const VakRead *u_args) {
    VakRead a;
    int rc = ksceKernelCopyFromUser(&a, u_args, sizeof(a));
    if (rc < 0) return rc;
    if ((a.step != 1 && a.step != 2) || !a.dst || !a.rows) return ERR_ARG;
    SceDisplayFrameBufInfo info;
    rc = plane_info(a.plane, &info);
    if (rc < 0) return rc;
    unsigned int w = info.framebuf.width, h = info.framebuf.height, pitch = info.framebuf.pitch;
    if (w > 1920 || h > 1088 || pitch < w || pitch > 2048) return ERR_PLANE;
    unsigned int out_w = w / a.step, row_bytes = w * 4, out_row = out_w * 4;
    if (row_bytes > VAK_MAX_READ || (unsigned long long)out_row * a.rows > VAK_MAX_READ) return ERR_ARG;
    if (a.y0 >= h / a.step || a.y0 + a.rows > h / a.step) return ERR_ARG;
    if (ksceKernelLockMutex(lock, 1, NULL) < 0) return ERR_BUSY;
    const unsigned char *base = info.framebuf.base;
    unsigned char *dst = a.dst;
    for (unsigned int r = 0; r < a.rows && rc >= 0; ++r) {
        unsigned int y = (a.y0 + r) * a.step;
        rc = ksceKernelCopyFromUserProc(info.pid, kchunk, base + y * pitch * 4, row_bytes);
        if (rc < 0) break;
        if (a.step == 2)
            for (unsigned int x = 0; x < out_w; ++x) memcpy(kchunk + x * 4, kchunk + x * 8, 4);
        rc = ksceKernelCopyToUser(dst + r * out_row, kchunk, out_row);
    }
    ksceKernelUnlockMutex(lock, 1);
    return rc < 0 ? rc : (int)(a.rows * out_row);
}

/* 2.1: syscalls only record state; the kernel input thread below applies it
 * every 8ms, like vitacompanion/ds4vita. In 2.0 the emulation call was made
 * from the bg app's syscall context and SceShell ignored every button except
 * PS. `samples` is kept for ABI: 0 releases, anything else presses. */
static volatile unsigned int btn_state, analog_state = 0x80808080, analog_active;

int vakButtons(unsigned int mask, unsigned int samples) {
    if ((mask & ~ALLOWED_BUTTONS) || samples > 64) return ERR_ARG;
    btn_state = samples ? mask : 0;
    return 0;
}

int vakAnalog(unsigned int packed_lxlyrxry, unsigned int samples) {
    if (samples > 64) return ERR_ARG;
    analog_state = packed_lxlyrxry;
    analog_active = samples != 0;
    return 0;
}

int vakAwake(int on) {
    if (on != 0 && on != 1) return ERR_ARG;
    awake = on;
    return 0;
}

/* Kills the foreground app plane's owner, never SceShell. */
static int kill_foreground(void) {
    SceDisplayFrameBufInfo info;
    int rc = plane_info(0, &info);
    if (rc < 0) return rc;
    if (info.pid <= 0 || info.pid == ksceKernelSysrootGetShellPid()) return ERR_ARG;
    return ksceAppMgrKillProcess(info.pid);
}

int vakKillForeground(void) {
    return kill_foreground();
}

static void ensure_shell_bridge(void);
static void bridge_reload_now(void);
static volatile int reload_req;

static int tick_main(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned int last_buttons = 0, ticks = 0, was_analog = 0, bridge_ticks = 500; /* first try ~1 s in */
    unsigned int ps_held = 0;      /* 3.2: hold PS to close a game (hub comes back) */
    unsigned int ps_close_in = 0;  /* 3.3: tap PS afterwards to shut the shell menu */
    unsigned int ps_close_for = 0;
    while (running) {
        unsigned int b = btn_state;
        /* The PS press that triggered the quit also opened the shell's menu;
         * a second tap closes it once the app is gone. */
        if (ps_close_in && !--ps_close_in) ps_close_for = 6;
        if (ps_close_for) { b |= 0x10000u; --ps_close_for; }
        /* PS (0x10000, SCE_CTRL_INTERCEPTED) goes in both masks, as ds4vita does. */
        if (b || last_buttons) ksceCtrlSetButtonEmulation(0, 0, b, b, 32);
        last_buttons = b;
        if (analog_active) {
            unsigned int p = analog_state;
            unsigned char lx = p >> 24, ly = p >> 16, rx = p >> 8, ry = p;
            ksceCtrlSetAnalogEmulation(0, 0, lx, ly, rx, ry, lx, ly, rx, ry, 32);
            was_analog = 1;
        } else if (was_analog) {
            ksceCtrlSetAnalogEmulation(0, 0, 128, 128, 128, 128, 128, 128, 128, 128, 0);
            was_analog = 0;
        }
        if (b && awake) ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        if (++ticks >= 60) { ticks = 0; if (awake) ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); }
        /* PS alone opens the shell menu; holding it ~1.2 s quits to the hub. */
        SceCtrlData kpad;
        if (ksceCtrlPeekBufferPositive(0, &kpad, 1) > 0) {
            if (kpad.buttons & 0x10000u) {
                if (++ps_held == 150 && kill_foreground() >= 0) ps_close_in = 90;  /* ~0.7 s later */
            } else ps_held = 0;
        }
        if (reload_req) { reload_req = 0; bridge_reload_now(); }
        if (++bridge_ticks >= 625) { bridge_ticks = 0; ensure_shell_bridge(); } /* ~5 s */
        ksceKernelDelayThread(8000);
    }
    ksceCtrlSetButtonEmulation(0, 0, 0, 0, 32);
    return 0;
}

/* 2.2: SceShell's UI ignores ksceCtrlSetButtonEmulation for everything but PS
 * (seen live with 2.0 and 2.1; apps DO see it). ds4vita-style hooks on the
 * SceCtrl syscall exports OR our buttons into what SceShell reads. Only the
 * shell's reads are touched; copies use the checked user<->kernel helpers,
 * exactly like ds4vita's patch_analogdata. */
/* 2.3: 2.2's four "2"-variant hooks installed but the shell still ignored
 * input, so hook all 12 SceCtrl user read exports and count who calls what
 * (vakDebug) to find the shell's real input path. NIDs: vita-headers db/360. */
#define HOOKS 12
static const unsigned int hook_nids[HOOKS] = {
    0xA9C3CED6, /* 0 sceCtrlPeekBufferPositive */
    0x15F81E8C, /* 1 sceCtrlPeekBufferPositive2 */
    0xA59454D3, /* 2 sceCtrlPeekBufferPositiveExt */
    0x860BF292, /* 3 sceCtrlPeekBufferPositiveExt2 */
    0x67E7AB83, /* 4 sceCtrlReadBufferPositive */
    0xC4226A3E, /* 5 sceCtrlReadBufferPositive2 */
    0xE2D99296, /* 6 sceCtrlReadBufferPositiveExt */
    0xA7178860, /* 7 sceCtrlReadBufferPositiveExt2 */
    0x104ED1A7, /* 8 sceCtrlPeekBufferNegative */
    0x81A89660, /* 9 sceCtrlPeekBufferNegative2 */
    0x15F96FB0, /* 10 sceCtrlReadBufferNegative */
    0x27A0C5FB, /* 11 sceCtrlReadBufferNegative2 */
};
static SceUID hook_uid[HOOKS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static tai_hook_ref_t hook_ref[HOOKS];
static volatile unsigned int calls[HOOKS], shell_calls[HOOKS], patched[HOOKS];

static void patch_shell(int idx, int ret, SceCtrlData *u_data) {
    int shell = ksceKernelGetProcessId() == ksceKernelSysrootGetShellPid();
    calls[idx]++;
    if (shell) shell_calls[idx]++;
    unsigned int b = btn_state & ~0x10000u; /* PS already reaches the shell */
    if (ret <= 0 || !b || !shell) return;
    if (ret > 64) ret = 64;
    for (int i = 0; i < ret; ++i) {
        SceCtrlData k;
        if (ksceKernelMemcpyUserToKernel(&k, u_data + i, sizeof(k)) < 0) return;
        if (idx >= 8) k.buttons &= ~b; else k.buttons |= b; /* negative = active-low */
        if (ksceKernelMemcpyKernelToUser(u_data + i, &k, sizeof(k)) < 0) return;
    }
    patched[idx]++;
}

/* 2.4: SceShell polls sceCtrlPeekBufferPositive2 (~450/s) and 2.3 patched
 * circle/cross into it, yet the LiveArea still ignored them. The shell is
 * touch-first, so inject front-panel touch the way ds4vita does: hook the
 * kernel ksceTouch* exports (pData is a kernel buffer there) and add one
 * synthetic report while a touch is active. Coordinates are panel units
 * (1920x1088 front). */
#define THOOKS 4
static const unsigned int thook_nids[THOOKS] = {
    0xBAD1960B, /* ksceTouchPeek */
    0x9B3F7207, /* ksceTouchPeekRegion */
    0x70C8AACE, /* ksceTouchRead */
    0x9A91F624, /* ksceTouchReadRegion */
};
static SceUID thook_uid[THOOKS] = {-1, -1, -1, -1};
static tai_hook_ref_t thook_ref[THOOKS];
static volatile unsigned int touch_on, touch_x, touch_y, touch_calls[THOOKS], touch_patched;

static void patch_touch(int idx, int ret, unsigned int port, SceTouchData *data) {
    touch_calls[idx]++;
    if (!touch_on || port != 0 || ret <= 0 || !data) return;
    if (ret > 64) ret = 64;
    for (int i = 0; i < ret; ++i) {
        SceTouchReport *r = &data[i].report[0];
        memset(r, 0, sizeof(*r));
        r->id = 0x70;
        r->force = 128;
        r->x = touch_x;
        r->y = touch_y;
        data[i].reportNum = 1;
    }
    touch_patched++;
}

static int thook_0(SceUInt32 port, SceTouchData *d, SceUInt32 n) {
    int ret = TAI_CONTINUE(int, thook_ref[0], port, d, n); patch_touch(0, ret, port, d); return ret; }
static int thook_1(SceUInt32 port, SceTouchData *d, SceUInt32 n, int region) {
    int ret = TAI_CONTINUE(int, thook_ref[1], port, d, n, region); patch_touch(1, ret, port, d); return ret; }
static int thook_2(SceUInt32 port, SceTouchData *d, SceUInt32 n) {
    int ret = TAI_CONTINUE(int, thook_ref[2], port, d, n); patch_touch(2, ret, port, d); return ret; }
static int thook_3(SceUInt32 port, SceTouchData *d, SceUInt32 n, int region) {
    int ret = TAI_CONTINUE(int, thook_ref[3], port, d, n, region); patch_touch(3, ret, port, d); return ret; }
static const void *thook_fn[THOOKS] = {thook_0, thook_1, thook_2, thook_3};

int vakTouch(unsigned int x, unsigned int y, int active) {
    if (x >= 1920 || y >= 1088 || (active != 0 && active != 1)) return ERR_ARG;
    touch_x = x; touch_y = y; touch_on = active;
    return 0;
}

int vakDebug(unsigned int *u_out) {
    unsigned int k[HOOKS * 3 + THOOKS + 1];
    for (int i = 0; i < HOOKS; ++i) { k[i] = calls[i]; k[HOOKS + i] = shell_calls[i]; k[2 * HOOKS + i] = patched[i]; }
    for (int i = 0; i < THOOKS; ++i) k[3 * HOOKS + i] = touch_calls[i];
    k[3 * HOOKS + THOOKS] = touch_patched;
    return ksceKernelCopyToUser(u_out, k, sizeof(k));
}

#define CTRL_HOOK(i) \
    static int hook_##i(int port, SceCtrlData *data, int count) { \
        int ret = TAI_CONTINUE(int, hook_ref[i], port, data, count); \
        patch_shell(i, ret, data); \
        return ret; \
    }
CTRL_HOOK(0) CTRL_HOOK(1) CTRL_HOOK(2) CTRL_HOOK(3) CTRL_HOOK(4) CTRL_HOOK(5)
CTRL_HOOK(6) CTRL_HOOK(7) CTRL_HOOK(8) CTRL_HOOK(9) CTRL_HOOK(10) CTRL_HOOK(11)
static const void *hook_fn[HOOKS] = {hook_0, hook_1, hook_2, hook_3, hook_4, hook_5,
    hook_6, hook_7, hook_8, hook_9, hook_10, hook_11};

static void release_hooks(void) {
    for (int i = 0; i < HOOKS; ++i)
        if (hook_uid[i] >= 0) { taiHookReleaseForKernel(hook_uid[i], hook_ref[i]); hook_uid[i] = -1; }
    for (int i = 0; i < THOOKS; ++i)
        if (thook_uid[i] >= 0) { taiHookReleaseForKernel(thook_uid[i], thook_ref[i]); thook_uid[i] = -1; }
}

/* 2.5: host the TCP bridge inside SceShell. In the background app it was
 * evicted the moment RetroArch launched (2026-09-19), taking all remote
 * control with it; SceShell is never evicted. Injected from the tick thread
 * (after module_start returns, so our syscall library is registered), and
 * re-injected if SceShell's pid ever changes. Runtime only, never tai config. */
static SceUID shell_bridge = -1, shell_bridge_pid = -1;
static int shell_bridge_rc;

static void vlog(const char *fmt, int a, int b) {
    SceUID fd = ksceIoOpen("ur0:data/vita-agent-bridge/kernel.log", SCE_O_CREAT | SCE_O_APPEND | SCE_O_WRONLY, 0666);
    if (fd >= 0) {
        char line[128];
        int n = snprintf(line, sizeof(line), fmt, a, b);
        ksceIoWrite(fd, line, n);
        ksceIoClose(fd);
    }
}

/* 2.6: before the first injection into a shell, stop+unload any bridge an
 * older vakern injected (same module name), so bridge updates need no reboot.
 * The bridge's module_stop waits for its threads, so this is a clean unload. */
static void unload_old_bridges(SceUID pid) {
    static SceUID ids[256];
    SceSize num = 256;
    if (ksceKernelGetModuleList(pid, 0x7FFFFFFF, 1, ids, &num) < 0) return;
    for (SceSize i = 0; i < num; ++i) {
        SceKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ksceKernelGetModuleInfo(pid, ids[i], &info) < 0) continue;
        if (strncmp(info.module_name, "VitaAgentBridge", 15) != 0) continue;
        int status = -1;
        int rc = ksceKernelStopUnloadModuleForPid(pid, ids[i], 0, NULL, 0, NULL, &status);
        vlog("old bridge unload=%08X status=%08X\n", rc, status);
    }
}

static void ensure_shell_bridge(void) {
    SceUID pid = ksceKernelSysrootGetShellPid();
    if (pid <= 0 || (shell_bridge >= 0 && pid == shell_bridge_pid)) return;
    if (pid != shell_bridge_pid) unload_old_bridges(pid);
    int status = -1;
    shell_bridge = ksceKernelLoadStartModuleForPid(pid, "ur0:data/vita-agent-bridge/vabridge.suprx", 0, NULL, 0, NULL, &status);
    shell_bridge_pid = pid;
    shell_bridge_rc = shell_bridge;
    vlog("shell bridge load=%08X status=%08X\n", shell_bridge, status);
}

/* Forget the current bridge so ensure_shell_bridge unloads every
 * VitaAgentBridge by name and injects the file on disk. Tick thread only. */
static void bridge_reload_now(void) {
    shell_bridge = -1;
    shell_bridge_pid = -1;
    ensure_shell_bridge();
}

int vakShellBridge(int action) {
    if (action == 0) return shell_bridge_rc;
    if (action == 1) {
        /* 3.1: the swap runs on the tick thread. Stop-unloading SceShell
         * modules from inside a syscall fails with C0028021 (seen 2026-09-19);
         * from our own kernel thread it works. */
        reload_req = 1;
        return 0;
    }
    return ERR_ARG;
}

int module_start(SceSize args, void *argp) {
    (void)args; (void)argp;
    lock = ksceKernelCreateMutex("vakern_lock", 0, 0, NULL);
    if (lock < 0) return SCE_KERNEL_START_FAILED;
    for (int i = 0; i < HOOKS; ++i)
        hook_uid[i] = taiHookFunctionExportForKernel(KERNEL_PID, &hook_ref[i], "SceCtrl",
            TAI_ANY_LIBRARY, hook_nids[i], hook_fn[i]);
    for (int i = 0; i < THOOKS; ++i)
        thook_uid[i] = taiHookFunctionExportForKernel(KERNEL_PID, &thook_ref[i], "SceTouch",
            TAI_ANY_LIBRARY, thook_nids[i], thook_fn[i]);
    SceUID fd = ksceIoOpen("ur0:data/vita-agent-bridge/kernel.log", SCE_O_CREAT | SCE_O_APPEND | SCE_O_WRONLY, 0666);
    if (fd >= 0) {
        char line[128];
        int ok = 0, bad = 0;
        for (int i = 0; i < HOOKS; ++i) { if (hook_uid[i] >= 0) ok++; else if (!bad) bad = hook_uid[i]; }
        for (int i = 0; i < THOOKS; ++i) { if (thook_uid[i] >= 0) ok++; else if (!bad) bad = thook_uid[i]; }
        int n = snprintf(line, sizeof(line), "vakern 0x%04X hooks ok=%d first_err=%08X\n", VAK_VERSION, ok, bad);
        ksceIoWrite(fd, line, n);
        ksceIoClose(fd);
    }
    running = 1;
    tick_thread = ksceKernelCreateThread("vakern_tick", tick_main, 0x60, 0x1000, 0, 0, NULL);
    if (tick_thread < 0 || ksceKernelStartThread(tick_thread, 0, NULL) < 0) {
        running = 0;
        if (tick_thread >= 0) ksceKernelDeleteThread(tick_thread);
        release_hooks();
        ksceKernelDeleteMutex(lock);
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
    (void)args; (void)argp;
    release_hooks();
    running = 0;
    SceUInt timeout = 3000000;
    if (ksceKernelWaitThreadEnd(tick_thread, NULL, &timeout) < 0) {
        running = 1; /* thread may still run our code: refuse unload */
        return SCE_KERNEL_STOP_FAIL;
    }
    ksceKernelDeleteThread(tick_thread);
    ksceCtrlSetButtonEmulation(0, 0, 0, 0, 1);
    ksceKernelDeleteMutex(lock);
    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
