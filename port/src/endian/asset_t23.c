/* Table 23: per-map player spawn points. Consumer: playerLoadSpawnerAsset (global/CC800.c), called from
 * mapLoadLevel (global/450.c); count = entry size / 10 (assetGetCachedSizeOrResolve). Records are read
 * through raceGridSpawnPoint by playerWarpToSpawnPoint (CC800.c), actorInitFromSpawnPoint (936B0.c) and
 * multiplayer/0.c; playerCheckSpawnerFlagFixup writes y of records 18/23 natively.
 *
 * Layout: G01Pos[size / 10], 10 bytes each:
 *   0x0 s16 x, y, z; 0x6 u8 yaw, camMode, spawnKind, flags
 * Every ROM entry is a multiple of 10 bytes. */
#include "endian/asset_swap.h"

void portSwapAssetTable23(s32 index, u8 *buf, u32 size) {
    u32 o;
    (void)index;

    for (o = 0; o + 10 <= size; o += 10) {
        assetSwap16Range(buf, o, o + 6, size);
    }
}
