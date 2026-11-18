#include "port_internal.h"
#include <emscripten.h>
#include <stdarg.h>

u32 gPortStatVi, gPortStatFrames, gPortStatGfxTasks, gPortStatAudTasks;
static char sLastError[256];
static s32 sPeakCur, sPeakLast;
static u32 sPeakTicks;

extern s32 gCurrentMap;
extern u8 gGameMode;

void portSetLastError(const char *msg) {
    snprintf(sLastError, sizeof sLastError, "%s", msg);
}

void portLog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

_Noreturn void portAbort(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    portSetLastError(buf);
    printf("%s\n", buf);
    fflush(stdout);
    abort();
}

void portAudioPeakFeed(const s16 *samples, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        s32 v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > sPeakCur) {
            sPeakCur = v;
        }
    }
}

/* called once per VI: the reported peak covers the last second */
void portAudioPeakTick(void) {
    if (++sPeakTicks >= 60) {
        sPeakLast = sPeakCur;
        sPeakCur = 0;
        sPeakTicks = 0;
    }
}

EMSCRIPTEN_KEEPALIVE const char *port_debug_stats(void) {
    static char buf[512];
    snprintf(buf, sizeof buf,
             "{\"vi\":%u,\"frames\":%u,\"gfxTasks\":%u,\"audTasks\":%u,\"map\":%d,\"mode\":%d,\"overlay\":%d,"
             "\"audioPeak\":%d,\"viFb\":\"0x%x\",\"viBlack\":%d,\"lastError\":\"%s\"}",
             gPortStatVi, gPortStatFrames, gPortStatGfxTasks, gPortStatAudTasks, (int)gCurrentMap, (int)gGameMode,
             gPortCurrentOverlay, (int)(sPeakLast > sPeakCur ? sPeakLast : sPeakCur), (u32)gPortViCurrentFb,
             gPortViBlack, sLastError);
    return buf;
}
