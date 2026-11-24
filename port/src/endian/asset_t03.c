/* Asset table 3: per-map floor/wall collision mesh, indexed by map (collisionBuildCellTable, 6AD90.c).
 *
 * Layout (offsets from the start of the inflated entry; checked against every map in the US ROM). It is the
 * table 2 container (asset_t02.c) with a different triangle record:
 *   0x00  u32 total triangle count        not read by C; swapped as u32 for consistency with table 2
 *   0x04  cell chain, header->unk18 * header->unk1A cells, ending exactly at the end of the entry:
 *           +0  u32 endOff               offset of the next cell header == end of this cell's triangles;
 *                                        read natively by collisionBuildCellTable (`*(u32 *)cursor`)
 *           +4  CollisionStaticTri[n]
 *   CollisionStaticTri (0x18 bytes, 6AD90.c), read through CollisionBufferEntry.tris/trisEnd and
 *   Actor.floorTriangle by collisionFindFloorAtPoint's mesh walk (6AD90.c ~880-1013), collisionTriangleHeightAtXZ,
 *   collisionRecordTriangleS, collisionCheckTriangleWindingS16, collisionStoreTriangleNormalAnglesS16,
 *   collisionCheckTriangleXZBoundsS16, collisionPointInTriangleXZS16:
 *     0x00 s16 x[3], 0x06 s16 y[3], 0x0C s16 z[3]      native s16
 *     0x12 s16 flags (bits 0x1..0x4850 tested as s16)  native s16
 *     0x14 u8 slopeType, 0x15 u8 surface, 0x16 u8 tintPath, 0x17 s8 offsetSlot   bytes, untouched
 *
 * Map-level gzip (MapFileHeader.unk9 bit 1): table 3 is not compressed by the asset table (sAssetTableCompressed[3] == 0),
 * but most maps (including the intro maps 0x28 and 0x50) store it as a raw gzip member that
 * collisionBuildCellTable inflates after assetLoad returns. The swapper sees the gzip bytes and leaves them
 * alone; the game patch must call `portAssetSwap(3, gCurrentMap, data, (u32)(dst - data))` right after
 * `gunzip(&src, &dst, window);` in collisionBuildCellTable (before the cell loop reads `*(u32 *)cursor`).
 *
 * Streaming (bit 1 clear): the entry is used only for the cell chain and freed; dynamicBufferAllocateEntry
 * (6AD90.c) later DMAs each cell's triangles straight from ROM into CollisionBufferEntry.tris, bypassing
 * assetLoad. That load must be followed by `portSwapCollisionTris(sCollisionCells[index].tris,
 * sCollisionCells[index].size)`. */
#include "endian/asset_swap.h"

void portSwapCollisionCellChain(s32 table, u8 *buf, u32 size); /* asset_t02.c */

void portSwapAssetTable03(s32 index, u8 *buf, u32 size) {
    (void)index;
    portSwapCollisionCellChain(3, buf, size);
}
