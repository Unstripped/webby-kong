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

/* ---- widescreen ---- */
f32 portGfxWideFactor(void); /* displayed width / the game's 4:3 width (>= 1); 1 when not wide */
f32 portGfxWideHalfYaw(f32 halfYawDeg); /* horizontal half-FOV widened by window aspect / (4:3); identity when not wide */
void portGfxWideClipX(s16 *left, s16 *right); /* screen x bounds reaching the image edge move out to the wide edge */

/* ---- debug trace (port/src/debug.c) ---- */
#define PORT_TRACE_MAP 0x1u   /* map transitions */
#define PORT_TRACE_CAM 0x2u   /* camera path start/end */
#define PORT_TRACE_ACTOR 0x4u /* actor spawn/despawn (noisy) */
extern u32 gPortTraceMask;
void portTrace(u32 cat, const char *fmt, ...);
#define PORT_TRACE(cat, ...) do { if (gPortTraceMask & (cat)) portTrace((cat), __VA_ARGS__); } while (0)

/* ---- quicksave resume point (port/src/qs.c; captured and applied by game code) ---- */
typedef struct {
    s32 pending;   /* set by port_resume_request; the game clears it when it starts the resume */
    s32 slot;      /* EEPROM file slot (gEepromCurrentSlot) */
    s32 kong;      /* gPlayerCharacterId[0], 0..4 */
    s32 map;
    s32 exit;
    s32 hasPos;    /* 1: x/y/z/yaw replace the exit's spawn point */
    f32 x, y, z;
    s32 yaw;       /* 0..4095 */
    s32 spawnArmed; /* game-internal: the position override survives until mapRequestTransition loads the map */
} PortResumePoint;
extern PortResumePoint gPortResume;
/* D_8076AEE2 (spawn override flags) bit set by a resume spawn: playerWarpToSpawnPoint faces D_8076AEF0 exactly */
#define PORT_SPAWN_EXACT_YAW 0x4000
s32 mapCaptureResumePoint(PortResumePoint *out, const char **reason); /* src/global/3C10.c */
void menuStartGameAtResume(void);                                      /* src/global/117190.c */

#endif
