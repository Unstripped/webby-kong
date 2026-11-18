/* Table 21: player waypoint paths (scripted walks), one entry per level index. Consumer:
 * playerWaypointPathsLoad -> playerWaypointPathsParse (global/F56F0.c), called from mapLoadLevel
 * (global/450.c) when gGameMode == 0; nodes are read by playerWaypointPathStart,
 * actorSetPositionAndFacingAngle and the waypoint follow code in F56F0.c.
 *
 * Layout (no padding):
 *   s16 pathCount
 *   per path: s16 nodeCount, WaypointNode[nodeCount], 0x12 bytes each:
 *     0x00 s16 x, y, z; 0x06 u8 timer0, timer1, timer2, speed; 0x0A s16 timer3, timer4, timer5, delay
 * Every ROM entry parses to exactly its length. */
#include "endian/asset_swap.h"

void portSwapAssetTable21(s32 index, u8 *buf, u32 size) {
    u32 o, n, i, j, nodes;
    (void)index;

    if (size == 0) {
        return;
    }
    assetCheck("t21 pathCount", 0, 2, size);
    n = (u16)asw16(buf, 0);
    o = 2;
    for (i = 0; i < n; i++) {
        assetCheck("t21 nodeCount", o, 2, size);
        nodes = (u16)asw16(buf, o);
        o += 2;
        for (j = 0; j < nodes; j++, o += 0x12) {
            assetSwap16Range(buf, o, o + 6, size);
            assetSwap16Range(buf, o + 0xA, o + 0x12, size);
        }
    }
}
