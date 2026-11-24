/* Asset table 4: "model 2" objects (ModelAssetHeader, include/model.h; ModelSlot.unk24), loaded by
 * modelSlotInitFromAsset, modelTypeGetDefaultScale and modelTypeNeedsPrivateAsset (36880.c). Format notes: decomp/tools/assets.py
 * model2_parts. All header offsets are entry offsets; the game adds the entry address when it reads them.
 *
 * Every section boundary below was checked against all 581 non-empty entries of the US ROM (kind 1: 511, kind 2:
 * 70): the sections [0x4C] .. [0x70] are contiguous in this order, the collision list ends within 3 bytes (word
 * alignment) of [0x50], kind-1 entries end at [0x70] (no quad list), kind-2 quad lists end at the entry end.
 *
 * Header (0x78 bytes):
 *   0x00 u32 flags        native  modelTypeNeedsPrivateAsset (&8), modelSlotInit (&= ~0x10), modelSlotBuildTriangleBuffer (&4),
 *                                 133A90.c (&1), playerSlotMatchesSizeMask callers (36880.c, 6AD90.c, 70FD0.c)
 *   0x04 f32 scale        native  modelTypeGetDefaultScale (`asset[1]`)
 *   0x08 s32              native  modelSlotResolveAssetTextures (= gFrameCounter), 36880.c:2242 (compare)
 *   0x0C..0x1B name + dev garbage: no reader                           untouched
 *   0x1C u8 modelKind (1 = DL model, 2 = textured quads), 0x1D u8 fullBright                       bytes
 *   0x1E s16 draw distance native modelSlotInit, modelApplyMapOverrideValue, 36880.c:1697
 *   0x20 s32 collision radius native collisionBuildSlotBoxes (70FD0.c)
 *   0x24..0x27 no reader                                                untouched
 *   0x28/0x2A/0x2C/0x2E/0x30 s16 (sprite texture id, anim texture id, yaw, half width, half depth)  native
 *        modelSlotInit
 *   0x32..0x39 u8 (sprite tex size/format/alpha/z), 0x3A u8 (36880.c:3709)                      bytes
 *   0x3C s16 texture refcount native modelSlotResolveAssetTextures (++), modelSegmentFreeUnusedTextures and
 *        36880.c:1961/2244 (--)
 *   0x40..0x70 s32 x13 section offsets  native
 * Kind 1 only: [0x40, 0x44) DL A, [0x44, 0x48) DL B: Gfx (modelSlotInitFromAsset -> displayListA/B; walked by
 *   renderReplaceSegment6Commands, dlFindTextureCommands, dlResolveNodeTextures, dlFreeSegmentCommandTextures,
 *   dlLengthToEndDL). [0x48, 0x4C) vertices: Vtx (segment 8, segmentData). In kind-2 entries 0x40/0x48/0x4C
 *   are 0x78 and 0x44 points into the collision list; those fields are never used as DLs.
 * [0x4C] collision (collisionBuildSlotBoxes / collisionTransformTriangle, 70FD0.c): s32 count, then count x 0x16
 *   (CollisionAssetTri: s16 x9, u8 flags, u8 unk13, s8 bone, pad).
 * [0x50] triangles (modelSlotBuildTriangleBuffer / boundsTransformToAabb / modelTriangleTransform, 36880.c):
 *   s32 count, s16 bounds[6], then count x 0x18 (ModelAssetTri: s16 v[9], u8, s8 bone, u8, u8, pad[2]).
 * [0x54] vertex positions (modelSlotBuildVertPosList), [0x58] node positions (modelSlotBuildNodePosList,
 *   modelSlotGetNodeMatrix): s32 count, count x 8 (ModelVert: s16 x/y/z, s8 bone, pad).
 * [0x5C] proximity paths (modelSlotBuildProxPaths): s32 count, count x 0xB8 (ModelPathEntry: f32 x5, u8 x4, then
 *   8 x ModelPathSeg 0x14: f32 x2, s16 x4, u8 x4).
 * [0x60, 0x64) bone chains (modelBuildBoneChainMatrix, 70FD0.c; indexed bone << 5): s32 offsets into the frame
 *   buffer, -1 terminated, 0x20 per bone. Whole range s32.
 * [0x64, 0x68) animation nodes (modelSlotAllocAnimBuffers, modelAnimEntryBuildNodeMatrices / QueueMatrixCopies /
 *   BuildKeyMatrix / Advance, 54150.c): u32 node offsets[10] relative to [0x64], then nodes laid out back to back
 *   from +0x28 to the section end: ModelAnimNodeHeader (0x3C bytes: u8 keyCount, u8 times[0x38], u8 matrixCount,
 *   pad[2]), then matrixCount records of 8 + keyCount * 0x24 bytes (u32 Mtx offset, u32 f32-matrix offset,
 *   ModelBKey f32 x9 per key) -> all words. Offsets of unused slots point past the last real node (into later
 *   sections or into a real node's records), so the walk follows the contiguous layout, not the offset table.
 * [0x68, 0x6C) initial frame buffer (modelSlotAllocAnimBuffers, modelSlotDataSize, modelSlotCopyData): u32 buffer
 *   size, u32 data size, then the data: f32 4x4 matrices and Mtx (memcpy'd into ModelData.frames, read by
 *   modelAnimEntryBuildKeyMatrix as f32 and by modelBuildBoneChainMatrix / guMtxCatL as Mtx) -> all words.
 * [0x6C] texture animations (modelSlotInitTexAnimNodes, modelSlotLoadAssetTextures): s32 count, count x 0x84
 *   (s32 x 0x21).
 * [0x70] kind 2 only: quads (modelQuadBuildOutput, modelSlotLinkTexAnimTextures, modelSlotLoadAssetTextures):
 *   s32 count, count x 0x30 (s16 texture, s16 palette, s16 x[4], y[4], z[4], uv[4][2], u8 x4). */
