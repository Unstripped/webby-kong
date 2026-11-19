/* Asset table 5: actor (skinned) models, AnimInitData (include/actor.h), loaded by controllerAnimStateLoadAsset
 * (17B90.c, `assetLoad(5, assetId - 1, 1, 1)`), which immediately relocates the stored pointers in place:
 * a stored pointer P is the entry offset P - [0x00] + 0x28. Format notes: decomp/tools/assets.py actor_parts /
 * actor_skeleton.
 *
 * Every section boundary below was checked against all 236 entries of the US ROM: the sections are contiguous,
 * in this order (bone table absent when its pointer is 0), and the animated-texture list ends at the end of the
 * entry (plus 2 bytes of alignment padding in 196 entries).
 *
 * Header (0x28 bytes):
 *   0x00 u32 vertex base (stored address of entry offset 0x28)  native  controllerAnimStateLoadAsset (relocated;
 *        renderActorDisplayList binds it to segment 3)
 *   0x04 u32 DL pointer array, 0x08 u32 bone table (0 = default D_80746B64), 0x0C u32 collision data,
 *        0x10 u32 animated-texture list                          native  controllerAnimStateLoadAsset (relocated)
 *   0x14 u32 segmentNodes (0 in ROM; overwritten by controllerBuildSubcommandList)  native
 *   0x18 u32 (0 in ROM; no reader found)                          native (all-zero, swap is a no-op)
 *   0x1C s16 height                                               native  7AF60.c (ActorModelHeader.unk1C)
 *   0x1E s16 (values 0..0x243; no C reader found)                 native, see the report
 *   0x20 u8 boneCount (controllerAnimInit, 1E2D0.c, A2F10.c), 0x21 u8 displayListCount (controllerAnimStateLoadAsset,
 *        renderActorDisplayList), 0x22..0x27 zero                 bytes
 * [0x28, dl[0]) vertices: Vtx (segment 3, bone-local; drawn only through the DLs).
 * [dl[0], array) display lists: Gfx. Every DL pointer in the array points into this range, which
 *   controllerBuildSubcommandList / renderReplaceSegment6Commands / controllerCommandRelocateAssets /
 *   gfxSwapGeometryModeCullBits / controllerAnimFreeSub walk as one command run ending at the array; section
 *   markers {w0 = 0, w1 = segment} are read as words by controllerBuildSubcommandList.
 * [array] u32 DL pointers x u8[0x21]                              native  controllerAnimStateLoadAsset (relocated)
 * [bones] u8[0x20] records of 0x10 bytes                           actorBuildBoneMatrices (port/src/asm/1E2D0.c):
 *   0x00 u8 parent, 0x01 u8 matrix slot, 0x02 u8 channel, 0x03 pad  bytes (LBU)
 *   0x04/0x08/0x0C f32 offset x/y/z                                native (LW / F)
 * [collision] (ActorSubNodeWrapper.collision, actorSub140CreateFromModel 69E60.c -> actorSub140CreateFromData
 *   70FD0.c): f32 radius, f32 height, then four counted lists, every field 32-bit:
 *   s32 n, n x 0x14 (f32 x/y/z/scale, s32 mtxOffset); s32 n, n x 0x10 (f32 x/y/z, s32); s32 n, n x 0x2C
 *   (f32 x9, s32 id, s32 boneOffset); s32 n, n x 0x34 (f32 x9, s32 id, s32 boneOffset x3)    native words
 * [anim list] animListInit (8CA50.c): s16 count, then count x {s16 frames, s16 segment, s16 bind flag,
 *   s16 table-25 texture index[frames]}                            native s16 */
#include "endian/asset_swap.h"

#define ACTOR_HEADER_SIZE 0x28
#define ACTOR_BONE_SIZE 0x10

static u32 rd32native(const u8 *buf, u32 off) {
    u32 v;
    memcpy(&v, buf + off, 4);
    return v;
}

