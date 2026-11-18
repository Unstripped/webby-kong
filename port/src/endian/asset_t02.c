/* Asset table 2: per-map static collision mesh, indexed by map (collisionInitStaticCache, 70FD0.c).
 *
 * Layout (all offsets from the start of the inflated entry; checked against every map in the US ROM):
 *   0x00  u32 total triangle count        read natively: collisionInitStaticCache (D_807FB530 = *(s32 *)data)
 *   0x04  cell chain, one cell per grid square (header->unk10 * header->unk12 cells, the chain ends exactly
 *         at the end of the entry):
 *           +0  u32 endOff               offset (from entry start) of the next cell header == end of this
 *                                        cell's triangles; read natively by collisionInitStaticCache
 *           +4  CollisionStaticTriIn[n]  n = (endOff - (cell + 4)) / 0x18
 *   CollisionStaticTriIn (0x18 bytes): read by collisionBuildStaticTri (70FD0.c, via collisionLoadStaticCell)
 *   and by the handwritten-asm replacement in port/src/asm/77B90.c (StaticTriIn, through Cell.rawTris):
 *     0x00 s16 x0, y0, z0, x1, y1, z1, x2, y2, z2   native s16
 *     0x12 s16 normalYaw                              native s16
 *     0x14 u8 unk14, 0x15 u8 unk15, 0x16 u8 flags, 0x17 pad   bytes, untouched
 *
 * Map-level gzip (MapFileHeader.unk9 bit 0): the asset table itself does not compress table 2
 * (D_80748E18[2] == 0), but most maps store the entry as a raw gzip member that collisionInitStaticCache
 * inflates itself after assetLoad returns. The swapper then sees the compressed bytes (gzip magic 1F 8B; an
 * uncompressed entry always starts with a small BE triangle count, so its first byte is 0) and leaves them
 * alone. The game patch must then call `portAssetSwap(2, gCurrentMap, data, (u32)(gzOut - data))` right after
 * `gunzip(&gzIn, &gzOut, gzWork);` in collisionInitStaticCache, which re-enters here with the inflated bytes.
 *
 * Streaming (bit 0 clear): the entry is only used for its cell chain and freed; each cell's triangles are later
 * DMA'd straight from ROM by collisionLoadStaticCell (dmaReadAssetDecompress into Cell.rawTris), bypassing
 * assetLoad. That load must be followed by `portSwapCollisionTris(sStaticCollisionCells[cell].rawTris,
 * sStaticCollisionCells[cell].size)`. Table 3 (asset_t03.c) uses the same record swap. */
#include "endian/asset_swap.h"

/* also used by asset_t03.c; portSwapCollisionTris is called from the streamed-cell game patches */
void portSwapCollisionTris(void *tris, u32 size);
void portSwapCollisionCellChain(s32 table, u8 *buf, u32 size);

#define COLLISION_TRI_SIZE 0x18

/* 0x18-byte collision triangle records: ten s16 at 0x00..0x13, four bytes at 0x14..0x17 */
void portSwapCollisionTris(void *tris, u32 size) {
    u8 *p = tris;
    u32 o;
    if (size % COLLISION_TRI_SIZE) {
        portAbort("PORT: collision triangle block size 0x%x not a multiple of 0x18", size);
    }
    for (o = 0; o < size; o += COLLISION_TRI_SIZE) {
        assetSwap16Range(p, o, o + 0x14, size);
    }
}

/* cell chain shared with table 3: u32 count, then { u32 endOff; tris[] } until the end of the entry */
void portSwapCollisionCellChain(s32 table, u8 *buf, u32 size) {
    u32 cur;
    if (size >= 2 && buf[0] == 0x1F && buf[1] == 0x8B) {
        return; /* map-level gzip member: swapped after the game's own gunzip (see header comment) */
    }
    assetCheck(table == 2 ? "t2 tri count" : "t3 tri count", 0, 4, size);
    asw32(buf, 0);
    cur = 4;
    while (cur + 4 <= size) {
        u32 end = asw32(buf, cur);
        if (end < cur + 4 || end > size) {
            portAbort("PORT: asset table %d cell chain broken at 0x%x (end 0x%x size 0x%x)", (int)table, cur, end,
                      size);
        }
        portSwapCollisionTris(buf + cur + 4, end - (cur + 4));
        cur = end;
    }
}

void portSwapAssetTable02(s32 index, u8 *buf, u32 size) {
    (void)index;
    portSwapCollisionCellChain(2, buf, size);
}
