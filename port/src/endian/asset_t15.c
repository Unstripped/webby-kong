/* Table 15: per-map object paths. Loader: pathFreeAsset -> pathTableLoad (global/127B00.c), called from
 * mapLoadLevel (global/450.c); the copied points are read by pathPlaybackStart and the path playback
 * update code in 127B00.c through gPathTable.
 *
 * Layout:
 *   s16 pathCount
 *   per path: s16 pathId (index into gPathTable[0x1A]), s16 pointCount, s16 (skipped, never read),
 *             PathPoint[pointCount], 10 bytes each: s16 unk0, x, y, z; u8 speed, u8 unk9
 * Entries 2, 8 and 196 start with pathCount 0 followed by unrelated bytes the game never reads; only the
 * walked fields are swapped. */
#include "endian/asset_swap.h"

void portSwapAssetTable15(s32 index, u8 *buf, u32 size) {
    u32 o, n, i, j, points;
    (void)index;

    if (size == 0) {
        return;
    }
    assetCheck("t15 pathCount", 0, 2, size);
    n = (u16)asw16(buf, 0);
    o = 2;
    for (i = 0; i < n; i++) {
        assetCheck("t15 path header", o, 6, size);
        asw16(buf, o);
        points = (u16)asw16(buf, o + 2);
        asw16(buf, o + 4);
        o += 6;
        for (j = 0; j < points; j++, o += 10) {
            assetSwap16Range(buf, o, o + 8, size);
        }
    }
}
