/*
 * C transcription of decomp/asm/global/16B60.s (handwritten): table-based trigonometry.
 *
 * Tables (all generated from the ROM by tools/gen_data.py, native u32 words holding the original
 * big-endian word values):
 *   D_80611F0C  u16 asin table, 0x402 halfwords packed two per word (data in .text)
 *   D_80612710  u16 asin tail table for inputs >= 0xFFC0, 64 halfwords packed two per word (data in .text)
 *   D_8061ACA0  f32 quarter-wave sine table, 0x204 entries: D_8061ACA0[k] = sin(k * (pi/2) / 0x200)
 */
#include "port_internal.h"

extern u32 D_80611F0C[];
extern u32 D_80612710[];
extern f32 D_8061ACA0[];

/* Halfword i of a table whose bytes were big-endian .word directives, now stored as native u32 words. */
static inline u32 beHalf(const u32 *words, u32 i) {
    u32 w = words[i >> 1];
    return (i & 1) ? (w & 0xFFFF) : (w >> 16);
}

/*
 * func_80611E88: non-ABI helper (input $v1, result $fp, preserves everything else).
 * Linear interpolation in the 1025-entry asin table; inputs whose (x >> 6) >= 0x3FF (including
 * x >= 0x10000 and negative x, as srl is logical) index the 64-entry tail table by x & 0x3F.
 */
static s32 func_80611E88(u32 v1) {
    u32 a1 = v1 >> 6;
    u32 fp;
    u32 frac;

    if ((s32)a1 < 0x3FF) {
        fp = beHalf(D_80611F0C, a1);
        frac = v1 & 0x3F;
        fp = fp + ((u32)((beHalf(D_80611F0C, a1 + 1) - fp) * frac) >> 6);
    } else {
        fp = beHalf(D_80612710, v1 & 0x3F);
    }
    return (s32)fp;
}

/* mathAsinFixed: asin(x / 65536) in units of 65536 per full turn (0..0x4000). */
s32 mathAsinFixed(s32 x) {
    return func_80611E88((u32)x);
}

/*
 * Shared body of mathSinF/mathCosF (mathCosF adds 0x400 to the full 32-bit $a0 and falls through;
 * only bits 0..11 of the angle are used, so the untruncated sum behaves like the s16 one).
 * Angle units: 0x1000 per full turn. Odd angles average two adjacent half-resolution entries.
 */
static f32 sinCore(s32 a0) {
    u32 t0 = (u32)a0 & 0x3FF;
    u32 idx;
    f32 r;

    if ((u32)a0 & 0x400) {
        t0 = 0x400 - t0;
    }
    idx = t0 >> 1;
    r = D_8061ACA0[idx];
    if ((u32)a0 & 1) {
        r = (r + D_8061ACA0[idx + 1]) * 0.5f;
    }
    if ((u32)a0 & 0x800) {
        r = 0.0f - r;
    }
    return r;
}

/* mathSinF: sine of an angle in 0x1000-per-turn units. */
f32 mathSinF(s16 angle) {
    return sinCore(angle);
}

/* mathCosF: cosine via sine of angle + 0x400. */
f32 mathCosF(s16 angle) {
    return sinCore((s32)angle + 0x400);
}

/* func_80612800 (unlabelled entry after mathSinF): tangent = sin(a) / cos(a). */
f32 func_80612800(s16 angle) {
    f32 s = sinCore(angle);
    f32 c = sinCore((s32)angle + 0x400);
    return s / c;
}
