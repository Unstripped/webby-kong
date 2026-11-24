/* Table 19: miscellaneous global blobs; the format depends on the index.
 *   0  s16 depthTable[512] (1024 bytes): zbufferDepthToDistance (global/103AB0.c) via gZbufferDistanceTable, loaded by
 *      mapLoadLevel (global/450.c) when gGameMode == 0                                  -> u16 words
 *   1  transition mesh blob (16452 bytes): transitionInit -> meshBlobParse, meshComputeVertexRanges,
 *      meshDrawTextured, meshApplyAnimation, transitionRender (global/10E1D0.c). No Gfx or Vtx inside:
 *        0x00 MeshBlob header, 11 words: s32 vertexCount, s32 cellCount, 7 pointer slots (overwritten),
 *             s32 tileWidth, s32 tileHeight
 *        0x2C MeshVertex[vertexCount]  f32 x, y, z
 *             MeshCell[cellCount]      s32 v0, v1, v2, st0, st1, st2, texture
 *             s32 columnCounts[c] (0-terminated, c includes the terminator), s32 columnStarts[c],
 *             s32 columnEnds[c]
 *             AnimTable header: 3 pointer slots (overwritten)
 *             AnimEntry[e] (kind 0 terminates, e includes it): s32 kind, group, start, end; f32 scale;
 *                                                             u32 source, destination
 *             s32 counts[k] (-1 terminates, k includes it)
 *             k pointer slots (anim->indices, overwritten)
 *             u16 indices[sum of counts]                                        -> words, then u16s
 *   2  CRC-32 lookup table (u32[256]) and
 *   3  240 bytes of mixed words: no code loads either index (assetLoad(0x13, ...) is only called with
 *      0, 1, 4, 5, 6, 7), so their format is unverified -> abort if they are ever loaded.
 *   4  u8 text[][32]: critterInitText (critter/4840.c), string bytes                    -> no swap
 *   5  u16 wordTimings[]: critter/4840.c frame thresholds per word                     -> u16 words
 *   6  f32 boneFrames[7 * 0x34]: actorSetNodeValuesForFrame / actorGroupBoneFrameUpdate (global/A2F10.c) -> u32 words
 *   7  newline-separated ASCII, '*' terminated: bannerTextUpdate and its draw (global/CBEA0.c) -> no swap */
#include "endian/asset_swap.h"

static u32 swapUntil32(u8 *buf, u32 o, u32 size, u32 terminator, u32 stride, const char *what) {
    u32 n = 0;
    for (;;) {
        u32 v;
        assetCheck(what, o, stride, size);
        v = asw32(buf, o);
        assetSwap32Range(buf, o + 4, o + stride, size);
        o += stride;
        n++;
        if (v == terminator) {
            return n;
        }
    }
}

static void swapTransitionMesh(u8 *buf, u32 size) {
    u32 o, vertices, cells, c, e, k, i, sum;

    assetCheck("t19 mesh header", 0, 0x2C, size);
    assetSwap32Range(buf, 0, 0x2C, size);
    memcpy(&vertices, buf + 0, 4);
    memcpy(&cells, buf + 4, 4);
    o = 0x2C;
    assetSwap32Range(buf, o, o + vertices * 12, size);
    o += vertices * 12;
    assetSwap32Range(buf, o, o + cells * 28, size);
    o += cells * 28;

    c = swapUntil32(buf, o, size, 0, 4, "t19 columnCounts");
    o += c * 4;
    assetSwap32Range(buf, o, o + c * 8, size); /* columnStarts, columnEnds */
    o += c * 8;

    assetSwap32Range(buf, o, o + 12, size); /* AnimTable header slots */
    o += 12;
    e = swapUntil32(buf, o, size, 0, 28, "t19 AnimEntry");
    o += e * 28;

    sum = 0;
    k = 0;
    for (;;) {
        u32 v;
        assetCheck("t19 anim counts", o + k * 4, 4, size);
        v = asw32(buf, o + k * 4);
        k++;
        if (v == 0xFFFFFFFFu) {
            break;
        }
        sum += v;
    }
    o += k * 4;
    for (i = 0; i < k; i++) { /* anim->indices pointer slots */
        assetCheck("t19 anim index slots", o, 4, size);
        asw32(buf, o);
        o += 4;
    }
    assetSwap16Range(buf, o, o + sum * 2, size);
}

void portSwapAssetTable19(s32 index, u8 *buf, u32 size) {
    switch (index) {
    case 0:
    case 5:
        assetSwap16Range(buf, 0, size & ~1u, size);
        break;
    case 1:
        swapTransitionMesh(buf, size);
        break;
    case 4:
    case 7:
        break; /* text bytes */
    case 6:
        assetSwap32Range(buf, 0, size & ~3u, size);
        break;
    default:
        portAbort("PORT: asset table 19 index %d has no known format", (int)index);
    }
}
