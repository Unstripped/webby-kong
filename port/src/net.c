/* Lockstep netplay: every client runs the same simulation; the only input point is the controller read
 * (osContGetReadData), which takes the pads of read gPortNetRead from a ring of host-ordered bundles (web/net.js,
 * headless --net-sim). The sim stalls in port_frame_step until the ring holds the next reads.
 * Sim state (gPortCoop*, gPortNetRead) lives in the snapshot; the session state below is kept across a restore. */
#include "port_internal.h"
#include <emscripten.h>
#include <string.h>

#define RING 64

/* ---- sim state ---- */
u8 gPortCoop;
u8 gPortCoopEnding;
u8 gPortCoopActiveMask;
u8 gPortCoopSlotKong[PORT_MAX_PLAYERS];
u8 gPortCoopPlayerSlot[PORT_MAX_PLAYERS];
u8 gPortCoopPlayers;
u8 gPortCoopRosterDirty;
u8 gPortCoopTrigger;
u32 gPortNetRead;

extern u8 gPlayerCharacterId[];
extern u8 gPlayerCount;

/* ---- session state (live) ---- */
typedef struct {
    u32 read;
    u8 valid, mask, joinSlot, joinKong;
    u32 pad[PORT_MAX_PLAYERS];
} NetBundle;

static s32 sActive;
static s32 sRole; /* 0 none, 1 host, 2 join */
static s32 sLocalSlot;
static s32 sDelay;
static s32 sMuted;
static NetBundle sRing[RING];
int gPortNetHashEvery;

int portNetQsRanges(QsRange *out) {
    int n = 0;
    out[n++] = QS_RANGE(sActive);
    out[n++] = QS_RANGE(sRole);
    out[n++] = QS_RANGE(sLocalSlot);
    out[n++] = QS_RANGE(sDelay);
    out[n++] = QS_RANGE(sMuted);
    out[n++] = QS_RANGE(sRing);
    out[n++] = QS_RANGE(gPortNetHashEvery);
    return n;
}

EM_JS(void, portNetJsLocalInput, (u32 read, u32 packed), {
    if (typeof window !== 'undefined' && window.dk64net) window.dk64net.onLocalInput(read, packed >>> 0);
});

EM_JS(void, portNetJsHash, (u32 read, u32 hash), {
    if (typeof window !== 'undefined' && window.dk64net) window.dk64net.onHash(read, hash >>> 0);
});

static u32 packPad(u16 button, s8 x, s8 y) {
    return ((u32)button << 16) | ((u32)(u8)x << 8) | (u32)(u8)y;
}

static void unpackPad(OSContPad *p, u32 v) {
    p->button = (u16)(v >> 16);
    p->stick_x = (s8)(u8)(v >> 8);
    p->stick_y = (s8)(u8)v;
    p->errno = 0;
}

static const NetBundle *bundleAt(u32 read) {
    const NetBundle *b = &sRing[read % RING];
    return b->valid && b->read == read ? b : NULL;
}

int portNetActive(void) {
    return sActive;
}

int portNetDelay(void) {
    return sDelay;
}

int portNetReadsAhead(void) {
    int n = 0;
    while (n < RING && bundleAt(gPortNetRead + (u32)n) != NULL) {
        n++;
    }
    return n;
}

/* a VI tick may read the pads; the next tick may read again before more bundles arrive */
int portNetTickAllowed(void) {
    return !sActive || (bundleAt(gPortNetRead) != NULL && bundleAt(gPortNetRead + 1) != NULL);
}

void portNetRingClear(void) {
    memset(sRing, 0, sizeof sRing);
}

