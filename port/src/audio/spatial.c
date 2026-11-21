/* 3D sound, game side. The sfx code (src/global/7130.c) works out each positional sound's direction from the
 * camera alongside the stereo pan it already computes; the direction travels with the sound player's state
 * (13A7A0.c, keyed by its ALVoice) to the synthesizer, whose physical voices (1435C0.c) then mix centred and
 * bracket their ENVMIXER with PORT_AOP_SPATIAL_BEGIN/END, which the audio HLE renders binaurally (hrtf.c).
 * Only sounds the game pans get a direction; distance volume and the behind-the-camera damping stay the game's. */
#include "port_internal.h"
#include "audio/hrtf.h"
#include <math.h>

s32 gPortAudioRate = 22050;
s32 gPortAudioHrtf;
u8 gPortAudioHeap44[PORT_AUDIO_HEAP44_SIZE] __attribute__((aligned(16)));

#define PI_F 3.14159265f
/* Closer to the camera than this (game units) a source fades into the plain centre pan: its direction swings
 * wildly there. The camera trails the kong by a few hundred units. */
#define NEAR_UNITS 60.0f

/* emitters of the sound being summed (portSpatialReset .. portSpatialResult) */
static struct {
    f32 x, y, z, level;
    s32 enabled, flat;
} sSum;
static u32 sPending;

typedef struct {
    u32 voice; /* ALVoice inside a sound player state; 0 = free */
    u32 dir;
} VoiceDir;
static VoiceDir sVoiceDir[64];

typedef struct {
    u32 pvoice; /* N_PVoice; the index is the renderer's filter slot */
    u8 spatial; /* the last ENVMIXER was rendered binaurally */
    u8 start;   /* a new sound started on it since */
} PVoiceSlot;
static PVoiceSlot sPVoice[HRTF_VOICES];
static int sLoggedSlots;

/* unit vector (right, up, front) of a source; *nearWeight fades it out close to the camera */
static void direction(f32 sinAz, f32 cosAz, f32 dx, f32 dy, f32 dz, f32 fdx, f32 fdy, f32 fdz, f32 v[3],
                      f32 *nearWeight) {
    f32 h = sqrtf(dx * dx + dz * dz);
    f32 n = sqrtf(sinAz * sinAz + cosAz * cosAz);
    f32 el = atan2f(dy, h) - atan2f(fdy, sqrtf(fdx * fdx + fdz * fdz)); /* relative to where the camera looks */
    f32 dist = sqrtf(h * h + dy * dy);
    if (n < 1e-6f) {
        sinAz = 0.0f;
        cosAz = 1.0f;
    } else {
        sinAz /= n;
        cosAz /= n;
    }
    if (el > PI_F / 2) {
        el = PI_F / 2;
    } else if (el < -PI_F / 2) {
        el = -PI_F / 2;
    }
    v[0] = -cosf(el) * sinAz; /* the game's azimuth grows to the left */
    v[1] = sinf(el);
    v[2] = cosf(el) * cosAz;
    *nearWeight = dist >= NEAR_UNITS ? 1.0f : dist / NEAR_UNITS;
}

static u32 pack(f32 x, f32 y, f32 z, f32 clarity) {
    f32 len = sqrtf(x * x + y * y + z * z), az, el, s;
    u32 c;
    if (len < 1e-6f) {
        return 0;
    }
    c = (u32)(clarity * 127.0f + 0.5f);
    if (c == 0) {
        return 0;
    }
    if (c > 127) {
        c = 127;
    }
    az = atan2f(x, z) * (180.0f / PI_F);
    if (az < 0.0f) {
        az += 360.0f;
    }
    s = y / len;
    el = asinf(s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s) * (180.0f / PI_F);
    return SPATIAL_VALID | ((u32)(az * (1024.0f / 360.0f) + 0.5f) & 0x3FFu) |
           ((u32)((el + 90.0f) * (255.0f / 180.0f) + 0.5f) << 10) | (c << 18);
}

void portSpatialReset(s32 enabled) {
    sSum.x = sSum.y = sSum.z = sSum.level = 0.0f;
    sSum.enabled = enabled;
    sSum.flat = 0;
}

void portSpatialFlat(void) {
    sSum.flat = 1;
}

