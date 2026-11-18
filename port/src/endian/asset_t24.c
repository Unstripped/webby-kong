/* Table 24: race routes (race overlay). Consumer: raceInitRoute -> raceLoadRouteData (race/A960.c);
 * routes and waypoints are then read via raceGetRoute / raceGetWaypoint and raceInitGateActors.
 * Only entries 0x0E, 0x27, 0x52 and 0xB9 are non-empty.
 *
 * Layout (byte-packed, fields are unaligned; every block is memcpy'd out):
 *   0x0 u8 routeCount                   RaceRouteTable.count
 *   0x1 u16 waypointCount               RaceRouteTable.count2
 *   0x3 per route: u16 count, u16 items[count] (waypoint indices)          RaceRoute
 *       RaceWaypoint[waypointCount], 0x1C bytes each (memcpy 0x1C):
 *         0x00 s16 x, y, z; 0x06 u16 angle; 0x08 f32 unk8, unkC; 0x10 u8 kind, pad[3];
 *         0x14 f32 unk14; 0x18 u8 mode, pad; 0x1A u16 width
 * Every non-empty ROM entry parses to exactly its length. */
#include "endian/asset_swap.h"

void portSwapAssetTable24(s32 index, u8 *buf, u32 size) {
    u32 o, routes, waypoints, i, n;
    (void)index;

    if (size == 0) {
        return;
    }
    assetCheck("t24 header", 0, 3, size);
    routes = buf[0];
    waypoints = (u16)asw16(buf, 1);
    o = 3;
    for (i = 0; i < routes; i++) {
        assetCheck("t24 route", o, 2, size);
        n = (u16)asw16(buf, o);
        o += 2;
        assetSwap16Range(buf, o, o + n * 2, size);
        o += n * 2;
    }
    for (i = 0; i < waypoints; i++, o += 0x1C) {
        assetSwap16Range(buf, o, o + 8, size);
        assetSwap32Range(buf, o + 8, o + 0x10, size);
        assetSwap32Range(buf, o + 0x14, o + 0x18, size);
        assetSwap16Range(buf, o + 0x1A, o + 0x1C, size);
    }
}
