/* Shared between kernel/vakern.c (syscall provider) and background/vabridge.c. */
#ifndef VAKERN_H
#define VAKERN_H

#define VAK_VERSION 0x0330
#define VAK_MAX_READ (32 * 1024)

typedef struct {
    int result;
    int pid;
    int shell_pid;
    unsigned int width, height, pitch, format;
} VakInfo;

/* Syscalls pass at most four register args; bigger requests use a struct. */
typedef struct {
    int plane;          /* 0 = foreground app, 1 = SceShell overlay */
    unsigned int step;  /* 1 full, 2 half resolution */
    unsigned int y0;    /* first output row */
    unsigned int rows;  /* output rows; rows * out_row_bytes <= VAK_MAX_READ */
    void *dst;          /* caller's user buffer */
} VakRead;

int vakGetVersion(void);
int vakGetPlaneInfo(int plane, VakInfo *out);
int vakReadRows(const VakRead *args);
int vakButtons(unsigned int mask, unsigned int samples);
int vakAnalog(unsigned int packed_lxlyrxry, unsigned int samples);
int vakAwake(int on);
int vakKillForeground(void);
/* 41 words: ctrl calls[12], shell_calls[12], patched[12]; touch calls[4], touch_patched. */
int vakDebug(unsigned int *out);
/* Front panel units 0..1919 x 0..1087; active 1 = finger down. */
int vakTouch(unsigned int x, unsigned int y, int active);
/* 0 = status (load rc), 1 = reload (never from the bridge itself), 2 = unload. */
int vakShellBridge(int action);

#endif
