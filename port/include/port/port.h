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
/* x span of the displayed wide image in the game's pixels (its 4:3 image covers 0..VI width); 0 when not wide.
 * Wide means the side areas are open, so the game's overscan border shows scene too: everything in this span is seen. */
int portGfxWideSpanX(f32 *left, f32 *right);
f32 portGfxWideHalfYaw(f32 halfYawDeg, f32 widen); /* half-FOV whose tangent is widen times as large */
void portGfxWideClipX(s16 *left, s16 *right); /* screen x bounds reaching the image edge move out to the wide edge */
/* G_NOOP tag: the renderer blacks the colour image being drawn left and right of the current scissor, as the N64
 * shows it, though the side areas and the overscan border are open (the screen wipe's 4:3 capture would otherwise
 * sit beside live scene and spill its tiles past the scissor) */
#define PORT_GFX_BLANK_SIDES 0x57494445u /* "WIDE" */

/* ---- draw distance (port/src/main.c; --drawdist, ?drawdist=) ---- */
extern f32 gPortDrawDistance; /* object draw distance / enemy spawn range multiplier, capped at the far clip; 1 = N64 */

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

/* ---- level progress for the web menu (port/src/progress.c; filled by game code) ---- */
typedef struct {
    s16 goldenBananas; /* KongInventory.goldenBananas[world], of 5; -1: none in this level */
    s16 bananas;       /* coloredBananas + bananasDelivered of the world, of 100; -1: none */
    s8 medal;          /* banana medal flag; -1: none */
    s8 blueprint;      /* 0 missing, 1 found, 2 turned in; -1: none */
} PortKongProgress;
typedef struct {
    s32 world; /* mapGetHubWorldIndex(map, 1): 0-6 Japes..Castle, 7 Isles (and lobbies), 8 Helm */
    s32 kong;  /* gPlayerCharacterId[0] when 0..4, else -1 */
    PortKongProgress kongs[5]; /* DK, Diddy, Lanky, Tiny, Chunky */
    s32 fairies, fairiesTotal;
    s32 crowns, crownsTotal;
    s32 keys, keysTotal;
} PortLevelProgress;
s32 mapCaptureLevelProgress(PortLevelProgress *out, const char **reason); /* src/global/3C10.c */

/* ---- dashboard save (port/src/save.c, progress.c; filled and driven by game code) ---- */
typedef struct {
    s32 slot, percent, goldenBananas, keys, crowns, medals, fairies, blueprints, kongs, playSeconds;
} PortSaveSummary;
s32 mapCaptureSaveSummary(PortSaveSummary *out, const char **reason); /* src/global/3C10.c */
extern s32 gPortSaveRequest;  /* web/menu.js exit: mainThreadLoop runs eepromSavePlayerData once (src/global/450.c) */
extern s32 gPortEepromQueued; /* EEPROM thread commands sent and not yet finished (src/global/104F0.c) */

/* ---- audio output rate and 3D sound (port/src/audio/spatial.c, hrtf.c; --audio-rate, --hrtf, web settings) ---- */
extern s32 gPortAudioRate; /* synthesizer and AI rate: 22050 (N64) or 44100; fixed at boot, restored by a quickload */
extern s32 gPortAudioHrtf; /* 1: positional sound effects are rendered binaurally (headphones); switchable live */
/* the synthesizer's heap at 44100: the game's 0x7BD80 bytes are full at 22050, 44100 needs 0x93200 */
#define PORT_AUDIO_HEAP44_SIZE 0xB0000
extern u8 gPortAudioHeap44[PORT_AUDIO_HEAP44_SIZE];
/* Audio commands emitted around one voice's ENVMIXER (n_alEnvmixerPull) when it is rendered binaurally:
 * BEGIN snapshots the dry mix, END replaces the voice's (centre-panned) dry contribution by its HRTF render.
 * Opcodes 0x7E/0x7F have no handler in the game's ucode; only the port's HLE executes them. */
#define PORT_AOP_SPATIAL_BEGIN 0x7E
#define PORT_AOP_SPATIAL_END 0x7F
/* sfx code (src/global/7130.c): directions are relative to the camera; a packed direction of 0 means none */
void portSpatialReset(s32 enabled); /* start one sound's emitter sum; enabled 0: the sound stays plain stereo */
void portSpatialFlat(void);         /* the game centres this sound: no direction */
void portSpatialEmitter(f32 sinAz, f32 cosAz, f32 dx, f32 dy, f32 dz, f32 fdx, f32 fdy, f32 fdz, f32 level);
u32 portSpatialResult(void);        /* level-weighted direction of the emitters since the reset */
/* one emitter: game azimuth (radians from the camera yaw, + = left) and source / focus minus camera */
u32 portSpatialAt(f32 az, f32 dx, f32 dy, f32 dz, f32 fdx, f32 fdy, f32 fdz);
void portSpatialPending(u32 dir);   /* direction for the next n_alSndpPlay; reset to 0 after the call */
struct GfSndState;
void n_alSndpSetSpatial(struct GfSndState *state, u32 dir); /* src/global/13A7A0.c: (re)bind a playing sound */
/* sound player (src/global/13A7A0.c) and synthesizer (src/global/1435C0.c) */
u32 portSpatialTakePending(void);
void portSpatialBind(void *voice, u32 dir); /* ALVoice of a sound state; 0 unbinds */
u32 portSpatialOf(void *voice);
s32 portSpatialPan(void *voice, s32 pan);   /* 0x40 for a voice rendered binaurally (it is mixed centred) */
void portSpatialVoiceStart(void *pvoice);   /* a physical voice starts a new sound: its filter history resets */
u32 portSpatialEnvmix(void *pvoice, void *voice, s32 *retarget); /* END command word, 0: plain envmixer */

#endif
