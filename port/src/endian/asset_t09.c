/* Table 9: per-map model/actor setup ("setup" file), one entry per map.
 * Consumers: modelAssetGetSubtables, modelTypeCountsInit, modelTypeCountBeforeMap, modelSlotsLoadMap
 * (global/36880.c); actorMapModelsSetup, actorListInitOffsets, actorListBuildLookupTable,
 * actorListSpawnFromRecords, actorListSetCustomFloat (global/8D3E0.c); gridBuildNodeLists (global/F9450.c);
 * csModelStateSetVars (global/42630.c); 3C10.c via D_8076A160[].modelData (same layout).
 *
 * Layout:
 *   s32 modelCount
 *   ModelAssetEntry[modelCount], 0x30 bytes each (modelSlotsLoadMap, gridBuildNodeLists):
 *     0x00 f32 x, y, z, scale, unk10, unk14, rotX, rotY, rotZ, yOffset   -> 10 x u32
 *     0x28 s16 modelType, 0x2A s16 mapObjectId                            -> 2 x u16
 *     0x2C u8 pad[2] (never read), 0x2E u8 light, 0x2F u8 flags            -> bytes
 *   s32 csVarsCount
 *   csVars[csVarsCount], 0x24 bytes each (modelSlotsLoadMap -> csModelStateSetVars):
 *     0x00 s32 modelIndex, 0x04 f32 vars[8] (memcpy'd into CsStateFull.unk14, f32[8])  -> 9 x u32
 *   s32 actorCount
 *   ActorListRecord[actorCount], 0x38 bytes each (actorListSpawnFromRecords, actorListBuildLookupTable):
 *     0x00 f32 x, y, z, scale, customData[8] (spawnData: read as f32/s32 words only)  -> 12 x u32
 *     0x30 s16 yaw, 0x32 s16 actorType, 0x34 s16 id                                    -> 3 x u16
 *     0x36 pad[2] (never read)
 * Every ROM entry is exactly this length; only maps 4 and 26 have csVars records. */
#include "endian/asset_swap.h"

void portSwapAssetTable09(s32 index, u8 *buf, u32 size) {
    u32 o, n, i;
    (void)index;

    if (size == 0) {
        return;
    }
    assetCheck("t09 modelCount", 0, 4, size);
    n = asw32(buf, 0);
    o = 4;
    for (i = 0; i < n; i++, o += 0x30) {
        assetSwap32Range(buf, o, o + 0x28, size);
        assetSwap16Range(buf, o + 0x28, o + 0x2C, size);
    }

    assetCheck("t09 csVarsCount", o, 4, size);
    n = asw32(buf, o);
    o += 4;
    assetSwap32Range(buf, o, o + n * 0x24, size);
    o += n * 0x24;

    assetCheck("t09 actorCount", o, 4, size);
    n = asw32(buf, o);
    o += 4;
    for (i = 0; i < n; i++, o += 0x38) {
        assetSwap32Range(buf, o, o + 0x30, size);
        assetSwap16Range(buf, o + 0x30, o + 0x36, size);
    }
}
