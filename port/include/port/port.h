#ifndef PORT_PORT_H
#define PORT_PORT_H

/* Shared by game TUs (force-included via prelude.h, which defines PORT_GAME_TU) and port TUs.
 * Only ultratypes are assumed; no libc headers in game TUs. */
#ifndef PORT_GAME_TU
#include <stddef.h> /* musl size_t (unsigned long) must win over ultratypes' unsigned int */
#ifndef _SIZE_T_DEF
#define _SIZE_T_DEF
#endif
#endif
#include <PR/ultratypes.h>

/* ---- memory model: low 8 MB of wasm memory is RDRAM, mapped 1:1 ---- */
#define K0_HOST(a) ((void *)((u32)(a) & 0x1FFFFFFFu))
#define PORT_PTR_MIN 0x00024000u
#define PORT_GAME_LIMIT 0x01000000u
#define PORT_IS_PTR(x) ((((u32)(x)) - 0x80000000u) < 0x20000000u || (((u32)(x)) >= PORT_PTR_MIN && ((u32)(x)) < PORT_GAME_LIMIT))

u32 portHostAddr(u32 a);

/* ---- endianness ---- */
#define PORT_BE16(x) ((u16)__builtin_bswap16((u16)(x)))
#define PORT_BE32(x) ((u32)__builtin_bswap32((u32)(x)))
#define PORT_BE64(x) ((u64)__builtin_bswap64((u64)(x)))

static inline u16 portReadBE16(const void *p) {
    const u8 *b = (const u8 *)p;
    return (u16)((b[0] << 8) | b[1]);
}
static inline u32 portReadBE32(const void *p) {
    const u8 *b = (const u8 *)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}
static inline u64 portReadBE64(const void *p) {
    return ((u64)portReadBE32(p) << 32) | portReadBE32((const u8 *)p + 4);
}
void portSwap16(void *p, u32 count);
void portSwap32(void *p, u32 count);

/* ---- threading ---- */
void portIdleWait(void);

/* ---- overlays ---- */
void portOverlayActivate(u8 overlayId);
void portOverlayEntry(void);

/* ---- asset / audio endianness ---- */
void portAssetSwap(s32 table, s32 index, void *buffer, u32 size);
void portAssetQueued(s32 slot, s32 table, s32 index, void *buffer, u32 size);
void portAssetDrained(s32 slot);
void portSwapCollisionTris(void *tris, u32 size);
void portSwapBankFile(void *ctl);
void portSwapSeqHeader(void *hdr);

/* ---- framebuffer coherence ---- */
void portGfxCpuWrote(const void *p, u32 len);
void portGfxSyncRdram(const void *p, u32 len);

/* ---- io registers ---- */
u32 portIoRead(u32 reg);
void portIoWrite(u32 reg, u32 value);

#endif