void portNetFillPads(OSContPad *pad) {
    const NetBundle *b = bundleAt(gPortNetRead);
    u32 packed;
    int i;
    if (b == NULL) {
        portAbort("PORT: net read %u has no inputs", gPortNetRead);
    }
    if (gPortCoopActiveMask == 0) {
        gPortCoop = 1;
        gPortCoopActiveMask = 1;
        memset(gPortCoopSlotKong, 0xFF, sizeof gPortCoopSlotKong);
        gPortCoopSlotKong[0] = gPlayerCharacterId[0];
        gPortCoopPlayers = 1;
        gPortCoopPlayerSlot[0] = 0;
    }
    if (b->mask != gPortCoopActiveMask) {
        for (i = 0; i < PORT_MAX_PLAYERS; i++) {
            u8 bit = (u8)(1u << i);
            if ((b->mask & bit) && !(gPortCoopActiveMask & bit)) {
                /* headless --net-sim adds several slots in one bundle: the others get kong = slot */
                gPortCoopSlotKong[i] = i == b->joinSlot ? b->joinKong : (u8)i;
            } else if (!(b->mask & bit) && (gPortCoopActiveMask & bit)) {
                gPortCoopSlotKong[i] = 0xFF;
            }
        }
        gPortCoopActiveMask = b->mask;
        gPortCoopRosterDirty = 1;
    }
    for (i = 0; i < PORT_MAX_PLAYERS; i++) {
        if (i < gPortCoopPlayers && gPortCoopPlayerSlot[i] < PORT_MAX_PLAYERS) {
            unpackPad(&pad[i], b->pad[gPortCoopPlayerSlot[i]]);
        } else {
            memset(&pad[i], 0, sizeof pad[i]);
            pad[i].errno = 0;
        }
    }
    {
        u16 taps = portInputTakeTaps();
        packed = sMuted ? 0 : packPad(gPortPad.button | taps, gPortPad.stick_x, gPortPad.stick_y);
    }
    portNetJsLocalInput(gPortNetRead + (u32)sDelay, packed);
    if (gPortNetHashEvery > 0) {
        if (gPortNetRead % (u32)gPortNetHashEvery == 0) {
            printf("hash %u %08x\n", gPortNetRead, coopStateHash());
            fflush(stdout);
        }
    } else if (gPortNetRead % 30 == 0) {
        portNetJsHash(gPortNetRead, coopStateHash());
    }
    gPortNetRead++;
}

/* ---- web/net.js (and headless --net-sim) ---- */
EMSCRIPTEN_KEEPALIVE void port_net_begin(s32 role, s32 localSlot, s32 delay) {
    sActive = 1;
    sRole = role;
    sLocalSlot = localSlot;
    sDelay = delay;
    sMuted = 0;
    portNetRingClear();
    if (role == 2) {
        portSaveSetForeign();
    }
}

EMSCRIPTEN_KEEPALIVE void port_net_end(void) {
    if (sRole == 1 && gPortCoop) {
        if (gPlayerCount <= 1) {
            gPortCoop = 0;
            gPortCoopActiveMask = 0;
        } else {
            /* the next map load (coopPrepareMapLoad) returns to one player and leaves co-op */
            gPortCoopEnding = 1;
            gPortCoopActiveMask = 1;
            gPortCoopRosterDirty = 1;
        }
    }
    sActive = 0;
    sRole = 0;
    sMuted = 0;
    portNetRingClear();
}

EMSCRIPTEN_KEEPALIVE void port_net_push(u32 read, s32 mask, s32 joinSlot, s32 joinKong, u32 p0, u32 p1, u32 p2,
                                        u32 p3, u32 p4) {
    NetBundle *b;
    if (read < gPortNetRead) {
        return;
    }
    if (read - gPortNetRead >= RING) {
        portAbort("PORT: net read %u is %u reads ahead (ring %d)", read, read - gPortNetRead, RING);
    }
    b = &sRing[read % RING];
    b->read = read;
    b->valid = 1;
    b->mask = (u8)mask;
    b->joinSlot = (u8)joinSlot;
    b->joinKong = (u8)joinKong;
    b->pad[0] = p0;
    b->pad[1] = p1;
    b->pad[2] = p2;
    b->pad[3] = p3;
    b->pad[4] = p4;
}

EMSCRIPTEN_KEEPALIVE u32 port_net_read(void) {
    return gPortNetRead;
}

EMSCRIPTEN_KEEPALIVE void port_net_set_muted(s32 muted) {
    sMuted = muted != 0;
}

EMSCRIPTEN_KEEPALIVE s32 port_net_local_view(void) {
    int v;
    if (!sActive) {
        return 0;
    }
    for (v = 0; v < gPortCoopPlayers; v++) {
        if (gPortCoopPlayerSlot[v] == sLocalSlot) {
            return v;
        }
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE s32 port_coop_free_kongs(void) {
    return coopFreeKongs();
}

EMSCRIPTEN_KEEPALIVE s32 port_coop_slot_kong(s32 slot) {
    if (slot < 0 || slot >= PORT_MAX_PLAYERS || !(gPortCoopActiveMask & (1u << slot))) {
        return 0xFF;
    }
    return gPortCoopSlotKong[slot];
}
