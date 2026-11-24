/* Asset table 16: per-map zones and spawn nodes, indexed by map. Loaded in mapLoadLevel (450.c) and parsed by
 * spawnNodeListInitFromMap (1295B0.c, via spawnNodesInitForMap) into gZoneTable / gSpawnNodeList; the buffer is freed
 * right after. Compressed by the asset table (sAssetTableCompressed[16] == 1), so assetLoad inflates it before the swap.
 *
 * Layout (sequential s16 stream; the walk ends exactly at the end of every map's entry in the US ROM):
 *   s16 zoneCount                                       -> ZoneTable.count
 *   zoneCount x Zone record:
 *     s16 vertexCount
 *     vertexCount x ZoneVertex { s16 x, y, z }          copied as ZoneVertex, read as s16 (min/max, sameY test)
 *     s16 entryCount
 *     entryCount x ZoneEntry (0xA) { s16 x, y, z; u8 flags, radius, height, pad }
 *                                                       zoneEntrySetDisabled/zoneEntryGetPosition and
 *                                                       the ZoneEntry users; bytes untouched
 *     s16 id, s16 unk19                                 read as s16 then truncated into Zone.id / Zone.unk19
 *   s16 nodeCount                                       -> SpawnNodeList.count
 *   nodeCount x spawn node:
 *     0x16-byte SpawnNodeRecord copied over SpawnNode 0x00..0x15 (actor.h):
 *       0x00 u8 baseEntryIndex, 0x01 u8 pad1           bytes
 *       0x02 s16 yaw, 0x04 s16 x, 0x06 s16 y, 0x08 s16 z   native s16
 *       0x0A unkA/unkB | s16 unkAWide                   CONFLICT, left in ROM byte order (see below)
 *       0x0C..0x15 u8 targetSpeed, unkD, zoneId, scale, actorState, pathCount, initialState, id,
 *                  respawnDelay, pad15[0]               bytes
 *     pathCount x u8[2] path step (SpawnNode.path)      bytes
 *
 * SpawnNode 0x0A conflict: read as two bytes (unkA/unkB) by bonus/0.c:120,130,467, 1295B0.c:597,
 * B7490.c:538-697 and BC3B0.c:221-294, and as one s16 (unkAWide) by 12E800.c:244-245
 * (behaviourPickRandomTargetY), BA790.c:242 (explodingChaserUpdate) and BC3B0.c:138-141,197-198
 * (actorUpdateLockOnCursor). ROM data holds both kinds (e.g. 0x2B00 byte pairs on map 0x50, 0x00D7 halfwords on
 * map 16). The field stays in ROM (big-endian) byte order so every byte reader is correct; the unkAWide readers
 * need a game patch reading `(s16)portReadBE16(&node->unkAWide)`. No code writes the field. */
#include "endian/asset_swap.h"

#define SPAWN_NODE_RECORD_SIZE 0x16
#define ZONE_ENTRY_SIZE 0xA

void portSwapAssetTable16(s32 index, u8 *buf, u32 size) {
    u32 o = 0;
    u32 zoneCount, nodeCount, n, i, j;
    (void)index;

    assetCheck("t16 zone count", o, 2, size);
    zoneCount = (u16)asw16(buf, o);
    o += 2;
    for (i = 0; i < zoneCount; i++) {
        assetCheck("t16 vertex count", o, 2, size);
        n = (u16)asw16(buf, o);
        o += 2;
        assetSwap16Range(buf, o, o + n * 6, size); /* ZoneVertex { s16 x, y, z } */
        o += n * 6;

        assetCheck("t16 entry count", o, 2, size);
        n = (u16)asw16(buf, o);
        o += 2;
        assetCheck("t16 zone entries", o, n * ZONE_ENTRY_SIZE, size);
        for (j = 0; j < n; j++, o += ZONE_ENTRY_SIZE) {
            assetSwap16Range(buf, o, o + 6, size); /* s16 x, y, z; flags/radius/height/pad are bytes */
        }

        assetSwap16Range(buf, o, o + 4, size); /* s16 id, s16 unk19 */
        o += 4;
    }

    assetCheck("t16 node count", o, 2, size);
    nodeCount = (u16)asw16(buf, o);
    o += 2;
    for (i = 0; i < nodeCount; i++) {
        assetCheck("t16 spawn node", o, SPAWN_NODE_RECORD_SIZE, size);
        assetSwap16Range(buf, o + 0x02, o + 0x0A, size); /* yaw, x, y, z */
        /* 0x0A..0x0B: unkA/unkB vs unkAWide conflict, kept in ROM byte order (header comment) */
        n = buf[o + 0x11]; /* pathCount */
        o += SPAWN_NODE_RECORD_SIZE;
        assetCheck("t16 path steps", o, n * 2, size); /* u8[2] steps, bytes */
        o += n * 2;
    }
}
