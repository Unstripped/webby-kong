#ifndef PORT_ASSET_SWAP_H
#define PORT_ASSET_SWAP_H
/* Shared helpers for the per-table asset swappers (port/src/endian/asset_tNN.c).
 * Asset bytes arrive exactly as in ROM (big-endian). A swapper converts every multi-byte field the game
 * reads natively; texels, TLUTs, byte arrays and packed bitstreams stay as they are. */
#include "port_internal.h"

typedef void (*AssetSwapFn)(s32 index, u8 *buf, u32 size);

typedef struct {
    u8 kind; /* 0 = unregistered, 1 = SWAP_NONE, 2 = SWAP_FN */
    AssetSwapFn fn;
} AssetSwapper;

/* bounds-checked field swaps; `what` names the field in the abort message */
void assetCheck(const char *what, u32 off, u32 len, u32 size);
static inline u16 asw16(u8 *buf, u32 off) {
    u16 v = portReadBE16(buf + off);
    memcpy(buf + off, &v, 2);
    return v;
}
static inline u32 asw32(u8 *buf, u32 off) {
    u32 v = portReadBE32(buf + off);
    memcpy(buf + off, &v, 4);
    return v;
}
/* big-endian reads of fields not swapped yet */
static inline u32 abe32(const u8 *buf, u32 off) { return portReadBE32(buf + off); }
static inline u16 abe16(const u8 *buf, u32 off) { return portReadBE16(buf + off); }

/* [start, end): u32 word / u16 halfword arrays */
void assetSwap32Range(u8 *buf, u32 start, u32 end, u32 size);
void assetSwap16Range(u8 *buf, u32 start, u32 end, u32 size);
/* [start, end): Gfx commands -> native u32 pairs (then validated: every opcode a defined F3DEX2/RDP one) */
void assetSwapGfxRange(u8 *buf, u32 start, u32 end, u32 size, s32 table, s32 index);
/* [start, end): Vtx -> native ob[3], flag, tc[2]; colour/normal bytes untouched */
void assetSwapVtxRange(u8 *buf, u32 start, u32 end, u32 size);

/* registry entries (asset_swap.c), one per swapped table: void portSwapAssetTableNN(s32, u8 *, u32) */
#define ASSET_SWAPPER(nn) void portSwapAssetTable##nn(s32 index, u8 *buf, u32 size)
ASSET_SWAPPER(01);
ASSET_SWAPPER(02);
ASSET_SWAPPER(03);
ASSET_SWAPPER(04);
ASSET_SWAPPER(05);
ASSET_SWAPPER(08);
ASSET_SWAPPER(09);
ASSET_SWAPPER(10);
ASSET_SWAPPER(11);
ASSET_SWAPPER(12);
ASSET_SWAPPER(13);
ASSET_SWAPPER(15);
ASSET_SWAPPER(16);
ASSET_SWAPPER(17);
ASSET_SWAPPER(18);
ASSET_SWAPPER(19);
ASSET_SWAPPER(20);
ASSET_SWAPPER(21);
ASSET_SWAPPER(22);
ASSET_SWAPPER(23);
ASSET_SWAPPER(24);
#endif
