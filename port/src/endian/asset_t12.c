/* Asset table 12: text/glyph-grid resource lists ("resNode" assets).
 *
 * Loaded only by resNodeGetOrLoad (global/112080.c, assetLoad(0xC, id, 1, 1)); the raw buffer is consumed once
 * by resParseList, which walks it as a packed byte stream (streamReadBytes = memcpy + advance, so no field is
 * aligned) into heap structs, then the asset buffer is freed. Every index (0..0x2A in the US ROM) has the same
 * grammar; all 43 entries were checked to parse exactly to their end. Stream, in order:
 *
 *   u8   listCount                                  resParseList -> ResParsedList.count (byte)
 *   listCount x list:                               resParseItemList / resParseItem
 *     u8   itemCount                                ResItemList.count (byte)
 *     itemCount x item:
 *       u8   flags                                  ResItem.flags (byte): 1 text, 2 grid, 4 has data
 *       if flags & 4: 4 bytes data                  copied to ResItem.data -> ResRow.data; its only reader,
 *                                                   func_806A3700 (global/A8400.c), is an empty stub, so
 *                                                   the bytes are left untouched
 *       if flags & 1: one span record               resParseVariant/resParseRecord (loop runs exactly once)
 *         u8   spanCount                            ResRecord.count (byte)
 *         spanCount x 8-byte ResTextSpan:
 *           +0 s32 offset  (into payload)           textLayoutAppendCommandList, textTableGetString
 *           +4 u16 length                           textLayoutAppendCommandList, textTableGetString
 *           +6 2 bytes pad
 *       else if flags & 2: one cell record          resParseVariant
 *         u8   cellCount                            ResRecord.count (byte)
 *         cellCount x 4-byte ResGridCell:
 *           +0 u16 effectId                         resGridBuildGroups (gEffectGridAssets index, glyph attr)
 *           +2 u8  sub                              resGridBuildGroups (glyph code)
 *           +3 1 byte pad
 *       f32  duration                               ResItem.duration -> resApplyKind (ResRow.timer = d * 30)
 *   u16  payloadLength                              ResParsedList.length -> resParseList heapAlloc/memcpy
 *   payloadLength bytes payload                     text bytes: textLayoutAppendRows, textTableGetString
 *
 * The walk reads each count/flag byte before using it, swaps every multi-byte field in place (asw16/asw32)
 * and bounds-checks every read against size. */
#include "endian/asset_swap.h"

static u8 readByte(const u8 *buf, u32 *off, u32 size, const char *what) {
    assetCheck(what, *off, 1, size);
    return buf[(*off)++];
}

void portSwapAssetTable12(s32 index, u8 *buf, u32 size) {
    u32 off = 0;
    u32 listCount, itemCount, count, flags, payloadLength, l, it, i;

    (void)index;
    listCount = readByte(buf, &off, size, "t12 list count");
    for (l = 0; l < listCount; l++) {
        itemCount = readByte(buf, &off, size, "t12 item count");
        for (it = 0; it < itemCount; it++) {
            flags = readByte(buf, &off, size, "t12 item flags");
            if (flags & 4) {
                assetCheck("t12 item data", off, 4, size);
                off += 4;
            }
            if (flags & 1) {
                count = readByte(buf, &off, size, "t12 span count");
                assetCheck("t12 spans", off, count * 8, size);
                for (i = 0; i < count; i++, off += 8) {
                    asw32(buf, off);     /* s32 offset into payload */
                    asw16(buf, off + 4); /* u16 length */
                }
            } else if (flags & 2) {
                count = readByte(buf, &off, size, "t12 cell count");
                assetCheck("t12 cells", off, count * 4, size);
                for (i = 0; i < count; i++, off += 4) {
                    asw16(buf, off); /* u16 effectId */
                }
            }
            assetCheck("t12 item duration", off, 4, size);
            asw32(buf, off); /* f32 duration */
            off += 4;
        }
    }
    assetCheck("t12 payload length", off, 2, size);
    payloadLength = asw16(buf, off);
    off += 2;
    assetCheck("t12 payload", off, payloadLength, size);
}
