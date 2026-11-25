/* Audio packs (docs/audio-packs.md): community replacement songs and sound effects, decoded and mixed by the page
 * (web/src/game/audiopack/, Module.portAudioPack) and hooked into the game by patches/0027-port-audio-pack.patch.
 *
 * Songs: the game's sequence keeps playing (timing is unchanged) but its volume is forced to 0 while a pack replaces
 * it; the page streams the replacement into the final mix (portPackMix) following the per-player state in sSeq.
 * Sound effects: audioInit swaps a replaced sound's ALSound for a copy in sSlots whose RAW16 wave lives in the page's
 * PCM pool, reached through ROM-space offsets >= PORT_PACK_PCM_BASE (portRomCopy -> portPackRead), so the game's own
 * synthesizer still pitches, pans and spatialises it.
 * Without Module.portAudioPack every bridge answers "no pack" and every hook is a no-op. */
#include "port_internal.h"
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

EM_JS(int, port_pack_sfx_count, (void), {
    var p = Module.portAudioPack;
    return p ? p.sfxCount() : 0;
});

EM_JS(int, port_pack_sfx_id, (int i), {
    var p = Module.portAudioPack;
    return p ? p.sfxId(i) : 0;
});

EM_JS(int, port_pack_sfx_bake, (int i, double rate, u32 origLoopCount, u32 *out), {
    var p = Module.portAudioPack;
    if (!p) {
        return 0;
    }
    return p.bakeSfx(i, rate, origLoopCount >>> 0, HEAPU32, out >> 2) ? 1 : 0;
});

EM_JS(void, port_pack_read, (void *dst, u32 off, u32 len), {
    var p = Module.portAudioPack;
    if (!p) {
        HEAPU8.fill(0, dst, dst + (len >>> 0));
        return;
    }
    p.read(HEAPU8, dst, off >>> 0, len >>> 0);
});

EM_JS(int, port_pack_has_song, (int id), {
    var p = Module.portAudioPack;
    return p && p.hasSong(id) ? 1 : 0;
});

EM_JS(int, port_pack_active, (void), {
    var p = Module.portAudioPack;
    return p && p.songCount() > 0 ? 1 : 0;
});

/* views are built per call: memory growth detaches old ones, and HEAP16/HEAPF32 need not be exported */
EM_JS(void, port_pack_mix, (s16 *buf, u32 frames, int rate, void *state), {
    var p = Module.portAudioPack;
    if (!p) {
        return;
    }
    var b = HEAPU8.buffer;
    p.mix(new Int16Array(b), new Int32Array(b), new Float32Array(b), buf >> 1, frames >>> 0, rate, state >> 2);
});

/* per sequence player, read by the page's mixer: 16 bytes each */
typedef struct {
    s32 id;    /* sequence the game started on this player (gSeqCurrentId) */
    u32 gen;   /* bumped on every (re)start: the replacement restarts */
    s32 state; /* alCSPGetState: 0 = stopped */
    f32 level; /* music volume x fade, 0..1 (before the sequence's own base volume) */
} PackSeq;
static PackSeq sSeq[4];

#define SLOT_COUNT 4096
#define MIX_MAX_FRAMES 4096
static u8 sSlots[SLOT_COUNT][PORT_PACK_SFX_SLOT] __attribute__((aligned(8)));
static u32 sSlotsUsed;
static s16 sMix[MIX_MAX_FRAMES * 2];

static struct {
    void *bank;
    s32 idBase;
} sBanks[2];
static s32 sBankCount;
static s32 sRecentSfx[8]; /* newest first; 0 = empty */

s32 portPackSfxCount(void) {
    return port_pack_sfx_count();
}

s32 portPackSfxId(s32 i) {
    return port_pack_sfx_id(i);
}

s32 portPackSfxBake(s32 i, f32 rate, u32 origLoopCount, u32 *out5) {
    return port_pack_sfx_bake(i, (double)rate, origLoopCount, out5);
}

void *portPackSfxSlot(void) {
    static s32 warned;
    void *slot;
    if (sSlotsUsed >= SLOT_COUNT) {
        if (!warned) {
            warned = 1;
            printf("PORT: audio pack sound slots exhausted\n");
        }
        return NULL;
    }
    slot = sSlots[sSlotsUsed++];
    memset(slot, 0, PORT_PACK_SFX_SLOT);
    return slot;
}

void portPackRegisterBank(void *bank, s32 idBase) {
    s32 i;
    for (i = 0; i < sBankCount; i++) {
        if (sBanks[i].bank == bank) {
            sBanks[i].idBase = idBase;
            return;
        }
    }
    if (sBankCount < 2) {
        sBanks[sBankCount].bank = bank;
        sBanks[sBankCount].idBase = idBase;
        sBankCount++;
    }
}

void portPackNoteSfx(void *bank, s32 sndId) {
    s32 i;
    for (i = 0; i < sBankCount; i++) {
        if (sBanks[i].bank == bank) {
            memmove(&sRecentSfx[1], &sRecentSfx[0], sizeof sRecentSfx - sizeof sRecentSfx[0]);
            sRecentSfx[0] = sBanks[i].idBase + sndId;
            return;
        }
    }
}

void portPackSeqStart(s32 player, s32 seqId) {
    if (player >= 0 && player < 4) {
        sSeq[player].id = seqId;
        sSeq[player].gen++;
    }
}

s16 portPackSeqVolume(s32 player, s32 seqId, f32 level, s16 vol) {
    if (player >= 0 && player < 4) {
        sSeq[player].level = level;
    }
    return port_pack_has_song(seqId) ? 0 : vol;
}

void portPackSeqState(s32 player, s32 seqId, s32 state) {
    if (player >= 0 && player < 4) {
        sSeq[player].id = seqId;
        sSeq[player].state = state;
    }
}

void portPackRead(void *dst, u32 off, u32 len) {
    port_pack_read(dst, off, len);
}

const s16 *portPackMix(const s16 *in, u32 frames) {
    static s32 warned;
    if (!port_pack_active()) {
        return in;
    }
    if (frames > MIX_MAX_FRAMES) {
        if (!warned) {
            warned = 1;
            printf("PORT: audio pack mix skipped, %u frames\n", frames);
        }
        return in;
    }
    memcpy(sMix, in, frames * 4);
    port_pack_mix(sMix, frames, gPortAudioRate, sSeq);
    return sMix;
}

EMSCRIPTEN_KEEPALIVE const char *port_debug_audio(void) {
    static char buf[256];
    s32 i, n;
    n = snprintf(buf, sizeof buf, "{\"seq\":[");
    for (i = 0; i < 4; i++) {
        n += snprintf(buf + n, sizeof buf - n, "%s%d", i ? "," : "", sSeq[i].state ? (int)sSeq[i].id : -1);
    }
    n += snprintf(buf + n, sizeof buf - n, "],\"sfx\":[");
    for (i = 0; i < 8 && sRecentSfx[i]; i++) {
        n += snprintf(buf + n, sizeof buf - n, "%s%d", i ? "," : "", (int)sRecentSfx[i]);
    }
    snprintf(buf + n, sizeof buf - n, "]}");
    return buf;
}
