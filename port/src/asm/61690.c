/*
 * C transcription of decomp/asm/global/61690.s (handwritten, VRAM 0x8065C990).
 */
#include "port_internal.h"
#include <math.h>

/* Layout mirror of LightTrigger (build/tree/include/render.h), size 0x40. */
typedef struct LightTrigger {
    /* 0x00 */ f32 cosOuter;
    /* 0x04 */ f32 cosInner;
    /* 0x08 */ f32 coneScale;
    /* 0x0C */ f32 dirX;
    /* 0x10 */ f32 dirY;
    /* 0x14 */ f32 dirZ;
    /* 0x18 */ s32 rangeSq;
    /* 0x1C */ f32 invFalloffLength;
    /* 0x20 */ f32 falloffStart;
    /* 0x24 */ f32 invFadeLength;
    /* 0x28 */ f32 fadeStart;
    /* 0x2C */ f32 r;
    /* 0x30 */ f32 g;
    /* 0x34 */ f32 b;
    /* 0x38 */ s16 x;
    /* 0x3A */ s16 y;
    /* 0x3C */ s16 z;
    /* 0x3E */ u8 type;
    /* 0x3F */ u8 volumeIndex;
} LightTrigger;

_Static_assert(sizeof(LightTrigger) == 0x40, "LightTrigger layout");

extern f32 gLightBaseR; /* ambient r */
extern f32 gLightBaseG; /* ambient g */
extern f32 gLightBaseB; /* ambient b */
extern f32 D_80758D50; /* spot distance cutoff (1100.0) */

/* cvt.w.s then the asm's upper clamp at 255 (no lower clamp; sb keeps the low byte). */
static u8 lightChannelByte(f32 v) {
    s32 i = (s32)lrintf(v);

    if (0xFF < i) {
        i = 0xFF;
    }
    return (u8)i;
}

/*
 * lightApplyToVertices: for each vertex in [start, end) stepping `stride` vertices, sums ambient plus
 * every light's colour times its intensity, scales by 255 (or by the vertex colour byte when
 * useVertexColor), and writes the rounded, upper-clamped result into cn[0..2].
 *   type != 0 (spot): skip if dist^2 > rangeSq or dist^2 == 0; cone term from the normalised
 *     light->vertex vector dotted with dir; distance fade 1 - (d - 300) / 800 for 300 <= d < D_80758D50,
 *     0 beyond.
 *   type == 0 (point): skip if dist^2 > rangeSq; 1 inside falloffStart, else 1 - min((d - start) * inv, 1).
 * Squared distances are 64-bit as in the asm (dmult/dmultu). When useVertexColor is 0 the scale stays
 * 255.0 ($fs2 is only overwritten by vertex colours).
 */
void lightApplyToVertices(Vtx *start, Vtx *end, s32 stride, LightTrigger *lights, s32 lightCount, s32 useVertexColor) {
    u8 *v = (u8 *)start;
    u32 step = (u32)stride << 4;
    LightTrigger *lightsEnd = lights + lightCount;
    f32 scale = 255.0f;

    while (v < (u8 *)end) {
        Vtx *vtx = (Vtx *)v;
        f32 r = gLightBaseR;
        f32 g = gLightBaseG;
        f32 b = gLightBaseB;
        s32 vx = vtx->v.ob[0];
        s32 vy = vtx->v.ob[1];
        s32 vz = vtx->v.ob[2];
        LightTrigger *l;

        for (l = lights; l != lightsEnd; l++) {
            s32 dx = vx - l->x;
            s32 dy = vy - l->y;
            s32 dz = vz - l->z;
            s64 d2 = (s64)dx * dx + (s64)dy * dy + (s64)dz * dz;
            f32 intensity;

            if ((s64)l->rangeSq < d2) {
                continue;
            }
            if (l->type != 0) {
                f32 dist;
                f32 inv;
                f32 dot;

                if (d2 == 0) {
                    continue;
                }
                dist = sqrtf((f32)d2);
                inv = 1.0f / dist;
                dot = (f32)dx * inv * l->dirX;
                dot = dot + (f32)dy * inv * l->dirY;
                dot = dot + (f32)dz * inv * l->dirZ;
                if (dot < l->cosOuter) {
                    intensity = 0.0f;
                } else {
                    if (dot < l->cosInner) {
                        intensity = (dot - l->cosOuter) * l->coneScale;
                    } else {
                        intensity = 1.0f;
                    }
                    if (!(dist < 300.0f)) {
                        if (dist < D_80758D50) {
                            dist = dist - 300.0f;
                            intensity = intensity * (1.0f - dist / 800.0f);
                        } else {
                            intensity = 0.0f;
                        }
                    }
                }
            } else {
                f32 dist = sqrtf((f32)d2);

                if (dist < l->falloffStart) {
                    intensity = 1.0f;
                } else {
                    f32 t = (dist - l->falloffStart) * l->invFalloffLength;

                    if (!(t < 1.0f)) {
                        t = 1.0f;
                    }
                    intensity = 1.0f - t;
                }
            }
            r = r + l->r * intensity;
            g = g + l->g * intensity;
            b = b + l->b * intensity;
        }

        if (useVertexColor) {
            scale = (f32)vtx->v.cn[0];
        }
        r = r * scale;
        if (useVertexColor) {
            scale = (f32)vtx->v.cn[1];
        }
        g = g * scale;
        if (useVertexColor) {
            scale = (f32)vtx->v.cn[2];
        }
        b = b * scale;
        vtx->v.cn[0] = lightChannelByte(r);
        vtx->v.cn[1] = lightChannelByte(g);
        vtx->v.cn[2] = lightChannelByte(b);
        v += step;
    }
}
