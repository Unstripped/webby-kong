/* Audio bank (.ctl) and compact-MIDI header byte order. Banks are walked through their still-relative
 * offsets before alBnkfNew turns them into pointers; every struct is swapped exactly once. */
#include "port_internal.h"

#define MAX_VISITED 8192

static u32 sVisited[MAX_VISITED];
static int sVisitedCount;
static int sRawWarned;

static int firstVisit(u32 off) {
    int i;
    for (i = 0; i < sVisitedCount; i++) {
        if (sVisited[i] == off) {
            return 0;
        }
    }
    if (sVisitedCount == MAX_VISITED) {
        portAbort("PORT: bank swap visited-set overflow");
    }
    sVisited[sVisitedCount++] = off;
    return 1;
}

static u16 sw16(u8 *p) {
    u16 v = portReadBE16(p);
    memcpy(p, &v, 2);
    return v;
}

static u32 sw32(u8 *p) {
    u32 v = portReadBE32(p);
    memcpy(p, &v, 4);
    return v;
}

static void swapBook(u8 *base, u32 off) {
    u8 *b = base + off;
    s32 order, npred, n, i;
    if (off == 0 || !firstVisit(off)) {
        return;
    }
    order = (s32)sw32(b);
    npred = (s32)sw32(b + 4);
    n = order * npred * 8;
    for (i = 0; i < n; i++) {
        sw16(b + 8 + i * 2);
    }
}

static void swapAdpcmLoop(u8 *base, u32 off) {
    u8 *l = base + off;
    int i;
    if (off == 0 || !firstVisit(off)) {
        return;
    }
    sw32(l);
    sw32(l + 4);
    sw32(l + 8);
    for (i = 0; i < 16; i++) {
        sw16(l + 12 + i * 2);
    }
}

static void swapRawLoop(u8 *base, u32 off) {
    u8 *l = base + off;
    if (off == 0 || !firstVisit(off)) {
        return;
    }
    sw32(l);
    sw32(l + 4);
    sw32(l + 8);
}

static void swapWaveTable(u8 *base, u32 off) {
    u8 *w = base + off;
    u32 loop, book;
    if (off == 0 || !firstVisit(off)) {
        return;
    }
    sw32(w);     /* base (ROM offset into the .tbl) */
    sw32(w + 4); /* len */
    /* +8 type, +9 flags: bytes */
    loop = sw32(w + 12);
    if (w[8] == AL_ADPCM_WAVE) {
        book = sw32(w + 16);
        swapAdpcmLoop(base, loop);
        swapBook(base, book);
    } else {
        if (!sRawWarned) {
            portLog("PORT: raw16 wave in bank");
            sRawWarned = 1;
        }
        swapRawLoop(base, loop);
    }
}

static void swapEnvelope(u8 *base, u32 off) {
    u8 *e = base + off;
    if (off == 0 || !firstVisit(off)) {
        return;
    }
    sw32(e);
    sw32(e + 4);
    sw32(e + 8);
}

static void swapSound(u8 *base, u32 off) {
    u8 *s = base + off;
    u32 env, wave;
    if (off == 0 || !firstVisit(off)) {
        return;
    }
    env = sw32(s);
    sw32(s + 4); /* keymap: bytes only */
    wave = sw32(s + 8);
    swapEnvelope(base, env);
    swapWaveTable(base, wave);
}

static void swapInstrument(u8 *base, u32 off) {
    u8 *in = base + off;
    s16 count;
    int i;
    if (off == 0 || !firstVisit(off)) {
        return;
    }
    sw16(in + 12); /* bendRange */
    count = (s16)sw16(in + 14);
    for (i = 0; i < count; i++) {
        swapSound(base, sw32(in + 16 + i * 4));
    }
}

static void swapBank(u8 *base, u32 off) {
    u8 *b = base + off;
    s16 count;
    int i;
    if (off == 0 || !firstVisit(off)) {
        return;
    }
    count = (s16)sw16(b);
    sw32(b + 4);                /* sampleRate */
    swapInstrument(base, sw32(b + 8)); /* percussion */
    for (i = 0; i < count; i++) {
        swapInstrument(base, sw32(b + 12 + i * 4));
    }
}

void portSwapBankFile(void *ctl) {
    u8 *f = ctl;
    s16 revision, count;
    int i;
    sVisitedCount = 0;
    revision = (s16)sw16(f);
    count = (s16)sw16(f + 2);
    if (revision != AL_BANK_VERSION) {
        portAbort("PORT: bank revision 0x%04x, expected 0x%04x", (unsigned)(u16)revision, AL_BANK_VERSION);
    }
    for (i = 0; i < count; i++) {
        swapBank(f, sw32(f + 4 + i * 4));
    }
    portLog("PORT: bank ok (%d banks, %d structs)", count, sVisitedCount);
}

/* ALCMidiHdr: u32 trackOffset[16]; u32 division */
void portSwapSeqHeader(void *hdr) {
    portSwap32(hdr, 17);
}
