/* Asset table 1: map geometry, indexed by map id (mapLoadLevel, 450.c: `mapChunkAsset = assetLoad(1, gCurrentMap,
 * 1, 1)`). The buffer stays resident for the whole map; mapHeaderLoad (2F550.c) keeps it in gMapFileBase and the
 * per-subsystem init calls it makes turn the stored offsets into pointers in place.
 *
 * Layout (offsets from the start of the inflated entry; every section boundary below was checked against all 216
 * non-empty maps of the US ROM: the sections are contiguous, in this order, and the last one ends at the end of
 * the entry). Struct offsets are those of the decomp's C declarations.
 *
 * MapFileHeader (include/map.h), read by mapHeaderLoad, mapChunkAllocLists / mapChunksInitFromMapData (55B20.c),
 * collisionInitStaticCache (70FD0.c), collisionBuildCellTable (6AD90.c), effectWaterInitNodes (63EC0.c),
 * actorTargetTableLoad (67890.c), triggerZoneInit (684C0.c):
 *   0x00 s32 static collision inflated size, 0x04 u32 collision mesh inflated size        native
 *   0x08 u8 render flags, 0x09 u8 gzip flags                                               bytes
 *   0x0A..0x2D s16 x18 (far clip, fogDist, matrix cache size, grid dims, light/fog params) native
 *   0x2E pad
 *   0x30..0x73 s32 x17 section offsets                                                     native
 *     0x30 BSP tree, 0x34 dlBase, 0x38 DL end == vertex base (segment 6, gMapVtxBase), 0x3C unused (garbage in
 *     ROM, never read), 0x40 track points, 0x44 sound emitters, 0x48 texture anims, 0x4C water surfaces,
 *     0x50 actor target quads, 0x54 trigger zones, 0x58 cells, 0x5C cell boxes, 0x60 grid triangle vectors,
 *     0x64 chunk collision table, 0x68 chunk records, 0x6C portal slots (C name portalsOff -> gMapPortalSlots),
 *     0x70 portals (C name relocsOff -> gMapPortals)
 *   0x74 u32 unknown (small values 1..9); no consumer reads it -> left as is
 *
 * [0x30] BSP tree: MapBspNode (include/map.h), 0xC6 bytes used (sizeof 0xC8). Always one node at 0x78 ending at
 *   dlBase in the US ROM (kind 2 chunk root or kind 1 leaf); kind 0 split nodes are still walked, children being
 *   tree-relative offsets as mapTreeRelocate adds `base` = tree start. Readers: mapTreeRelocate,
 *   mapNodeTreeInitSurfaceEffects, mapTreeCloneShape, mapTreeRender, mapViewDrawPass, mapBspPlaneSideTest
 *   (2F550.c), mapChunksRender (55B20.c).
 *     0x00..0x5B u32/s32/f32 x23 (child0, child1, dl, unkC..unk18, unk1C[12] DL offsets, split plane f32 x4)
 *     0x5C..0xAB s16 x40 (unk5C..unk6E, unk70[12], unk88[12], bounds A0..AA)
 *     0xAC..0xC5 bytes (unkAC[12], kind, unkB9[12], unkC5 count)
 * [0x34, 0x38) display lists: Gfx only. displayListResolveTextureImages walks the whole range command by
 *   command; node/portal/chunk DL offsets all point inside it; chunk layer DLs (mapChunksInitFromMapData) contain
 *   cell markers {w0 = 0, w1 = cell id} read as words by mapChunkBuildCellTable / mapChunkEmitLayerDisplayLists,
 *   and G_SETTIMG w1 table-25 indices read as words by mapChunkLoadAssetRefs/mapChunkReleaseAssets. Every
 *   8-byte command in this range of every map has a defined F3DEX2/RDP opcode.
 * [0x38, 0x40) vertices: Vtx only (segment 6 base; chunk baseVtx = vtxBase + chunk[0x2C], copied and lit by
 *   lightApplyToVertices). Size is a multiple of 16 in every map.
 * [0x40, 0x44) track points (mapHeaderLoad, mapRelocateOffsetTable; bonus/2690.c, minecart/0.c): s32 N (-1 =
 *   none), N + 2 s32 offsets relative to the section (relocated into TargetVec3f pointers), then f32 x/y/z
 *   points. Every field is 32-bit -> whole section swapped as words.
 * [0x44] sound emitters (mapGetSoundEmitters): s32 count, LevelSoundEmitter[count] (8 bytes: s16 x3, u8 x2).
 * [0x48] texture anims (mapChunkEmitSegmentCommands / mapChunkAnimateTextures): s32 count, MapChunkAnim[count]
 *   (0x7C bytes: u8 x6 + pad, 0x08 texture pointer (0 in ROM), 0x0C s32 textureIds[] up to 0x7C).
 * [0x4C] water surfaces (effectWaterInitNodes and the water code in 63EC0.c): the count is read ONLY as the
 *   byte at +3 (`*(u8 *)(unk4C + 3)`), so the count word stays big-endian. WaterSurfaceDef[count] (include/water.h,
 *   0x6C bytes): 0x00..0x43 f32/s32 x17, 0x44..0x5F s16 x14, 0x60..0x6B bytes.
 * [0x50] actor target quads (actorTargetTableLoad etc., 67890.c): s32 count, ActorTargetQuad[count] (0x40 bytes:
 *   f32 texCoords[8], s16 x/y/z[4] + portalSlot/fadeStart/fadeEnd, u8 alphaNear/alphaFar).
 * [0x54] trigger zones (triggerZoneInit / triggerZoneCheckEntry, 684C0.c): s32 count, TriggerZoneData[count]
 *   (0x10 bytes: s16 x7, u8 x2).
 * [0x58] cells (gMapCells, 55B20.c): s32 count, MapCell[count] (0x1C bytes: s16 x12, u8 visible/seen, pad).
 * [0x5C] cell boxes (gMapCellBoxes, mapCellFindAtPoint etc.): s32 count (not read by C, but a count in every map),
 *   MapBox[count] (s16 x6).
 * [0x60] grid triangle vectors (gMapGridTriangleVerts, mapGridTriangleBuildPlane): MapGridVectors
 *   [portalSlotCount / 2] (f32 x12 each), no count word.
 * [0x64] chunk collision table: s32 chunk count (mapChunkAllocLists), MapChunkCollEntry[count + 1] (12 bytes:
 *   s32 offset from the section start, s16 minX/minZ/maxX/maxZ), then MapCollisionNode records from entry[0]
 *   .offset to entry[count].offset (0x14 bytes: s16 x9, u8 flags, pad; mapChunkFindNodeAtPoint).
 * [0x68] chunk records (mapChunksInitFromMapData): chunkCount x 0x34 bytes: 0x00..0x07 bytes (rgb, noSprites,
 *   kind, ...), 0x08..0x33 s32 x11 (lightMode, 4 x {layer DL offset, size}, vertex offset, vertex size).
 * [0x6C] portal slots (gMapPortalSlots): s32 count, MapATpSlot[count] (10 bytes: s16 x4, u8 seen/enabled).
 * [0x70] portals (gMapPortals): s32 count, MapPortal[count] (0x10 bytes: s16 x3, pad, s32 dl0/dl1). */
