/* Table 17: recorded input demos (bonus-stage / attract playback). Consumer: func_8060B750 ->
 * recordInitPositionBuffers (global/10130.c); playback reads in func_8060B4D4, func_8060B55C,
 * func_8060B430, func_8060B7C4, func_8060B7D0 (10130.c). Loaded from menuSetupBonusCharacter
 * (global/117190.c) only when gMenuTargetMode == 3.
 *
 * Layout (no alignment padding; every block is memcpy'd out):
 *   s16 inputCount                                  D_807463B0
 *   InputRecord[inputCount], 4 bytes each:          D_807463A8
 *     s16 durationFlags (bit 15: take next button word), s8 stickX, s8 stickY
 *   s16 buttonCount                                 D_807463A0
 *   u16 buttons[buttonCount]                        D_807ECE90 (OSContPad.button values)
 *   PositionRecord, 0x70 bytes                      D_807ECE20
 *     0x00 f32 x, y, z; 0x0C s16 yaw; 0x0E u8 map, exit, kongIndex, pad
 *     0x12 KongInventory (0x5E): u8 unk0[5], u8 unk5, u16 coins, u16 instrumentEnergy,
 *          u16 coloredBananas[14], bananasDelivered[14], goldenBananas[14]
 * Entry 5's data ends 0x20 bytes into goldenBananas (the game's memcpy reads past it); fields beyond
 * `size` are left alone. */
#include "endian/asset_swap.h"

static void sw16If(u8 *buf, u32 off, u32 size) {
    if (off + 2 <= size) {
        asw16(buf, off);
    }
}

static void sw32If(u8 *buf, u32 off, u32 size) {
    if (off + 4 <= size) {
        asw32(buf, off);
    }
}

void portSwapAssetTable17(s32 index, u8 *buf, u32 size) {
    u32 o, n, i;
    (void)index;

    if (size == 0) {
        return;
    }
    assetCheck("t17 inputCount", 0, 2, size);
    n = (u16)asw16(buf, 0);
    o = 2;
    for (i = 0; i < n; i++, o += 4) {
        assetCheck("t17 input", o, 4, size);
        asw16(buf, o);
    }

    assetCheck("t17 buttonCount", o, 2, size);
    n = (u16)asw16(buf, o);
    o += 2;
    assetSwap16Range(buf, o, o + n * 2, size);
    o += n * 2;

    /* PositionRecord */
    sw32If(buf, o + 0x00, size);
    sw32If(buf, o + 0x04, size);
    sw32If(buf, o + 0x08, size);
    sw16If(buf, o + 0x0C, size);
    for (i = 0x18; i < 0x70; i += 2) {
        sw16If(buf, o + i, size);
    }
}
