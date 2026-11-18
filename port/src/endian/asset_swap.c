/* Byte-order fixups for assets loaded from ROM tables: called once per freshly loaded entry
 * (assetLoad sync path, or dmaDrainQueueAndFreeBuffer for queued loads). */
#include "port_internal.h"
#include "endian/asset_swap.h"

#ifndef PORT_STRICT
#define PORT_STRICT 1
#endif

/* headless --lax clears this to survey which tables a run touches */
int gPortAssetStrict = PORT_STRICT;
static u8 sSeenUnregistered[64];

/* table 26: inflated sizes of compressed tables' entries (u32 per index) */
static void swapU32Array(s32 index, u8 *buf, u32 size) {
    (void)index;
    portSwap32(buf, size / 4);
}

#define SWAP_NONE { 1, NULL }
#define SWAP_FN(f) { 2, f }

static const AssetSwapper sSwappers[64] = {
    [1] = SWAP_FN(portSwapAssetTable01),  /* map geometry */
    [2] = SWAP_FN(portSwapAssetTable02),  /* static collision (gzip entries: hooked after the game's gunzip) */
    [3] = SWAP_FN(portSwapAssetTable03),  /* collision cells (same) */
    [4] = SWAP_FN(portSwapAssetTable04),  /* model-2 objects */
    [5] = SWAP_FN(portSwapAssetTable05),  /* actor models */
    [7] = SWAP_NONE,                      /* textures */
    [8] = SWAP_FN(portSwapAssetTable08),  /* camera paths */
    [9] = SWAP_FN(portSwapAssetTable09),  /* per-map model/actor setup */
    [10] = SWAP_FN(portSwapAssetTable10), /* cutscene scripts */
    [11] = SWAP_FN(portSwapAssetTable11), /* animations (keyframe bitstream stays big-endian) */
    [12] = SWAP_FN(portSwapAssetTable12), /* resNode text/effect streams */
    [13] = SWAP_FN(portSwapAssetTable13), /* actor scripts */
    [14] = SWAP_NONE,                     /* font sheets / textures */
    [15] = SWAP_FN(portSwapAssetTable15), /* object paths */
    [16] = SWAP_FN(portSwapAssetTable16), /* zones and spawn nodes */
    [17] = SWAP_FN(portSwapAssetTable17), /* recorded input demos */
    [18] = SWAP_FN(portSwapAssetTable18), /* volume triggers */
    [19] = SWAP_FN(portSwapAssetTable19), /* misc, per index */
    [20] = SWAP_FN(portSwapAssetTable20), /* zone polygons */
    [21] = SWAP_FN(portSwapAssetTable21), /* waypoint paths */
    [22] = SWAP_FN(portSwapAssetTable22), /* critter groups */
    [23] = SWAP_FN(portSwapAssetTable23), /* spawn points */
    [24] = SWAP_FN(portSwapAssetTable24), /* race routes */
    [25] = SWAP_NONE,                     /* textures */
    [26] = SWAP_FN(swapU32Array),         /* inflated sizes */
};

int gPortAssetTrace; /* headless --trace-assets */

void portAssetSwap(s32 table, s32 index, void *buffer, u32 size) {
    if (gPortAssetTrace) {
        portLog("PORT: asset %d/%d at 0x%x-0x%x", (int)table, (int)index, (u32)buffer, (u32)buffer + size);
    }
    const AssetSwapper *s;
    if (table < 0 || table >= 64) {
        portAbort("PORT: asset table %d out of range", (int)table);
    }
    s = &sSwappers[table];
    switch (s->kind) {
    case 1:
        return;
    case 2:
        s->fn(index, buffer, size);
        return;
    default:
        if (gPortAssetStrict) {
            portAbort("PORT: asset table %d index %d has no swapper", (int)table, (int)index);
        }
        if (!sSeenUnregistered[table]) {
            sSeenUnregistered[table] = 1;
            portLog("PORT: (lax) asset table %d first load index %d size 0x%x", (int)table, (int)index, size);
        }
        return;
    }
}

typedef struct {
    s32 table, index;
    void *buffer;
    u32 size;
    int pending;
} QueuedAsset;

static QueuedAsset sQueued[0xC1];

void portAssetQueued(s32 slot, s32 table, s32 index, void *buffer, u32 size) {
    if (slot < 0 || slot >= (s32)(sizeof sQueued / sizeof sQueued[0])) {
        portAbort("PORT: asset DMA queue slot %d out of range", (int)slot);
    }
    sQueued[slot].table = table;
    sQueued[slot].index = index;
    sQueued[slot].buffer = buffer;
    sQueued[slot].size = size;
    sQueued[slot].pending = 1;
}

void portAssetDrained(s32 slot) {
    QueuedAsset *q = &sQueued[slot];
    if (q->pending) {
        q->pending = 0;
        portAssetSwap(q->table, q->index, q->buffer, q->size);
    }
}