/* stored pointer -> entry offset (pointers are relocated against [0x00], which maps to offset 0x28) */
static u32 actorRel(u32 ptr, u32 base, u32 size, const char *what) {
    u32 off = ptr - base + ACTOR_HEADER_SIZE;
    assetCheck(what, off, 0, size);
    return off;
}

static u32 swapCollisionList(u8 *buf, u32 p, u32 stride, u32 size, const char *what) {
    u32 n;
    assetCheck(what, p, 4, size);
    n = asw32(buf, p);
    if (n > size / stride) {
        portAbort("PORT: actor model %s count %u too large (size 0x%x)", what, n, size);
    }
    assetSwap32Range(buf, p + 4, p + 4 + n * stride, size);
    return p + 4 + n * stride;
}

void portSwapAssetTable05(s32 index, u8 *buf, u32 size) {
    u32 base, array, bones, coll, anims, dl0, i, p, frames;
    u32 boneCount, dlCount;
    s16 count;

    assetCheck("actor model header", 0, ACTOR_HEADER_SIZE, size);
    base = asw32(buf, 0x00);
    array = actorRel(asw32(buf, 0x04), base, size, "actor DL array");
    bones = asw32(buf, 0x08);
    coll = actorRel(asw32(buf, 0x0C), base, size, "actor collision");
    anims = actorRel(asw32(buf, 0x10), base, size, "actor anim list");
    asw32(buf, 0x14);
    asw32(buf, 0x18);
    asw16(buf, 0x1C);
    asw16(buf, 0x1E);
    boneCount = buf[0x20];
    dlCount = buf[0x21];

    /* DL pointer array; the DL run starts at the first entry and ends at the array */
    if (dlCount == 0) {
        portAbort("PORT: actor model %d has no display list", (int)index);
    }
    assetSwap32Range(buf, array, array + dlCount * 4, size);
    dl0 = actorRel(rd32native(buf, array), base, size, "actor DL");
    if (dl0 < ACTOR_HEADER_SIZE || dl0 > array) {
        portAbort("PORT: actor model %d DL 0x%x outside 0x28..0x%x", (int)index, dl0, array);
    }
    for (i = 1; i < dlCount; i++) {
        u32 d = actorRel(rd32native(buf, array + i * 4), base, size, "actor DL");
        if (d < dl0 || d >= array) {
            portAbort("PORT: actor model %d DL %u at 0x%x outside 0x%x..0x%x", (int)index, i, d, dl0, array);
        }
    }
    assetSwapVtxRange(buf, ACTOR_HEADER_SIZE, dl0, size);
    assetSwapGfxRange(buf, dl0, array, size, 5, index);

    /* bone table: three f32 per record */
    if (bones != 0) {
        bones = actorRel(bones, base, size, "actor bones");
        assetCheck("actor bones", bones, boneCount * ACTOR_BONE_SIZE, size);
        for (i = 0; i < boneCount; i++) {
            assetSwap32Range(buf, bones + i * ACTOR_BONE_SIZE + 4, bones + (i + 1) * ACTOR_BONE_SIZE, size);
        }
    }

    /* collision: radius, height, four counted lists of 32-bit fields */
    assetSwap32Range(buf, coll, coll + 8, size);
    p = swapCollisionList(buf, coll + 8, 0x14, size, "collision nodes");
    p = swapCollisionList(buf, p, 0x10, size, "collision subnodes");
    p = swapCollisionList(buf, p, 0x2C, size, "collision boxes");
    swapCollisionList(buf, p, 0x34, size, "collision triangles");

    /* animated-texture list: all s16 */
    assetCheck("actor anim list", anims, 2, size);
    count = (s16)asw16(buf, anims);
    p = anims + 2;
    for (; count > 0; count--) {
        assetCheck("actor anim node", p, 6, size);
        frames = (u16)asw16(buf, p);
        asw16(buf, p + 2);
        asw16(buf, p + 4);
        p += 6;
        if ((s16)frames > 0) {
            assetSwap16Range(buf, p, p + frames * 2, size);
            p += frames * 2;
        }
    }
}
