/* Asset table 20: per-map zone polygons, indexed by map id (mapLoadNodeAsset -> mapNodeAssetParse, 55B20.c).
 * Uncompressed; only maps 0x07, 0x0B, 0x16, 0x1A and 0x1E have an entry in the US ROM (not on the intro path).
 *
 * The parser walks the entry as an s16 stream through `s16 *p` and copies the arrays into MapZone, where they
 * are read as s16 again (mapFaceBuildTriangleFan reads verts via `*(s16 *)(verts + off)`; enterCells/exitCells/
 * links are s16 arrays read by the zone visibility code in 55B20.c). Every field is therefore native s16:
 *   s16 zoneCount
 *   zoneCount x {
 *     s16 idx, s16 vertCount, s16 verts[vertCount * 3] (x, y, z),
 *     s16 height, s16 exitCount, s16 exitCells[16], s16 enterCount, s16 enterCells[16], s16 linkCount,
 *     s16 links[8]
 *   }
 * The stream ends exactly at the end of every entry in the US ROM. */
#include "endian/asset_swap.h"

#define ZONE_TAIL_HALVES (1 + 1 + 16 + 1 + 16 + 1 + 8)

void portSwapAssetTable20(s32 index, u8 *buf, u32 size) {
    s32 zones, i, vertCount;
    u32 p;

    if (buf == NULL || size == 0) {
        return;
    }
    assetCheck("t20 zone count", 0, 2, size);
    zones = (s16)asw16(buf, 0);
    p = 2;
    for (i = 0; i < zones; i++) {
        assetCheck("t20 zone head", p, 4, size);
        asw16(buf, p);
        vertCount = (s16)asw16(buf, p + 2);
        p += 4;
        if (vertCount < 0) {
            portAbort("PORT: asset table 20 index %d zone %d vertex count %d", (int)index, (int)i, (int)vertCount);
        }
        assetSwap16Range(buf, p, p + (u32)vertCount * 6, size);
        p += (u32)vertCount * 6;
        assetSwap16Range(buf, p, p + ZONE_TAIL_HALVES * 2, size);
        p += ZONE_TAIL_HALVES * 2;
    }
}
