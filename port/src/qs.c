/* Quicksave: an exact snapshot is RDRAM plus the .data and .bss of every port and game object, bounded by the
 * qs marker objects that CMake links first and last (system libraries link after them and stay live).
 * Host-only statics inside those ranges (GL handles, SDL devices, malloc'd scratch) are stashed around a
 * restore; suspended fibers' asyncify rewind ids are remapped (thread.c).
 * Protocol: save_begin, copy the table's ranges out, save_end; load_begin, copy them in, load_end.
 * Nothing here keeps state across calls: a restore overwrites this file's statics too. */
#include "port_internal.h"
#include <emscripten.h>
#include <string.h>

extern char __qs_data_begin[], __qs_data_end[], __qs_bss_begin[], __qs_bss_end[];

PortResumePoint gPortResume;

#define MAX_KEEP 96

static void qsTable(u32 t[6]) {
    if (__qs_data_end < __qs_data_begin || __qs_bss_end < __qs_bss_begin || (u32)__qs_data_begin < 0x800000u) {
        portAbort("PORT: qs markers out of order");
    }
    t[0] = 0;
    t[1] = 0x800000u;
    t[2] = (u32)__qs_data_begin;
    t[3] = (u32)(__qs_data_end - __qs_data_begin);
    t[4] = (u32)__qs_bss_begin;
    t[5] = (u32)(__qs_bss_end - __qs_bss_begin);
}

static int collect(QsRange *r) {
    int n = 0;
    n += portMainQsRanges(r + n);
    n += portSaveQsRanges(r + n);
    n += portInputQsRanges(r + n);
    n += portAiQsRanges(r + n);
    n += portDebugQsRanges(r + n);
    n += portOverlayQsRanges(r + n);
    n += gfxTmemQsRanges(r + n);
    n += gfxBackendQsRanges(r + n);
    n += portNetQsRanges(r + n);
    if (n > MAX_KEEP) {
        portAbort("PORT: too many qs keep ranges (%d)", n);
    }
    return n;
}

/* {start, length} x3: RDRAM, .data, .bss */
EMSCRIPTEN_KEEPALIVE u32 *port_qs_table(void) {
    static u32 sTable[6];
    qsTable(sTable);
    return sTable;
}

EMSCRIPTEN_KEEPALIVE u32 port_qs_size(void) {
    u32 t[6];
    qsTable(t);
    return t[1] + t[3] + t[5];
}

/* 0: not at a frame boundary, or a fiber cannot be encoded */
EMSCRIPTEN_KEEPALIVE int port_qs_save_begin(void) {
    if (portInThread()) {
        return 0;
    }
    return portThreadsQsEncode();
}

EMSCRIPTEN_KEEPALIVE void port_qs_save_end(void) {
    portThreadsQsRestoreLive();
}

/* returns the stash of live host statics (malloc heap: outside the snapshot), NULL if not possible now */
EMSCRIPTEN_KEEPALIVE u8 *port_qs_load_begin(void) {
    QsRange r[MAX_KEEP];
    int n, i;
    u32 total = 0, o = 0;
    u8 *stash;
    if (portInThread()) {
        return NULL;
    }
    n = collect(r);
    for (i = 0; i < n; i++) {
        total += r[i].n;
    }
    stash = malloc(total ? total : 1);
    if (stash == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        memcpy(stash + o, r[i].p, r[i].n);
        o += r[i].n;
    }
    return stash;
}

/* 0: the snapshot's fibers cannot resume in this page (memory is already overwritten) */
EMSCRIPTEN_KEEPALIVE int port_qs_load_end(u8 *stash) {
    QsRange r[MAX_KEEP];
    int n, i;
    u32 o = 0;
    n = collect(r);
    for (i = 0; i < n; i++) {
        memcpy(r[i].p, stash + o, r[i].n);
        o += r[i].n;
    }
    free(stash);
    if (!portThreadsQsDecode()) {
        return 0;
    }
    gfxBackendQsRestored();
    portNetRingClear(); /* bundles were for reads of the session's timeline, not the snapshot's */
    portLog("PORT: quickload restored %u bytes", port_qs_size());
    return 1;
}

/* ---- resume point: the game starts it from mainThreadLoop once the EEPROM is read (src/global/450.c) ---- */
EMSCRIPTEN_KEEPALIVE void port_resume_request(s32 slot, s32 kong, s32 map, s32 exit, s32 hasPos, f32 x, f32 y, f32 z,
                                              s32 yaw) {
    gPortResume.slot = slot;
    gPortResume.kong = kong;
    gPortResume.map = map;
    gPortResume.exit = exit;
    gPortResume.hasPos = hasPos;
    gPortResume.x = x;
    gPortResume.y = y;
    gPortResume.z = z;
    gPortResume.yaw = yaw;
    gPortResume.pending = 1;
}

/* JSON: {"ok":true,"slot":..,"kong":..,"map":..,"exit":..,"hasPos":..,"x":..,"y":..,"z":..,"yaw":..} or
 * {"ok":false,"reason":"..."} */
EMSCRIPTEN_KEEPALIVE const char *port_resume_capture(void) {
    static char buf[256];
    PortResumePoint p;
    const char *reason = "";
    if (!mapCaptureResumePoint(&p, &reason)) {
        snprintf(buf, sizeof buf, "{\"ok\":false,\"reason\":\"%s\"}", reason);
    } else {
        snprintf(buf, sizeof buf,
                 "{\"ok\":true,\"slot\":%d,\"kong\":%d,\"map\":%d,\"exit\":%d,\"hasPos\":%d,\"x\":%.2f,\"y\":%.2f,"
                 "\"z\":%.2f,\"yaw\":%d}",
                 (int)p.slot, (int)p.kong, (int)p.map, (int)p.exit, (int)p.hasPos, p.x, p.y, p.z, (int)p.yaw);
    }
    return buf;
}
