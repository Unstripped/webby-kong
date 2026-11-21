#include "port_internal.h"
#include <emscripten.h>
#include <stdarg.h>
#include <string.h>

u32 gPortStatVi, gPortStatFrames, gPortStatGfxTasks, gPortStatAudTasks;
static char sLastError[256];
static s32 sPeakCur, sPeakLast;
static u32 sPeakTicks;

extern s32 gCurrentMap;
extern u8 gGameMode;
extern u8 *gPrimaryPlayerActor; /* Actor * in the game */
extern u8 gPlayerCount;
extern u16 gActiveActorCount;

/* Actor field offsets (build/tree/include/actor.h): actorType 0x58, x 0x7C, y 0x80, z 0x84, yaw 0xE6, chunkIndex 0x12C */
#define ACTOR_TYPE 0x58
#define ACTOR_X 0x7C
#define ACTOR_Y 0x80
#define ACTOR_Z 0x84
#define ACTOR_YAW 0xE6
#define ACTOR_CHUNK 0x12C

/* ---- trace ring: read by port_debug_trace, echoed to stdout by headless --trace ---- */
#define TRACE_RING 256
typedef struct {
    u32 seq;
    u32 vi;
    u32 cat;
    char text[120];
} TraceEvent;

u32 gPortTraceMask = PORT_TRACE_MAP | PORT_TRACE_CAM;
int gPortTracePrint;
static TraceEvent sTrace[TRACE_RING];
static u32 sTraceNext; /* seq the next event gets */

int portDebugQsRanges(QsRange *out) {
    int n = 0;
    out[n++] = QS_RANGE(sTrace);
    out[n++] = QS_RANGE(sTraceNext);
    out[n++] = QS_RANGE(gPortTraceMask);
    out[n++] = QS_RANGE(gPortTracePrint);
    out[n++] = QS_RANGE(gPortStatVi);
    out[n++] = QS_RANGE(gPortStatFrames);
    out[n++] = QS_RANGE(gPortStatGfxTasks);
    out[n++] = QS_RANGE(gPortStatAudTasks);
    out[n++] = QS_RANGE(sLastError);
    out[n++] = QS_RANGE(sPeakCur);
    out[n++] = QS_RANGE(sPeakLast);
    out[n++] = QS_RANGE(sPeakTicks);
    return n;
}

void portTrace(u32 cat, const char *fmt, ...) {
    TraceEvent *ev = &sTrace[sTraceNext % TRACE_RING];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev->text, sizeof ev->text, fmt, ap);
    va_end(ap);
    ev->seq = sTraceNext++;
    ev->vi = gPortStatVi;
    ev->cat = cat;
    if (gPortTracePrint) {
        printf("TRACE vi=%u %s\n", ev->vi, ev->text);
        fflush(stdout);
    }
}

/* JSON of events with seq >= since still in the ring; pass the returned "next" back as since */
EMSCRIPTEN_KEEPALIVE const char *port_debug_trace(u32 since) {
    static char buf[65536];
    u32 oldest = sTraceNext - (sTraceNext < TRACE_RING ? sTraceNext : TRACE_RING);
    u32 seq = since < oldest ? oldest : since > sTraceNext ? sTraceNext : since;
    size_t n;
    int first = 1;
    n = (size_t)snprintf(buf, sizeof buf, "{\"events\":[");
    /* worst-case event: ~70 header bytes + 119 text bytes each escaped to 6 */
    for (; seq < sTraceNext && sizeof buf - n >= 1024; seq++) {
        const TraceEvent *ev = &sTrace[seq % TRACE_RING];
        const char *s;
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s{\"seq\":%u,\"vi\":%u,\"cat\":%u,\"text\":\"", first ? "" : ",",
                              ev->seq, ev->vi, ev->cat);
        first = 0;
        for (s = ev->text; *s != '\0'; s++) {
            u8 c = (u8)*s;
            if (c == '"' || c == '\\') {
                buf[n++] = '\\';
                buf[n++] = (char)c;
            } else if (c < 0x20) {
                n += (size_t)snprintf(buf + n, sizeof buf - n, "\\u%04x", c);
            } else {
                buf[n++] = (char)c;
            }
        }
        buf[n++] = '"';
        buf[n++] = '}';
    }
    snprintf(buf + n, sizeof buf - n, "],\"next\":%u,\"dropped\":%s}", seq, since < oldest ? "true" : "false");
    return buf;
}

EMSCRIPTEN_KEEPALIVE u32 port_debug_trace_mask(s32 mask) {
    if (mask >= 0) {
        gPortTraceMask = (u32)mask;
    }
    return gPortTraceMask;
}

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
    static char buf[768];
    u8 *player = gPrimaryPlayerActor;
    int n = snprintf(buf, sizeof buf,
                     "{\"vi\":%u,\"frames\":%u,\"gfxTasks\":%u,\"audTasks\":%u,\"map\":%d,\"mode\":%d,\"overlay\":%d,"
                     "\"audioPeak\":%d,\"viFb\":\"0x%x\",\"viBlack\":%d,\"lastError\":\"%s\",\"actors\":%u,\"players\":%d,"
                     "\"player\":",
                     gPortStatVi, gPortStatFrames, gPortStatGfxTasks, gPortStatAudTasks, (int)gCurrentMap, (int)gGameMode,
                     gPortCurrentOverlay, (int)(sPeakLast > sPeakCur ? sPeakLast : sPeakCur), (u32)gPortViCurrentFb,
                     gPortViBlack, sLastError, (u32)gActiveActorCount, (int)gPlayerCount);
    if (PORT_IS_PTR(player)) {
        f32 x, y, z;
        s16 yaw, chunk;
        u32 type;
        if ((u32)player & 0x80000000u) {
            player = K0_HOST(player);
        }
        memcpy(&type, player + ACTOR_TYPE, sizeof type);
        memcpy(&x, player + ACTOR_X, sizeof x);
        memcpy(&y, player + ACTOR_Y, sizeof y);
        memcpy(&z, player + ACTOR_Z, sizeof z);
        memcpy(&yaw, player + ACTOR_YAW, sizeof yaw);
        memcpy(&chunk, player + ACTOR_CHUNK, sizeof chunk);
        snprintf(buf + n, sizeof buf - n, "{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"yaw\":%d,\"chunk\":%d,\"type\":%u}}", x, y,
                 z, (int)yaw, (int)chunk, type);
    } else {
        snprintf(buf + n, sizeof buf - n, "null}");
    }
    return buf;
}
