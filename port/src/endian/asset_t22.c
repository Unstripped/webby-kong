/* Asset table 22: per-map critter (ambient creature) groups, indexed by map id. Loaded by mapLoadLevel (450.c)
 * only when gGlobalOverlayFlags & 0x1000 and handed to critterInit -> critterLoadData (critter/0.c). Entries exist
 * for maps 0x07, 0x1E, 0x21, 0x22, 0x2B, 0x2D, 0x2F, 0x32, 0x38, 0x48, 0xB0 in the US ROM (not on the intro path).
 *
 * Packed byte stream (records are unaligned; critterLoadData memcpys them into CritterGroup / CritterZone):
 *   u8 groupCount                                  memcpy(critterGroups, data, 1)
 *   groupCount x {
 *     u8 type, u8 zoneCount, u8 count              memcpy(grp, src, 3)
 *     zoneCount x CritterZone[0x00..0x20)          memcpy(zone, src, 0x20); read natively by critter/0.c:
 *       0x00 s16 x, y, z, unk6                     native s16
 *       0x08 CritterZoneShape: s16 radius, reactDistSq, height, unk6, extraHeight, unkA   native s16
 *            0x14 s32 ringRadius, 0x18 s32 nearDistSq                                     native s32
 *       0x1C u8 flags, pad[3]                      bytes
 *   }
 * The stream ends exactly at the entry's data end (inflated sizes are 0 or 1 byte larger). */
#include "endian/asset_swap.h"

#define CRITTER_ZONE_SIZE 0x20

void portSwapAssetTable22(s32 index, u8 *buf, u32 size) {
    u32 groups, g, zones, z, p;

    (void)index;
    if (buf == NULL || size == 0) {
        return;
    }
    groups = buf[0];
    p = 1;
    for (g = 0; g < groups; g++) {
        assetCheck("t22 group", p, 3, size);
        zones = buf[p + 1];
        p += 3;
        for (z = 0; z < zones; z++) {
            assetCheck("t22 zone", p, CRITTER_ZONE_SIZE, size);
            assetSwap16Range(buf, p, p + 0x14, size);
            assetSwap32Range(buf, p + 0x14, p + 0x1C, size);
            p += CRITTER_ZONE_SIZE;
        }
    }
}