#include "endian/asset_swap.h"

#define MODEL_HEADER_SIZE 0x78
#define MODEL_ANIM_NODE_HEADER_SIZE 0x3C

/* s32 count at `off`, then count records of `stride` bytes; per record u32 words [w0, w1), u16 halves [h0, h1) */
static void swapCounted(u8 *buf, u32 off, u32 first, u32 stride, u32 w0, u32 w1, u32 h0, u32 h1, u32 size,
                        const char *what) {
    u32 n, i, r;
    assetCheck(what, off, 4, size);
    n = asw32(buf, off);
    if (n > size / stride) {
        portAbort("PORT: model %s count %u too large (size 0x%x)", what, n, size);
    }
    assetCheck(what, off + first, n * stride, size);
    for (i = 0, r = off + first; i < n; i++, r += stride) {
        if (w1 > w0) {
            assetSwap32Range(buf, r + w0, r + w1, size);
        }
        if (h1 > h0) {
            assetSwap16Range(buf, r + h0, r + h1, size);
        }
    }
}

static void swapAnimNodes(u8 *buf, u32 start, u32 end, u32 size) {
    u32 p, keyCount, matrixCount, stride;

    assetSwap32Range(buf, start, start + 10 * 4, size);
    p = start + 10 * 4;
    while (p < end) {
        assetCheck("model anim node", p, MODEL_ANIM_NODE_HEADER_SIZE, size);
        keyCount = buf[p];
        matrixCount = buf[p + 0x39];
        stride = 8 + keyCount * 0x24;
        p += MODEL_ANIM_NODE_HEADER_SIZE;
        assetSwap32Range(buf, p, p + matrixCount * stride, size);
        p += matrixCount * stride;
    }
    if (p != end) {
        portAbort("PORT: model anim nodes end at 0x%x, section ends at 0x%x", p, end);
    }
}

/* s32 count, then ModelPathEntry[count] (0xB8): f32 x5, u8 x4, then 8 x ModelPathSeg (0x14 from +0x18):
 * f32 x2, s16 x4, u8 x4. All 8 segments are swapped (segCount only bounds the ones the game reads). */
static void swapPaths(u8 *buf, u32 off, u32 size) {
    u32 n, i, s, r, seg;
    assetCheck("model paths", off, 4, size);
    n = asw32(buf, off);
    if (n > size / 0xB8) {
        portAbort("PORT: model paths count %u too large (size 0x%x)", n, size);
    }
    assetCheck("model paths", off + 4, n * 0xB8, size);
    for (i = 0, r = off + 4; i < n; i++, r += 0xB8) {
        assetSwap32Range(buf, r, r + 0x14, size);
        for (s = 0, seg = r + 0x18; s < 8; s++, seg += 0x14) {
            assetSwap32Range(buf, seg, seg + 0x08, size);
            assetSwap16Range(buf, seg + 0x08, seg + 0x10, size);
        }
    }
}

void portSwapAssetTable04(s32 index, u8 *buf, u32 size) {
    u32 o[13];
    u32 i;

    assetCheck("model header", 0, MODEL_HEADER_SIZE, size);
    asw32(buf, 0x00);
    asw32(buf, 0x04);
    asw32(buf, 0x08);
    asw16(buf, 0x1E);
    asw32(buf, 0x20);
    assetSwap16Range(buf, 0x28, 0x32, size);
    asw16(buf, 0x3C);
    for (i = 0; i < 13; i++) {
        o[i] = asw32(buf, 0x40 + i * 4);
    }
#define OFF(field) o[((field) - 0x40) / 4]

    if (buf[0x1C] == 1) {
        assetSwapGfxRange(buf, OFF(0x40), OFF(0x44), size, 4, index);
        assetSwapGfxRange(buf, OFF(0x44), OFF(0x48), size, 4, index);
        assetSwapVtxRange(buf, OFF(0x48), OFF(0x4C), size);
    }

    swapCounted(buf, OFF(0x4C), 4, 0x16, 0, 0, 0x00, 0x12, size, "collision");
    /* triangles: count, s16 bounds[6], records from +0x10 */
    assetCheck("model triangles", OFF(0x50), 0x10, size);
    assetSwap16Range(buf, OFF(0x50) + 4, OFF(0x50) + 0x10, size);
    swapCounted(buf, OFF(0x50), 0x10, 0x18, 0, 0, 0x00, 0x12, size, "triangles");
    swapCounted(buf, OFF(0x54), 4, 8, 0, 0, 0x00, 0x06, size, "vertex positions");
    swapCounted(buf, OFF(0x58), 4, 8, 0, 0, 0x00, 0x06, size, "node positions");
    swapPaths(buf, OFF(0x5C), size);

    assetSwap32Range(buf, OFF(0x60), OFF(0x64), size);
    if (OFF(0x64) != OFF(0x68)) {
        swapAnimNodes(buf, OFF(0x64), OFF(0x68), size);
    }
    if (OFF(0x6C) > OFF(0x68)) {
        assetSwap32Range(buf, OFF(0x68), OFF(0x6C), size);
    }
    swapCounted(buf, OFF(0x6C), 4, 0x84, 0x00, 0x84, 0, 0, size, "texture animations");
    if (buf[0x1C] == 2) {
        swapCounted(buf, OFF(0x70), 4, 0x30, 0, 0, 0x00, 0x2C, size, "quads");
    }
#undef OFF
}
