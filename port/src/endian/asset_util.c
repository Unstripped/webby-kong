#include "endian/asset_swap.h"

void assetCheck(const char *what, u32 off, u32 len, u32 size) {
    if (off > size || len > size - off) {
        portAbort("PORT: asset swap %s out of range (off 0x%x len 0x%x size 0x%x)", what, off, len, size);
    }
}

void assetSwap32Range(u8 *buf, u32 start, u32 end, u32 size) {
    u32 o;
    assetCheck("u32 range", start, end - start, size);
    for (o = start; o + 4 <= end; o += 4) {
        asw32(buf, o);
    }
}

void assetSwap16Range(u8 *buf, u32 start, u32 end, u32 size) {
    u32 o;
    assetCheck("u16 range", start, end - start, size);
    for (o = start; o + 2 <= end; o += 2) {
        asw16(buf, o);
    }
}

/* F3DEX2 + RDP opcodes (include/PR/gbi.h, F3DEX_GBI_2); 0x00 doubles as G_NOOP and the game's markers */
static int validOpcode(u8 op) {
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7: case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4:
    case 0xE5: case 0xE6: case 0xE7: case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED:
    case 0xEE: case 0xEF: case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6:
    case 0xF7: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        return 1;
    default:
        return 0;
    }
}

void assetSwapGfxRange(u8 *buf, u32 start, u32 end, u32 size, s32 table, s32 index) {
    u32 o;
    assetCheck("Gfx range", start, end - start, size);
    if ((end - start) & 7) {
        portAbort("PORT: Gfx range 0x%x-0x%x not a multiple of 8 (table %d index %d)", start, end, (int)table, (int)index);
    }
    for (o = start; o < end; o += 8) {
        u32 w0 = asw32(buf, o);
        asw32(buf, o + 4);
        if (!validOpcode(w0 >> 24)) {
            portAbort("PORT: bad DL after swap table %d index %d (offset 0x%x op 0x%02x)", (int)table, (int)index, o,
                      (unsigned)(w0 >> 24));
        }
    }
}

void assetSwapVtxRange(u8 *buf, u32 start, u32 end, u32 size) {
    u32 o;
    assetCheck("Vtx range", start, end - start, size);
    for (o = start; o + 16 <= end; o += 16) {
        asw16(buf, o + 0);
        asw16(buf, o + 2);
        asw16(buf, o + 4);
        asw16(buf, o + 6);
        asw16(buf, o + 8);
        asw16(buf, o + 10);
    }
}
