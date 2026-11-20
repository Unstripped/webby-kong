/* 3D sound: packed directions (spatial.c) and the binaural renderer the audio HLE runs (hrtf.c). */
#ifndef PORT_AUDIO_HRTF_H
#define PORT_AUDIO_HRTF_H
#include <PR/ultratypes.h>

/* Packed direction, relative to the listener (the camera): bit 31 set, then
 *   bits 0..9   azimuth, 0..1023 = 0..360 degrees clockwise from straight ahead (90 = right)
 *   bits 10..17 elevation, 0..255 = -90..+90 degrees
 *   bits 18..24 clarity, 0..127: how much of the voice is rendered as a point source; the rest stays the
 *               game's centre pan (several emitters around the listener, or a source at the camera)
 * The END command word carries the same bits 0..24 plus the physical voice's slot and a reset flag. */
#define SPATIAL_VALID 0x80000000u
#define SPATIAL_FIELDS 0x01FFFFFFu
#define SPATIAL_AZ(d) ((d) & 0x3FFu)
#define SPATIAL_EL(d) (((d) >> 10) & 0xFFu)
#define SPATIAL_CLARITY(d) (((d) >> 18) & 0x7Fu)
#define SPATIAL_SLOT(c) (((c) >> 25) & 0x1Fu)
#define SPATIAL_RESET 0x40000000u
#define HRTF_VOICES 32  /* physical voices: the game allocates 30 */
#define HRTF_FRAME 184  /* samples per ENVMIXER */

/* Measured HRIRs (hrir_kemar.c, tools/gen_hrir.py): rings of constant elevation, right half (azimuth 0..180),
 * minimum phase, s16 scaled by 2^Q; [direction][ear 0 = left, 1 = right][tap] */
#define HRIR_RINGS 14
#define HRIR_DIRS 368
typedef struct {
    s8 elevation;
    u8 count;
    u16 first;
} HrirRing;
extern const HrirRing gHrirRings[HRIR_RINGS];
extern const u8 gHrirAzimuth[HRIR_DIRS];
extern const s32 gHrir22Q;
extern const s32 gHrir44Q;
extern const s16 gHrir22[HRIR_DIRS][2][24];
extern const s16 gHrir44[HRIR_DIRS][2][48];

/* END command: savedL/R is the dry mix before the voice's ENVMIXER, dryL/R after it (184 samples each);
 * rewrites dryL/R with the voice re-rendered binaurally. */
void portHrtfMix(u32 cmd, const s16 *savedL, const s16 *savedR, s16 *dryL, s16 *dryR);

#endif