void portSpatialEmitter(f32 sinAz, f32 cosAz, f32 dx, f32 dy, f32 dz, f32 fdx, f32 fdy, f32 fdz, f32 level) {
    f32 v[3], w;
    if (!sSum.enabled || !(level > 0.0f)) {
        return;
    }
    direction(sinAz, cosAz, dx, dy, dz, fdx, fdy, fdz, v, &w);
    w *= level;
    sSum.x += v[0] * w;
    sSum.y += v[1] * w;
    sSum.z += v[2] * w;
    sSum.level += level;
}

/* Emitters on opposite sides cancel: the clarity (|sum| / total level) leaves the rest in the centre pan. */
u32 portSpatialResult(void) {
    f32 len;
    if (!sSum.enabled || sSum.flat || !(sSum.level > 0.0f)) {
        return 0;
    }
    len = sqrtf(sSum.x * sSum.x + sSum.y * sSum.y + sSum.z * sSum.z);
    return pack(sSum.x, sSum.y, sSum.z, len / sSum.level);
}

u32 portSpatialAt(f32 az, f32 dx, f32 dy, f32 dz, f32 fdx, f32 fdy, f32 fdz) {
    f32 v[3], w;
    direction(sinf(az), cosf(az), dx, dy, dz, fdx, fdy, fdz, v, &w);
    return pack(v[0], v[1], v[2], w);
}

void portSpatialPending(u32 dir) {
    sPending = dir;
}

u32 portSpatialTakePending(void) {
    u32 dir = sPending;
    sPending = 0;
    return dir;
}

void portSpatialBind(void *voice, u32 dir) {
    u32 key = (u32)(uintptr_t)voice;
    int i, freeIdx = -1;
    if (key == 0) {
        return;
    }
    for (i = 0; i < (int)(sizeof sVoiceDir / sizeof sVoiceDir[0]); i++) {
        if (sVoiceDir[i].voice == key) {
            if (dir != 0) {
                sVoiceDir[i].dir = dir;
            } else {
                sVoiceDir[i].voice = 0;
            }
            return;
        }
        if (sVoiceDir[i].voice == 0 && freeIdx < 0) {
            freeIdx = i;
        }
    }
    if (dir != 0 && freeIdx >= 0) {
        sVoiceDir[freeIdx].voice = key;
        sVoiceDir[freeIdx].dir = dir;
    }
}

u32 portSpatialOf(void *voice) {
    u32 key = (u32)(uintptr_t)voice;
    int i;
    if (key == 0) {
        return 0;
    }
    for (i = 0; i < (int)(sizeof sVoiceDir / sizeof sVoiceDir[0]); i++) {
        if (sVoiceDir[i].voice == key) {
            return sVoiceDir[i].dir;
        }
    }
    return 0;
}

static u32 activeDir(void *voice) {
    return gPortAudioHrtf ? portSpatialOf(voice) : 0;
}

s32 portSpatialPan(void *voice, s32 pan) {
    return activeDir(voice) != 0 ? 0x40 : pan;
}

static PVoiceSlot *slotOf(void *pvoice, int assign) {
    u32 key = (u32)(uintptr_t)pvoice;
    int i;
    for (i = 0; i < HRTF_VOICES; i++) {
        if (sPVoice[i].pvoice == key) {
            return &sPVoice[i];
        }
    }
    if (!assign) {
        return NULL;
    }
    for (i = 0; i < HRTF_VOICES; i++) {
        if (sPVoice[i].pvoice == 0) {
            sPVoice[i].pvoice = key;
            sPVoice[i].spatial = 0;
            sPVoice[i].start = 1;
            return &sPVoice[i];
        }
    }
    if (!sLoggedSlots) {
        sLoggedSlots = 1;
        portLog("PORT: more than %d physical voices; the rest stay plain stereo", HRTF_VOICES);
    }
    return NULL;
}

void portSpatialVoiceStart(void *pvoice) {
    PVoiceSlot *p = slotOf(pvoice, 0);
    if (p != NULL) {
        p->start = 1;
    }
}

u32 portSpatialEnvmix(void *pvoice, void *voice, s32 *retarget) {
    u32 dir = activeDir(voice), cmd;
    PVoiceSlot *p = slotOf(pvoice, 1);
    *retarget = 0;
    if (p == NULL) {
        return 0;
    }
    if (dir == 0) {
        *retarget = p->spatial; /* back to the game's pan */
        p->spatial = 0;
        return 0;
    }
    *retarget = !p->spatial;
    cmd = (dir & SPATIAL_FIELDS) | ((u32)(p - sPVoice) << 25) | (!p->spatial || p->start ? SPATIAL_RESET : 0);
    p->spatial = 1;
    p->start = 0;
    return cmd;
}
