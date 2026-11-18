/* Table 8: camera path data, one entry per map plus index 0 (the global camera paths).
 * Consumers: camInit -> camLoadPathTable -> camLoadPathData (global/201B0.c); index 0 is parsed with
 * allocate = 0 (only the counts and path lists are read), map entries with allocate = 1.
 *
 * Layout (all offsets relative to the running cursor, no alignment padding anywhere):
 *   0x00  s16 segCount[24]                         camLoadPathData: CamPathHead.count
 *   0x30  CamPathSeg segs[sum(segCount)]           0x12 bytes each: s16 a[3], b[3], c[3] (camCopyVec3s)
 *         s16 zoneCount
 *         CamZoneEntryFull zones[zoneCount]         0x1C bytes each, struct-copied into paths->zones:
 *           0x00 union (0x10 bytes), read per zone type (byte 0x16) by camUpdateZones and its helpers:
 *                type 0 CamZoneWeightLinks  u8 zones[6], u8 weights[6]   (func_8061F4B0)  -> bytes
 *                type 6 CamZoneFollowOverride u8 x3                                         -> bytes
 *                type 1 fixedEye, 2/8/9 rail, 3 tether, 5 linkZone, 7 circleRadius: all s16 -> 8 x s16
 *                (type 4 and unknown types read no union field; swapped as s16, harmless)
 *           0x10 s16 centerX, centerY, centerZ  (camInitZones, mapChunkFindByPosition(zp[8..10]))
 *           0x16 u8 type, flags, weight, radius, height, distMode
 *         s16 pathCount
 *         per path: s16 stepCount, then stepCount x { s16 node, s16 frame }   (CamPathList)
 *         s16 nodeCount
 *         node records until nodeCount typed records were read (unknown types are 4-byte headers that do
 *         not count):
 *           0x0 u8 unk0, 0x1 u8 type, 0x2 2 bytes (never read), then a payload copied into CamPathPoint.u:
 *             13  0xC: s32 args[3]                (camPathCommandDispatch(args[0..2]), args[1] >> 16)
 *             12  0x2: u16 sfx id                 (sfxAllocateChannel(*(u16 *)args))
 *             10/15/16  0xE: CamPathPointOrbit s16 x6, u8 fovEnd, u8 fovStart
 *             1   0x6: s16 x, y, z               (CamPathPointPos; maxDist not present)
 *             2   0x8: s16 x, y, z, maxDist
 *             3   0xC: CamPathPointSegment s16 x6
 *             4   s16 keyCount at +4, keys at +0x20: keyCount x CamSplineKeyPosLook (s16 x6, u8 fov, s8 pitch)
 *             5   s16 keyCount at +4, keys at +0x14: keyCount x CamSplineKeyPos (s16 x3, u8 fov, s8 pitch)
 *             other: no payload
 *         For types 4/5 the bytes between +6 and the keys are never read and stay as they are.
 * Every non-empty ROM entry parses to exactly its inflated length with this walk. */
#include "endian/asset_swap.h"

void portSwapAssetTable08(s32 index, u8 *buf, u32 size) {
    u32 o, segs, n, i, j;
    (void)index;

    if (size == 0) {
        return;
    }
    assetCheck("t08 header", 0, 0x30, size);
    segs = 0;
    for (i = 0; i < 24; i++) {
        segs += (u16)asw16(buf, i * 2);
    }
    o = 0x30;
    assetSwap16Range(buf, o, o + segs * 0x12, size);
    o += segs * 0x12;

    assetCheck("t08 zoneCount", o, 2, size);
    n = (u16)asw16(buf, o);
    o += 2;
    for (i = 0; i < n; i++, o += 0x1C) {
        u8 type;
        assetCheck("t08 zone", o, 0x1C, size);
        type = buf[o + 0x16];
        if (type != 0 && type != 6) {
            assetSwap16Range(buf, o, o + 0x10, size);
        }
        assetSwap16Range(buf, o + 0x10, o + 0x16, size);
    }

    assetCheck("t08 pathCount", o, 2, size);
    n = (u16)asw16(buf, o);
    o += 2;
    for (i = 0; i < n; i++) {
        u32 steps;
        assetCheck("t08 path", o, 2, size);
        steps = (u16)asw16(buf, o);
        o += 2;
        assetSwap16Range(buf, o, o + steps * 4, size);
        o += steps * 4;
    }

    assetCheck("t08 nodeCount", o, 2, size);
    n = (u16)asw16(buf, o);
    o += 2;
    while (n != 0) {
        u8 type;
        u32 keys;
        assetCheck("t08 node", o, 4, size);
        type = buf[o + 1];
        switch (type) {
        case 13:
            assetSwap32Range(buf, o + 4, o + 0x10, size);
            o += 0x10;
            break;
        case 12:
            assetSwap16Range(buf, o + 4, o + 6, size);
            o += 6;
            break;
        case 10:
        case 15:
        case 16:
            assetSwap16Range(buf, o + 4, o + 0x10, size);
            o += 0x12;
            break;
        case 1:
            assetSwap16Range(buf, o + 4, o + 0xA, size);
            o += 0xA;
            break;
        case 2:
            assetSwap16Range(buf, o + 4, o + 0xC, size);
            o += 0xC;
            break;
        case 3:
            assetSwap16Range(buf, o + 4, o + 0x10, size);
            o += 0x10;
            break;
        case 4:
            assetCheck("t08 spline4", o, 0x20, size);
            keys = (u16)asw16(buf, o + 4);
            o += 0x20;
            for (j = 0; j < keys; j++, o += 0xE) {
                assetSwap16Range(buf, o, o + 0xC, size);
            }
            break;
        case 5:
            assetCheck("t08 spline5", o, 0x14, size);
            keys = (u16)asw16(buf, o + 4);
            o += 0x14;
            for (j = 0; j < keys; j++, o += 8) {
                assetSwap16Range(buf, o, o + 6, size);
            }
            break;
        default:
            o += 4;
            continue; /* untyped header: not counted (camLoadPathData skip) */
        }
        n--;
    }
}