#include "endian/asset_swap.h"

#define MAP_HEADER_SIZE 0x78
#define MAP_BSP_NODE_SIZE 0xC6
#define MAP_BSP_MAX_DEPTH 64

static u32 rd32(const u8 *buf, u32 off) {
    u32 v;
    memcpy(&v, buf + off, 4);
    return v;
}

/* `n` records of `stride` bytes at `start`: u32 words [w0, w1) and u16 halves [h0, h1) of each record */
static void swapRecords(u8 *buf, u32 start, u32 n, u32 stride, u32 w0, u32 w1, u32 h0, u32 h1, u32 size,
                        const char *what) {
    u32 r, i;
    if (n > size / stride) {
        portAbort("PORT: map %s count %u too large (size 0x%x)", what, n, size);
    }
    assetCheck(what, start, n * stride, size);
    for (i = 0, r = start; i < n; i++, r += stride) {
        if (w1 > w0) {
            assetSwap32Range(buf, r + w0, r + w1, size);
        }
        if (h1 > h0) {
            assetSwap16Range(buf, r + h0, r + h1, size);
        }
    }
}

/* s32 count at `off` (swapped) followed by records */
static u32 swapCounted(u8 *buf, u32 off, u32 stride, u32 w0, u32 w1, u32 h0, u32 h1, u32 size, const char *what) {
    u32 n;
    assetCheck(what, off, 4, size);
    n = asw32(buf, off);
    swapRecords(buf, off + 4, n, stride, w0, w1, h0, h1, size, what);
    return n;
}

