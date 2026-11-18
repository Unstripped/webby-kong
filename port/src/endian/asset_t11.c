/* Asset table 11: animation clips (AnimClip, include/actor.h), loaded by controllerAnimCacheGetOrLoad (17B90.c,
 * `assetLoad(0xB, id, 1, 1)`), which then adds 6 to the u16 at +0x06 in place, so that field must be native
 * before it returns. Format notes: decomp/tools/animclip.py (parses all 1731 US clips to their exact end).
 *
 * Header:
 *   0x00 f32 speed           native  controllerAnimSetState / controllerAnimSetDirect /
 *                                    controllerAnimSetStateIfChanged (17B90.c), 19A00.c:861
 *   0x04 u16 flags           native  same sites (slot->flags = clip->flags), scriptOpAnimResetFrameRange (19A00.c),
 *                                    func_806195D0 (port/src/asm/1E2D0.c: `LH(a2, 4) & 0x20`)
 *   0x06 u16 data offset - 6 native  controllerAnimCacheGetOrLoad (`+= 6`), 1E2D0.c (`a2 + LH(a2, 6)`)
 *   0x08 s16 root X/Y/Z base native  1E2D0.c decodePose (`LH(a2, 8 + 2 * k)`)
 *   0x0E u8 root X/Y/Z bit widths, 0x11 u8 channel count + 1, 0x12 u8 frame count (AnimClip.frameCount, 17B90.c,
 *        19A00.c), 0x13 u8 row stride                                        bytes (1E2D0.c LBU)
 * 0x14 descriptors       native u16  1E2D0.c decodePose / blendPose (`LHU(t3, 0)`, `LHU(t3, 2)`, `LHU(t3, 4)`):
 *   per channel (u8[0x11] - 1 of them), per component X/Y/Z: one u16 `d`; if `d & 0x10` a second u16 (scale
 *   base/bits) follows. The descriptors end exactly at the data offset.
 * data offset .. end: keyframe rows (u8[0x13] bytes each, u8[0x12] rows), a packed MSB-first bitstream read by
 *   1E2D0.c through portReadBE64 windows: stays big-endian, untouched. */
#include "endian/asset_swap.h"

#define ANIM_HEADER_SIZE 0x14

void portSwapAssetTable11(s32 index, u8 *buf, u32 size) {
    u32 p, comps, dataOff;
    u16 d;

    assetCheck("anim header", 0, ANIM_HEADER_SIZE, size);
    asw32(buf, 0x00);
    asw16(buf, 0x04);
    dataOff = asw16(buf, 0x06) + 6u;
    asw16(buf, 0x08);
    asw16(buf, 0x0A);
    asw16(buf, 0x0C);

    comps = buf[0x11] != 0 ? (u32)(buf[0x11] - 1) * 3 : 0;
    p = ANIM_HEADER_SIZE;
    while (comps-- != 0) {
        assetCheck("anim descriptor", p, 2, size);
        d = asw16(buf, p);
        p += 2;
        if (d & 0x10) {
            assetCheck("anim scale descriptor", p, 2, size);
            asw16(buf, p);
            p += 2;
        }
    }
    if (p != dataOff) {
        portAbort("PORT: anim %d descriptors end at 0x%x, header says data at 0x%x", (int)index, p, dataOff);
    }
}
