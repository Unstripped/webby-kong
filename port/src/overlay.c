/* Overlays are all linked in; "loading" one restores its pristine data and zeroes its bss.
 * Each overlay's objects are bracketed in link order by marker objects (CMake writes
 * ovl_marker_<ovl>_{begin,end}.c), because wasm clang ignores `#pragma clang section`. */
#include "port_internal.h"

int gPortCurrentOverlay;

#define OVL_LIST(X) X(multiplayer) X(minecart) X(bonus) X(race) X(critter) X(boss) X(arcade) X(jetpac) X(menu)

#define DECL(o)                                                                                           \
    extern char __dk_##o##_data_begin[], __dk_##o##_data_end[], __dk_##o##_bss_begin[], __dk_##o##_bss_end[];
OVL_LIST(DECL)

typedef struct {
    const char *name;
    char *dataBegin, *dataEnd, *bssBegin, *bssEnd;
    u8 *snapshot;
} OverlayRange;

#define ENTRY(o) { #o, __dk_##o##_data_begin, __dk_##o##_data_end, __dk_##o##_bss_begin, __dk_##o##_bss_end, NULL },
static OverlayRange sOverlays[] = { OVL_LIST(ENTRY) };
#define NUM_OVERLAYS ((int)(sizeof sOverlays / sizeof sOverlays[0]))

void portOverlayQsRanges(QsSink *out) {
    QS_KEEP(out, sOverlays);
}

void portOverlayInit(void) {
    int i;
    for (i = 0; i < NUM_OVERLAYS; i++) {
        OverlayRange *o = &sOverlays[i];
        size_t n;
        if (o->dataEnd < o->dataBegin || o->bssEnd < o->bssBegin) {
            portAbort("PORT: overlay %s markers out of order", o->name);
        }
        n = (size_t)(o->dataEnd - o->dataBegin);
        o->snapshot = malloc(n ? n : 1);
        memcpy(o->snapshot, o->dataBegin, n);
    }
}

static int overlayIndex(u8 id) {
    /* loadOverlay ids (src/global/15050.c) */
    switch (id) {
    case 2: return 8;  /* menu */
    case 3: return 0;  /* multiplayer */
    case 4: return 1;  /* minecart */
    case 5: return 3;  /* race */
    case 6: return 4;  /* critter */
    case 7: return 5;  /* boss */
    case 8: return 2;  /* bonus */
    case 9: return 6;  /* arcade */
    case 10: return 7; /* jetpac */
    default: return -1;
    }
}

void portOverlayActivate(u8 overlayId) {
    int i = overlayIndex(overlayId);
    gPortCurrentOverlay = overlayId;
    if (i >= 0) {
        OverlayRange *o = &sOverlays[i];
        memcpy(o->dataBegin, o->snapshot, (size_t)(o->dataEnd - o->dataBegin));
        memset(o->bssBegin, 0, (size_t)(o->bssEnd - o->bssBegin));
    }
}