static void swapBspNode(u8 *buf, u32 tree, u32 off, u32 size, int depth) {
    if (depth > MAP_BSP_MAX_DEPTH) {
        portAbort("PORT: map BSP tree deeper than %d at 0x%x", MAP_BSP_MAX_DEPTH, off);
    }
    assetCheck("map BSP node", off, MAP_BSP_NODE_SIZE, size);
    assetSwap32Range(buf, off + 0x00, off + 0x5C, size);
    assetSwap16Range(buf, off + 0x5C, off + 0xAC, size);
    if (buf[off + 0xB8] == 0) { /* split node: tree-relative children (mapTreeRelocate) */
        swapBspNode(buf, tree, tree + rd32(buf, off + 0x00), size, depth + 1);
        swapBspNode(buf, tree, tree + rd32(buf, off + 0x04), size, depth + 1);
    }
}

void portSwapAssetTable01(s32 index, u8 *buf, u32 size) {
    u32 tree, dlBase, vtxBase, track, emitters, anims, water, quads, zones, cells, boxes, vecs, coll, chunks, slots,
        portals;
    u32 n, slotCount, collStart, collEnd;

    if (buf == NULL || size == 0) {
        return;
    }
    assetCheck("map header", 0, MAP_HEADER_SIZE, size);
    asw32(buf, 0x00);
    asw32(buf, 0x04);
    assetSwap16Range(buf, 0x0A, 0x2E, size);
    assetSwap32Range(buf, 0x30, 0x74, size);
    tree = rd32(buf, 0x30);
    dlBase = rd32(buf, 0x34);
    vtxBase = rd32(buf, 0x38);
    track = rd32(buf, 0x40);
    emitters = rd32(buf, 0x44);
    anims = rd32(buf, 0x48);
    water = rd32(buf, 0x4C);
    quads = rd32(buf, 0x50);
    zones = rd32(buf, 0x54);
    cells = rd32(buf, 0x58);
    boxes = rd32(buf, 0x5C);
    vecs = rd32(buf, 0x60);
    coll = rd32(buf, 0x64);
    chunks = rd32(buf, 0x68);
    slots = rd32(buf, 0x6C);
    portals = rd32(buf, 0x70);

    swapBspNode(buf, tree, tree, size, 0);

    assetSwapGfxRange(buf, dlBase, vtxBase, size, 1, index);
    if (track < vtxBase || (track - vtxBase) % 16 != 0) {
        portAbort("PORT: map %d vertex block 0x%x-0x%x not whole Vtx", (int)index, vtxBase, track);
    }
    assetSwapVtxRange(buf, vtxBase, track, size);
    assetSwap32Range(buf, track, emitters, size);

    swapCounted(buf, emitters, 0x08, 0, 0, 0x00, 0x06, size, "sound emitters");
    swapCounted(buf, anims, 0x7C, 0x08, 0x7C, 0, 0, size, "texture anims");
    assetCheck("water count", water, 4, size);
    swapRecords(buf, water + 4, buf[water + 3], 0x6C, 0x00, 0x44, 0x44, 0x60, size, "water surfaces");
    swapCounted(buf, quads, 0x40, 0x00, 0x20, 0x20, 0x3E, size, "target quads");
    swapCounted(buf, zones, 0x10, 0, 0, 0x00, 0x0E, size, "trigger zones");
    swapCounted(buf, cells, 0x1C, 0, 0, 0x00, 0x18, size, "cells");
    swapCounted(buf, boxes, 0x0C, 0, 0, 0x00, 0x0C, size, "cell boxes");
    slotCount = swapCounted(buf, slots, 0x0A, 0, 0, 0x00, 0x08, size, "portal slots");
    swapRecords(buf, vecs, (u32)((s32)slotCount / 2), 0x30, 0x00, 0x30, 0, 0, size, "grid vectors");

    /* chunk collision table: count, count + 1 entries, then nodes */
    assetCheck("chunk count", coll, 4, size);
    n = asw32(buf, coll);
    if (n >= size / 12) {
        portAbort("PORT: map %d chunk count %u too large", (int)index, n);
    }
    swapRecords(buf, coll + 4, n + 1, 0x0C, 0x00, 0x04, 0x04, 0x0C, size, "chunk collision entries");
    collStart = coll + rd32(buf, coll + 4);
    collEnd = coll + rd32(buf, coll + 4 + n * 12);
    if (collEnd < collStart || (collEnd - collStart) % 0x14 != 0) {
        portAbort("PORT: map %d collision nodes 0x%x-0x%x not whole records", (int)index, collStart, collEnd);
    }
    swapRecords(buf, collStart, (collEnd - collStart) / 0x14, 0x14, 0, 0, 0x00, 0x12, size, "collision nodes");
    swapRecords(buf, chunks, n, 0x34, 0x08, 0x34, 0, 0, size, "chunk records");

    swapCounted(buf, portals, 0x10, 0x08, 0x10, 0x00, 0x06, size, "portals");
}
