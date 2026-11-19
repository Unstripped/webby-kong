/*
 * C transcription of decomp/asm/global/1E2D0.s (handwritten): skeletal animation pose builder.
 *
 * actorBuildBoneMatrices(actor) (func_806195D0) decodes the actor's packed animation keyframes, blends animation layers,
 * builds per-bone rotation/scale matrices (quaternion slerp-style blend or direct Euler), applies
 * per-bone scale/translation adjustments, chains the bones through the hierarchy starting from the
 * actor's world matrix, and finally converts every bone matrix in place into an N64 fixed-point Mtx.
 *
 * Entry points in the asm and what became of them (all internal except actorBuildBoneMatrices):
 *   func_806195D0  actorBuildBoneMatrices, external
 *   func_806197B8  alabel inside actorBuildBoneMatrices: two-pose blend; code after label `blend`
 *   func_80619894, func_806198B0, func_80619A88: alabels inside the blend loop (inlined)
 *   func_80619B44  non-ABI subroutine (in $s0, out FP regs): static halfAngleQuatTerms()
 *   func_80619B80, func_80619BBC, func_80619C08: alabels inside func_80619B44 (halfSinCos())
 *   func_80619C2C  non-ABI subroutine: static decodePose()
 *   func_80619F50  non-ABI subroutine: static blendPose()
 *   func_8061A004  alabel inside func_80619F50 (inlined)
 *   func_8061A1A0  non-ABI subroutine: static applyPoseOffsets(); func_8061A1B4 is its loop head
 *   func_8061A3A0, func_8061A420, func_8061A4C0: alabels in the direct (unblended) matrix path
 *   func_8061A61C, func_8061A638, func_8061A774: alabels of the scale / translation adjust passes
 *   func_8061A8AC, func_8061A938: hierarchy multiply loop (inlined in actorBuildBoneMatrices)
 *
 * The routine sets Status.FR on entry and clears it on exit, so all 32 FPRs are independent singles;
 * every $fXX / $fXXf register is a distinct f32 variable here, named after the asm register.
 * Operation order of every float op follows the asm exactly.
 *
 * Input: $a0 = the Actor (`void actorBuildBoneMatrices(Actor *)` in the decomp); the parameter is a pointer
 * (same wasm signature as Actor *).
 *
 * Memory accessed (offsets are bytes):
 *   actor+0x00  ptr to model info: +0x08 ptr bone table (0x10-byte records: u8 parent, u8 boneIndex,
 *               u8 channelIndex, pad, f32 x/y/z at +4/+8/+C), +0x20 u8 bone count
 *   actor+0x04  ptr to anim state ("sub4"): +0x00/+0x04/+0x08/+0x0C ptrs to anim slots, +0x10 f32 layer
 *               weight, +0x1C ptr offset list, +0x20 ptr scale list, +0x24 ptr translation list,
 *               +0x28/+0x2A u16 blend weights (0..256)
 *   actor+0x08  ptr to the bone matrix buffer (64 bytes per bone: f32 4x4, rewritten as Mtx)
 *   actor+0x0C  f32 4x4 world matrix (rows at +0x0C, +0x1C, +0x2C, translation +0x3C)
 *   anim slot:  +0x00 ptr animation data, +0x08 f32 frame fraction, +0x0C s16 frame, +0x0E s16 next frame
 *   animation data (an asset; native s16/u16/u8 header fields, packed big-endian keyframe bitstream):
 *     +0x04 s16 flags (0x20 = no interpolation), +0x06 s16 offset of keyframe data from the anim start,
 *     +0x08/+0x0A/+0x0C s16 root translation bases, +0x0E/+0x0F/+0x10 u8 root translation bit widths,
 *     +0x11 u8 bone count, +0x13 u8 bytes per keyframe, +0x14.. u16 channel descriptors (native);
 *     keyframe bitstream at anim + s16[+6] + frame * u8[+0x13], read with ldl/ldr as raw big-endian
 *     bytes (unaligned 64-bit windows), so those bytes must stay untouched by any asset swapper.
 *
 * Scratch buffers (generated bss): D_80746E20 saved actor pointer; D_80746E24 / D_80747040 rotation
 * channel arrays (s16 x3 per bone) for pose A / B; D_8074725C / D_80747478 scale arrays (u16 x3 per bone).
 */
#include "port_internal.h"
#include <math.h>

typedef s16 __attribute__((may_alias)) mas16;
typedef u16 __attribute__((may_alias)) mau16;
typedef u32 __attribute__((may_alias)) mau32;
typedef f32 __attribute__((may_alias)) maf32;

extern f32 D_8061ACA0[]; /* quarter-wave sine table, 0x204 entries (data in global .text) */
extern void *D_80746E20;
extern u8 D_80746E24[];
extern u8 D_80747040[];
extern u8 D_8074725C[];
extern u8 D_80747478[];
extern f32 D_80757490; /* 0x38000001: just above 2^-15 */

#define LB(p, o) (*(const s8 *)((const u8 *)(p) + (o)))
#define LBU(p, o) (*(const u8 *)((const u8 *)(p) + (o)))
#define LH(p, o) ((s32)(*(const mas16 *)((const u8 *)(p) + (o))))
#define LHU(p, o) ((u32)(*(const mau16 *)((const u8 *)(p) + (o))))
#define LW(p, o) (*(const mau32 *)((const u8 *)(p) + (o)))
#define LP(p, o) ((u8 *)LW(p, o)) /* game pointers are 32-bit host addresses */
#define SH(p, o, v) (*(mau16 *)((u8 *)(p) + (o)) = (u16)(v))
#define SW(p, o, v) (*(mau32 *)((u8 *)(p) + (o)) = (u32)(v))
#define F(p, o) (*(maf32 *)((u8 *)(p) + (o)))

/* mult + mflo: low 32 bits of the signed 64-bit product. */
static inline u32 mulLo(u32 a, u32 b) {
    return (u32)((s64)(s32)a * (s64)(s32)b);
}

/* cvt.w.s under the default FCSR rounding mode (nearest, ties to even). */
static inline u32 cvtws(f32 x) {
    return (u32)(s32)lrintf(x);
}

/* ---------------- keyframe bitstream (two parallel streams: frame and next frame) ---------------- */

typedef struct {
    const u8 *t4; /* frame stream byte pointer */
    const u8 *s4; /* next-frame stream byte pointer */
    u64 s3;       /* frame window, consumed bits shifted out at the top */
    u64 s7;       /* next-frame window */
    u32 s2;       /* bits consumed in the current windows */
} BitStreams;

/* Takes the top n bits of both windows (dsrlv by 64-n), then shifts them out (dsllv by n). */
static inline void bitsTake(BitStreams *b, u32 n, u32 *s5, u32 *t8) {
    u32 sh = (64 - n) & 63;
    *s5 = (u32)(b->s3 >> sh);
    *t8 = (u32)(b->s7 >> sh);
    b->s3 <<= n & 63;
    b->s7 <<= n & 63;
}

/* Accounts n more bits; reloads both 8-byte windows at the current byte when they would overflow. */
static inline void bitsReserve(BitStreams *b, u32 n) {
    u32 adv;

    b->s2 += n;
    if ((s32)b->s2 >= 0x41) {
        b->s2 -= n;
        adv = b->s2 >> 3;
        b->s4 += adv;
        b->t4 += adv;
        b->s3 = portReadBE64(b->t4);
        b->s7 = portReadBE64(b->s4);
        b->s2 &= 7;
        b->s3 <<= b->s2;
        b->s7 <<= b->s2;
        b->s2 += n;
    }
}

/* Rotation channel delta: 11-bit signed frame difference interpolated by t9/1024, result << 5. */
static inline u32 bitsRotation(BitStreams *b, u32 n, u32 t9) {
    u32 s5, t8;

    bitsReserve(b, n);
    bitsTake(b, n, &s5, &t8);
    t8 = t8 - s5;
    t8 = (u32)((s32)(t8 << 21) >> 21);
    t8 = (u32)((s32)mulLo(t8, t9) >> 10);
    return (s5 + t8) << 5;
}

/* Scale channel delta: (value << 4) + ((next - value) * t9 >> 6), logical shift as in the asm. */
static inline u32 bitsScale(BitStreams *b, u32 n, u32 t9) {
    u32 s5, t8;

    bitsReserve(b, n);
    bitsTake(b, n, &s5, &t8);
    t8 = t8 - s5;
    t8 = mulLo(t8, t9) >> 6;
    return (s5 << 4) + t8;
}

/*
 * func_80619C2C: decodes one animation slot into rotation array t5 and scale array t1 and returns the
 * root translation (asm outputs $fs1, $fs1f, $fs2). useNext ($t9 on entry) selects the next frame for
 * both streams. A null animation (a2 == 0) zeroes both arrays for every bone and returns 0,0,0.
 */
static void decodePose(u8 *a1, u8 *a2, u8 *t1, u8 *t5, u32 useNext, f32 out[3]) {
    BitStreams b;
    u32 t0, t6, t7, t9, s0, s1, s5, t8, k;
    const u8 *a3;

    if (a2 == NULL) {
        u8 *actor = (u8 *)D_80746E20;
        u8 *end = t5 + LBU(LP(actor, 0), 0x20) * 6;
        do {
            SH(t5, 0, 0);
            t5 += 2;
            SH(t1, 0, 0);
            t1 += 2;
        } while (t5 != end);
        out[0] = 0.0f;
        out[2] = out[0];
        out[1] = out[0];
        return;
    }

    t0 = LBU(a2, 0x13);
    a3 = a2 + LH(a2, 6);
    b.t4 = a3 + mulLo((u32)LH(a1, 0xC), t0);
    b.s4 = a3 + mulLo((u32)LH(a1, 0xE), t0);
    if (useNext != 0) {
        b.t4 = b.s4;
    }
    t9 = 0;
    if ((LH(a2, 4) & 0x20) == 0) {
        t9 = cvtws(F(a1, 8) * 1024.0f);
    }
    t6 = (LBU(a2, 0x11) - 1) * 3;
    b.s3 = portReadBE64(b.t4);
    b.s7 = portReadBE64(b.s4);
    b.s2 = 0;

    /* root translation: base << 10 plus the interpolated packed delta, as fixed point 22.10 */
    for (k = 0; k < 3; k++) {
        t7 = (u32)LH(a2, 8 + 2 * k) << 10;
        s0 = LBU(a2, 0xE + k);
        if (s0 != 0) {
            bitsTake(&b, s0, &s5, &t8);
            t8 = t8 - s5;
            t8 = mulLo(t8, t9);
            s5 = s5 << 10;
            b.s2 += s0;
            s5 = s5 + t8;
            t7 = t7 + s5;
        }
        out[k] = (f32)(s32)t7;
    }

    /* channels: descriptor bits 4..15 = rotation base (bit 4 = has scale), bits 0..3 = delta width */
    {
        const u8 *t3 = a2 + 0x14;
        do {
            s0 = LHU(t3, 0);
            s1 = s0 & 0xF;
            s0 &= 0xFFF0;
            if (s1 != 0) {
                s0 = s0 + bitsRotation(&b, s1, t9);
            }
            SH(t5, 0, s0);
            if ((s0 & 0x10) == 0) {
                t6--;
                SH(t1, 0, 0);
                t1 += 2;
                t5 += 2;
                t3 += 2;
                continue;
            }
            s0 = LHU(t3, 2);
            t3 += 4;
            t5 += 2;
            s1 = 0x10;
            if (s0 != 0) {
                s1 = s0 & 0xF;
                s0 ^= s1;
            }
            if (s1 != 0) {
                s0 = s0 + bitsScale(&b, s1, t9);
            }
            t6--;
            SH(t1, 0, s0);
            t1 += 2;
        } while (t6 != 0);
    }
}

/*
 * func_80619F50: decodes a second animation slot and blends it into the rotation (t5) and scale (t1)
 * arrays with weight t0 / 256. Root translation bits are skipped. Bones whose three descriptors have
 * no delta width and no scale flag are left untouched.
 */
static void blendPose(u8 *a1, u8 *a2, u8 *t1, u8 *t5, u32 t0) {
    BitStreams b;
    u32 t6, t7, t9, s0, s1, s5;
    const u8 *a3;
    const u8 *t3;

    t7 = LBU(a2, 0x13);
    a3 = a2 + LH(a2, 6);
    b.t4 = a3 + mulLo((u32)LH(a1, 0xC), t7);
    t9 = cvtws(F(a1, 8) * 1024.0f);
    b.s4 = a3 + mulLo((u32)LH(a1, 0xE), t7);
    t6 = LBU(a2, 0x11) - 1;
    s0 = LBU(a2, 0xF) + LBU(a2, 0xE) + LBU(a2, 0x10);
    b.s2 = s0;
    b.s3 = portReadBE64(b.t4) << (s0 & 63);
    b.s7 = portReadBE64(b.s4) << (s0 & 63);
    t3 = a2 + 0x14;

    do {
        if (((LHU(t3, 0) | LHU(t3, 2) | LHU(t3, 4)) & 0x1F) == 0) {
            t3 += 6;
            t5 += 6;
            t1 += 6;
        } else {
            t7 = 3;
            do {
                s0 = LHU(t3, 0);
                s1 = s0 & 0xF;
                s0 &= 0xFFF0;
                if (s1 != 0) {
                    s0 = s0 + bitsRotation(&b, s1, t9);
                }
                s1 = (u32)LH(t5, 0);
                s0 = (u32)(s32)(s16)s0;
                s5 = s0 - s1;
                s5 = mulLo(s5, t0);
                s0 &= 0x10;
                s5 = (u32)((s32)s5 >> 8) + s1;
                SH(t5, 0, s5);
                if (s0 != 0) {
                    s0 = LHU(t3, 2);
                    t3 += 2;
                    s1 = 0x10;
                    if (s0 != 0) {
                        s1 = s0 & 0xF;
                        s0 ^= s1;
                    }
                    if (s1 != 0) {
                        s0 = s0 + bitsScale(&b, s1, t9);
                    }
                }
                s1 = (u32)LH(t1, 0);
                s0 = (u32)(s32)(s16)s0;
                s5 = s0 - s1;
                s5 = mulLo(s5, t0);
                t7--;
                t3 += 2;
                t5 += 2;
                t1 += 2;
                s5 = (u32)((s32)s5 >> 8) + s1;
                SH(t1, -2, s5);
            } while (t7 != 0);
        }
        t6--;
    } while (t6 != 0);
}

/*
 * func_8061A1A0: applies the anim state's offset list (anim+0x1C; 8-byte records: s8 weight, u8 bone,
 * s16 x/y/z) to the rotation array t0. Positive weight adds weight*v/128; negative weight lerps toward
 * v by |weight|/128. A zero weight ends the list.
 */
static void applyPoseOffsets(const u8 *a0, u8 *t0) {
    const u8 *t4 = LP(a0, 0x1C);
    u32 t6, s0, s1, k;
    u8 *t7;

    if (t4 == NULL) {
        return;
    }
    for (;;) {
        t6 = (u32)(s32)LB(t4, 0);
        if (t6 == 0) {
            return;
        }
        t7 = t0 + LBU(t4, 1) * 6;
        if ((s32)t6 >= 0) {
            for (k = 0; k < 3; k++) {
                s0 = (u32)LH(t4, 2 + 2 * k);
                s1 = (u32)LH(t7, 2 * k);
                s0 = (u32)((s32)mulLo(t6, s0) >> 7) + s1;
                SH(t7, 2 * k, s0);
            }
        } else {
            t6 = 0 - t6;
            for (k = 0; k < 3; k++) {
                s0 = (u32)LH(t4, 2 + 2 * k);
                s1 = (u32)LH(t7, 2 * k);
                s0 = s0 - s1;
                s0 = (u32)((s32)mulLo(s0, t6) >> 7) + s1;
                SH(t7, 2 * k, s0);
            }
        }
        t4 += 8;
    }
}

/* ---------------- sine table lookups ---------------- */

/*
 * Half-angle lookup of func_80619B44: s16 angle (0x10000 per turn) -> sin(a/2) in *odd and cos(a/2)
 * in *even (the asm's $fXXf / $fXX pair). Negative angles negate the cosine slot via fs5 - x (fs5 = 0).
 */
static inline void halfSinCos(s32 angle, f32 fs5, f32 *odd, f32 *even) {
    u32 off = (((u32)angle >> 6) << 2) & 0x7FC;
    u32 off2 = 0x800 - off;

    if (angle >= 0) {
        *odd = D_8061ACA0[off >> 2];
        *even = D_8061ACA0[off2 >> 2];
    } else {
        *even = D_8061ACA0[off >> 2];
        *odd = D_8061ACA0[off2 >> 2];
        *even = fs5 - *even;
    }
}

/*
 * Full-angle lookup of the direct matrix path (1A320..1A4BC): u16 channel value >> 5 = 11-bit angle;
 * sin -> *odd, cos -> *even, quadrant from bits 9..10.
 */
static inline void fullSinCos(u32 v, f32 fs5, f32 *odd, f32 *even) {
    u32 t2 = v >> 5;
    u32 q = t2 & 0x600;
    u32 off = (t2 << 2) & 0x7FC;
    u32 off2 = 0x800 - off;

    if (q == 0x600) {
        *even = D_8061ACA0[off >> 2];
        *odd = D_8061ACA0[off2 >> 2];
        *odd = fs5 - *odd;
    } else if (q == 0x400) {
        *odd = D_8061ACA0[off >> 2];
        *even = D_8061ACA0[off2 >> 2];
        *odd = fs5 - *odd;
        *even = fs5 - *even;
    } else if (q == 0x200) {
        *even = D_8061ACA0[off >> 2];
        *odd = D_8061ACA0[off2 >> 2];
        *even = fs5 - *even;
    } else {
        *odd = D_8061ACA0[off >> 2];
        *even = D_8061ACA0[off2 >> 2];
    }
}

typedef struct {
    f32 ft1, ft1f, ft2, ft2f, ft5, ft5f, ft3, ft3f, fa0, fa0f;
} QuatTerms;

/*
 * func_80619B44: products of the half-angle sines/cosines of the three s16 rotation channels at p
 * (the terms of an Euler -> quaternion conversion).
 */
static void halfAngleQuatTerms(const u8 *p, f32 fs5, QuatTerms *o) {
    f32 fv0, fv0f, fv1, fv1f;

    halfSinCos(LH(p, 0), fs5, &fv0f, &fv0);
    halfSinCos(LH(p, 2), fs5, &fv1f, &fv1);
    o->ft1 = fv0 * fv1;
    o->ft1f = fv0 * fv1f;
    o->ft2 = fv0f * fv1;
    o->ft2f = fv0f * fv1f;
    halfSinCos(LH(p, 4), fs5, &o->ft5f, &o->ft5);
    o->ft3 = o->ft1 * o->ft5;
    o->ft3f = o->ft2f * o->ft5f;
    o->fa0 = o->ft2 * o->ft5;
    o->fa0f = o->ft1f * o->ft5f;
}

/* Matrix element store helpers for the bone matrix being written. */
#define SF(p, o, v) (F(p, o) = (v))

void actorBuildBoneMatrices(void *actor) {
    u8 *a0, *a1, *a2, *a3, *s0p, *t0p, *t2p;
    u32 t5, s1, t6, t7, i, n;
    f32 fv0, fv0f, fv1, fv1f, ft0, ft0f, ft1, ft1f, ft2, ft2f, ft3, ft3f;
    f32 fa0, fa0f, fa1, fa1f, ft4, ft4f, ft5, ft5f;
    f32 fs0, fs0f, fs1, fs1f, fs2, fs2f, fs3, fs3f, fs4, fs4f, fs5, fs5f;
    f32 tr[3];

    D_80746E20 = actor;
    a0 = LP(actor, 4);
    fs5 = 0.0f;
    a1 = LP(a0, 0);
    a2 = LP(a1, 0);
    decodePose(a1, a2, D_8074725C, D_80746E24, 0, tr);
    fs1 = tr[0];
    fs1f = tr[1];
    fs2 = tr[2];
    a1 = LP(a0, 4);
    a2 = LP(a1, 0);
    if (a2 != NULL && LHU(a0, 0x28) != 0) {
        blendPose(a1, a2, D_8074725C, D_80746E24, LHU(a0, 0x28));
    }
    applyPoseOffsets(a0, D_80746E24);
    a1 = LP(a0, 8);
    a2 = LP(a1, 0);
    if (a2 == NULL) {
        a1 = LP(a0, 0);
        a2 = LP(a1, 0);
        if (a2 != NULL) {
            if ((LH(a2, 4) & 0x20) == 0) {
                goto direct;
            }
            /* uninterpolated animation: blend frame and next frame by the frame fraction */
            a1 = LP(a0, 0);
            ft3 = fs1;
            ft3f = fs1f;
            fa0 = fs2;
            decodePose(a1, LP(a1, 0), D_80747478, D_80747040, 1, tr);
            fs1 = tr[0];
            fs1f = tr[1];
            fs2 = tr[2];
            a1 = LP(a0, 4);
            a2 = LP(a1, 0);
            if (a2 != NULL && LHU(a0, 0x28) != 0) {
                blendPose(a1, a2, D_80747478, D_80747040, LHU(a0, 0x28));
            }
            applyPoseOffsets(a0, D_80747040);
            a1 = LP(a0, 0);
            fs4 = F(a1, 8);
            goto blend;
        }
    }
    /* second layer (slot +8; slot +0 decoded as a null pose when slot +8 is empty) */
    ft3 = fs1;
    ft3f = fs1f;
    fa0 = fs2;
    decodePose(a1, a2, D_80747478, D_80747040, 0, tr);
    fs1 = tr[0];
    fs1f = tr[1];
    fs2 = tr[2];
    a1 = LP(a0, 0xC);
    a2 = LP(a1, 0);
    if (a2 != NULL && LHU(a0, 0x2A) != 0) {
        blendPose(a1, a2, D_80747478, D_80747040, LHU(a0, 0x2A));
    }
    applyPoseOffsets(a0, D_80747040);
    fs4 = F(a0, 0x10);

blend: /* func_806197B8 */
    {
        u8 *a2a = (u8 *)D_80746E20;
        QuatTerms o;
        u32 t9off;

        fs5f = 1.0f;
        fa0f = ft3 - fs1;
        fs4f = 2.0f;
        fs3f = fs5f - fs4;
        a1 = LP(a2a, 0);
        fa0f = fa0f * fs3f;
        fa1 = ft3f - fs1f;
        fa1 = fa1 * fs3f;
        fa1f = fa0 - fs2;
        n = LBU(a1, 0x20);
        fs1 = fa0f + fs1;
        fa1f = fa1f * fs3f;
        fa0f = 1024.0f;
        fs1f = fa1 + fs1f;
        fa0f = fa0f * fs4;
        fs2 = fa1f + fs2;
        t6 = cvtws(fa0f);
        a3 = LP(a2a, 8);
        t0p = LP(a1, 8);

        /* blend scale arrays: B (D_80747478) into A (D_8074725C); 0 means 0x8000 unless both are 0 */
        for (i = 0; i < n * 3; i++) {
            u32 t2 = LHU(D_80747478, 2 * i);
            t7 = LHU(D_8074725C, 2 * i);
            if (t2 == 0) {
                if (t7 == 0) {
                    continue;
                }
                t2 = 0x8000;
            } else if (t7 == 0) {
                t7 = 0x8000;
            }
            t2 = t2 - t7;
            t2 = (u32)((s32)mulLo(t2, t6) >> 10);
            t2 = t2 + t7;
            SH(D_8074725C, 2 * i, t2);
        }

        /* per bone: Euler -> quaternion for both poses, normalized lerp, quaternion -> matrix */
        t5 = n;
        do {
            u8 *t7p;
            u32 s0w, s3w;

            s1 = LBU(t0p, 2) * 6;
            t9off = s1;
            halfAngleQuatTerms(D_80746E24 + s1, fs5, &o);
            fa1 = o.ft1f * o.ft5;
            fs0 = o.ft3 + o.ft3f;
            fa1f = o.ft2 * o.ft5f;
            fs0f = o.fa0 - o.fa0f;
            ft4 = o.ft1 * o.ft5f;
            fs2f = fa1 + fa1f;
            ft4f = o.ft2f * o.ft5;
            fs3 = ft4 - ft4f;
            halfAngleQuatTerms(D_80747040 + s1, fs5, &o);
            fa1 = o.ft1f * o.ft5;
            ft0 = o.ft3 + o.ft3f;
            fa1f = o.ft2 * o.ft5f;
            ft0f = o.fa0 - o.fa0f;
            ft4 = o.ft1 * o.ft5f;
            ft1 = fa1 + fa1f;
            ft4f = o.ft2f * o.ft5;
            ft1f = ft4 - ft4f;
            ft3 = fs0 * ft0;
            ft3f = fs0f * ft0f;
            fa0 = fs2f * ft1;
            ft3 = ft3 + ft3f;
            fa0f = fs3 * ft1f;
            ft3 = ft3 + fa0;
            fs0 = fs0 * fs3f;
            ft3 = ft3 + fa0f;
            fs0f = fs0f * fs3f;
            if (ft3 < fs5) {
                ft0 = fs5 - ft0;
                ft0f = fs5 - ft0f;
                ft1 = fs5 - ft1;
                ft1f = fs5 - ft1f;
            }
            ft0 = ft0 * fs4;
            ft0f = ft0f * fs4;
            ft3 = fs0 + ft0;
            fs2f = fs2f * fs3f;
            ft3f = fs0f + ft0f;
            ft1 = ft1 * fs4;
            fs3 = fs3 * fs3f;
            fa0 = fs2f + ft1;
            ft1f = ft1f * fs4;
            fs4f = 2.0f;
            fv0 = ft3 * ft3;
            fa0f = fs3 + ft1f;
            fv0f = ft3f * ft3f;
            fv1 = fa0 * fa0;
            fv0 = fv0 + fv0f;
            fv0f = fa0f * fa0f;
            fv0 = fv0 + fv1;
            fv0 = fv0 + fv0f;
            fs4f = fs4f / fv0;
            fv0f = fa0 * fs4f;
            t7p = a3 + (LBU(t0p, 1) << 6);
            fv1 = fa0f * fs4f;
            t6 = LW(t0p, 4);
            ft0 = ft3 * fv0f;
            s0w = LW(t0p, 8);
            ft2 = ft3f * fv1;
            s3w = LW(t0p, 0xC);
            ft1f = ft3f * fv0f;
            fs0f = ft2 - ft0;
            ft0f = ft3 * fv1;
            fs2f = ft2 + ft0;
            fa1f = fa0 * fv0f;
            fs0 = ft1f + ft0f;
            ft4f = fa0f * fv1;
            fv0f = 1.0f / 32768.0f; /* 0x38000000 */
            ft4 = fa0 * fv1;
            ft5f = fa1f + ft4f;
            fv0 = ft3f * fs4f;
            ft5f = fs5f - ft5f;
            fv1f = ft3 * fv0;
            fv1 = ft1f - ft0f;
            ft1 = ft3f * fv0;
            fs3 = ft4 - fv1f;
            ft3 = ft1 + fa1f;
            ft3 = fs5f - ft3;
            ft0 = ft4 + fv1f;
            fv1f = ft1 + ft4f;
            fv1f = fs5f - fv1f;

            /* rows, each scaled by scale/32768 when the blended scale channel is nonzero */
            {
                u8 *t9p = D_8074725C + t9off;
                u32 t2 = LHU(t9p, 0);
                if (t2 != 0) {
                    fv0 = (f32)(s32)t2;
                    fv0 = fv0 * fv0f;
                    ft5f = ft5f * fv0;
                    fs0 = fs0 * fv0;
                    fs0f = fs0f * fv0;
                }
                SF(t7p, 0x0, ft5f);
                SF(t7p, 0x4, fs0);
                SF(t7p, 0x8, fs0f);
                t2 = LHU(t9p, 2);
                if (t2 != 0) {
                    fv0 = (f32)(s32)t2;
                    fv0 = fv0 * fv0f;
                    fv1 = fv1 * fv0;
                    fv1f = fv1f * fv0;
                    ft0 = ft0 * fv0;
                }
                SF(t7p, 0x10, fv1);
                SF(t7p, 0x14, fv1f);
                SF(t7p, 0x18, ft0);
                t2 = LHU(t9p, 4);
                if (t2 != 0) {
                    fv0 = (f32)(s32)t2;
                    fv0 = fv0 * fv0f;
                    fs2f = fs2f * fv0;
                    fs3 = fs3 * fv0;
                    ft3 = ft3 * fv0;
                }
                SF(t7p, 0x20, fs2f);
                SF(t7p, 0x24, fs3);
                SF(t7p, 0x28, ft3);
            }
            /* func_80619A88 */
            t0p += 0x10;
            SW(t7p, 0x30, t6);
            t5--;
            SW(t7p, 0x34, s0w);
            SW(t7p, 0x38, s3w);
        } while (t5 != 0);
    }
    goto adjust;

direct: /* L8061A2A8: single layer, Euler angles straight to a matrix */
    {
        u8 *actorp = (u8 *)D_80746E20;
        u8 *s5end;
        u8 *mtx;
        u8 *t1p;
        u8 *s7p;
        u32 sc;
        u32 w4, w8, wC;

        a1 = LP(actorp, 0);
        a3 = LP(actorp, 8);
        t0p = LP(a1, 8);
        s5end = t0p + (LBU(a1, 0x20) << 4);
        fs0f = 1.0f / 32768.0f; /* 0x38000000 */
        do {
            mtx = a3 + (LBU(t0p, 1) << 6);
            t1p = D_80746E24 + LBU(t0p, 2) * 6;
            s7p = D_8074725C + LBU(t0p, 2) * 6;
            fullSinCos(LHU(t1p, 0), fs5, &fv0f, &fv0);
            fullSinCos(LHU(t1p, 4), fs5, &ft0f, &ft0);
            ft1 = fv0f * ft0f;
            ft1f = fv0 * ft0f;
            ft2 = fv0f * ft0;
            w4 = LW(t0p, 4);
            w8 = LW(t0p, 8);
            SW(mtx, 0x30, w4);
            ft2f = fv0 * ft0;
            wC = LW(t0p, 0xC);
            fullSinCos(LHU(t1p, 2), fs5, &fv1f, &fv1);
            fa0 = fv1 * ft0;
            SW(mtx, 0x38, wC);
            fa0f = fv1 * ft0f;
            SW(mtx, 0x34, w8);

            fa1 = fs5 - fv1f;
            sc = LHU(s7p, 0);
            if (sc != 0) {
                ft3 = (f32)(s32)sc;
                ft3 = ft3 * fs0f;
                fa0 = fa0 * ft3;
                fa0f = fa0f * ft3;
                fa1 = fa1 * ft3;
            }
            SF(mtx, 0x0, fa0);
            SF(mtx, 0x4, fa0f);
            SF(mtx, 0x8, fa1);

            fa1f = ft2 * fv1f;
            fa1f = fa1f - ft1f;
            ft4 = ft1 * fv1f;
            ft4 = ft4 + ft2f;
            ft4f = fv0f * fv1;
            sc = LHU(s7p, 2);
            if (sc != 0) {
                ft3 = (f32)(s32)sc;
                ft3 = ft3 * fs0f;
                fa1f = fa1f * ft3;
                ft4 = ft4 * ft3;
                ft4f = ft4f * ft3;
            }
            SF(mtx, 0x10, fa1f);
            SF(mtx, 0x14, ft4);
            SF(mtx, 0x18, ft4f);

            ft5 = ft2f * fv1f;
            ft5 = ft5 + ft1;
            ft5f = ft1f * fv1f;
            ft5f = ft5f - ft2;
            fs0 = fv0 * fv1;
            sc = LHU(s7p, 4);
            if (sc != 0) {
                ft3 = (f32)(s32)sc;
                ft3 = ft3 * fs0f;
                ft5 = ft5 * ft3;
                ft5f = ft5f * ft3;
                fs0 = fs0 * ft3;
            }
            SF(mtx, 0x20, ft5);
            SF(mtx, 0x24, ft5f);
            SF(mtx, 0x28, fs0);
            t0p += 0x10;
        } while (t0p != s5end);
    }

adjust: /* func_8061A61C: per-bone scale list (anim+0x20), 16-byte records */
    {
        u8 *actorp = (u8 *)D_80746E20;
        u8 *list = LP(LP(actorp, 4), 0x20);
        u8 *m;
        u32 t2;

        if (list != NULL) {
            s0p = LP(actorp, 8);
            for (;;) { /* func_8061A638 */
                t2 = LBU(list, 0);
                if (t2 == 0xFF) {
                    break;
                }
                fv0 = F(list, 4);
                fv0f = F(list, 8);
                fv1 = F(list, 0xC);
                m = s0p + (t2 << 6);
                F(m, 0x0) = F(m, 0x0) * fv0;
                F(m, 0x4) = F(m, 0x4) * fv0;
                F(m, 0x8) = F(m, 0x8) * fv0;
                F(m, 0x10) = F(m, 0x10) * fv0f;
                F(m, 0x14) = F(m, 0x14) * fv0f;
                F(m, 0x18) = F(m, 0x18) * fv0f;
                F(m, 0x20) = F(m, 0x20) * fv1;
                F(m, 0x24) = F(m, 0x24) * fv1;
                F(m, 0x28) = F(m, 0x28) * fv1;
                t2 = LBU(list, 1);
                if (t2 != 0xFF) {
                    /* second bone gets the inverse scale */
                    fv1f = 1.0f;
                    fv0 = fv1f / fv0;
                    fv0f = fv1f / fv0f;
                    fv1 = fv1f / fv1;
                    m = s0p + (t2 << 6);
                    F(m, 0x0) = F(m, 0x0) * fv0;
                    F(m, 0x4) = F(m, 0x4) * fv0;
                    F(m, 0x8) = F(m, 0x8) * fv0;
                    F(m, 0x10) = F(m, 0x10) * fv0f;
                    F(m, 0x14) = F(m, 0x14) * fv0f;
                    F(m, 0x18) = F(m, 0x18) * fv0f;
                    F(m, 0x20) = F(m, 0x20) * fv1;
                    F(m, 0x24) = F(m, 0x24) * fv1;
                    F(m, 0x28) = F(m, 0x28) * fv1;
                }
                list += 0x10;
            }
        }

        /* L8061A758 / func_8061A774: per-bone translation list (anim+0x24) */
        list = LP(LP(actorp, 4), 0x24);
        if (list != NULL) {
            s0p = LP(actorp, 8);
            for (;;) {
                t2 = LBU(list, 0);
                if (t2 == 0xFF) {
                    break;
                }
                fv0 = F(list, 4);
                fv0f = F(list, 8);
                fv1 = F(list, 0xC);
                m = s0p + (t2 << 6);
                F(m, 0x30) = F(m, 0x30) + fv0;
                F(m, 0x34) = F(m, 0x34) + fv0f;
                F(m, 0x38) = F(m, 0x38) + fv1;
                t2 = LBU(list, 1);
                if (t2 != 0xFF) {
                    m = s0p + (t2 << 6);
                    F(m, 0x30) = F(m, 0x30) - fv0;
                    F(m, 0x34) = F(m, 0x34) - fv0f;
                    F(m, 0x38) = F(m, 0x38) - fv1;
                }
                list += 0x10;
            }
        }
    }

    /* L8061A7F0: root = world matrix with the scaled root translation added */
    a0 = (u8 *)D_80746E20;
    a1 = a0 + 0xC;
    a2 = LP(a0, 0);
    fv0 = F(a1, 0x0);
    s0p = LP(a0, 8);
    fv0f = F(a1, 0x4);
    t2p = LP(a2, 8);
    fv1 = F(a1, 0x8);
    t5 = LBU(a2, 0x20);
    fv1f = F(a1, 0x10);
    fa0 = D_80757490;
    ft0 = F(a1, 0x14);
    fs1 = fs1 * fa0;
    ft0f = F(a1, 0x18);
    ft1 = F(a1, 0x20);
    fs1f = fs1f * fa0;
    ft1f = F(a1, 0x24);
    ft2 = F(a1, 0x28);
    fs2 = fs2 * fa0;
    ft2f = F(a1, 0x30);
    fa0 = fs1 * fv0;
    ft3 = F(a1, 0x34);
    fa1f = fs1f * fv1f;
    ft3f = F(a1, 0x38);
    fa0 = fa0 + fa1f;
    s1 = LBU(t2p, 1);
    fa1f = fs2 * ft1;
    a3 = s0p + (s1 << 6);
    fa0f = fs1 * fv0f;
    fa0 = fa0 + fa1f;
    fa1f = fs1f * ft0;
    ft2f = ft2f + fa0;
    fa0f = fa0f + fa1f;
    fa1f = fs2 * ft1f;
    fs0f = F(a3, 0x0);
    fa1 = fs1 * fv1;
    fa0f = fa0f + fa1f;
    fa1f = fs1f * ft0f;
    fa1 = fa1 + fa1f;
    fa1f = fs2 * ft2;
    ft3 = ft3 + fa0f;
    fa1 = fa1 + fa1f;
    ft3f = ft3f + fa1;

    /* hierarchy: bone = bone * parent, in bone-table order (func_8061A938 / func_8061A8AC) */
    for (;;) {
        fs1 = F(a3, 0x4);
        fs2 = fs0f * fv0;
        fs5 = F(a3, 0x8);
        fs3 = fs0f * fv0f;
        fs4 = F(a3, 0x10);
        fs2f = fs1 * fv1f;
        fs4f = F(a3, 0x14);
        fs3f = fs1 * ft0;
        fa0 = fs2 + fs2f;
        fs2 = fs5 * ft1;
        fs2f = fs5 * ft1f;
        fa0 = fa0 + fs2;
        fs2 = fs0f * fv1;
        F(a3, 0x0) = fa0;
        fa0f = fs2f + fs3;
        fs2f = fs1 * ft0f;
        fa0f = fa0f + fs3f;
        fa1 = fs2 + fs2f;
        fs2f = fs5 * ft2;
        fa1 = fa1 + fs2f;
        F(a3, 0x4) = fa0f;
        fs2 = fs4 * fv0;
        fs5 = F(a3, 0x18);
        fs3 = fs4 * fv0f;
        fs0f = F(a3, 0x20);
        fs2f = fs4f * fv1f;
        fs1 = F(a3, 0x24);
        fs3f = fs4f * ft0;
        fa1f = fs2 + fs2f;
        fs2 = fs5 * ft1;
        F(a3, 0x8) = fa1;
        fs2f = fs5 * ft1f;
        fa1f = fa1f + fs2;
        fs2 = fs4 * fv1;
        F(a3, 0x10) = fa1f;
        ft4 = fs2f + fs3;
        fs2f = fs4f * ft0f;
        ft4 = ft4 + fs3f;
        ft4f = fs2 + fs2f;
        fs2f = fs5 * ft2;
        ft4f = ft4f + fs2f;
        F(a3, 0x14) = ft4;
        fs2 = fs0f * fv0;
        fs5 = F(a3, 0x28);
        fs3 = fs0f * fv0f;
        fs4 = F(a3, 0x30);
        fs2f = fs1 * fv1f;
        fs4f = F(a3, 0x34);
        fs3f = fs1 * ft0;
        ft5 = fs2 + fs2f;
        fs2 = fs5 * ft1;
        F(a3, 0x18) = ft4f;
        fs2f = fs5 * ft1f;
        ft5 = ft5 + fs2;
        fs2 = fs0f * fv1;
        ft5f = fs2f + fs3;
        fs2f = fs1 * ft0f;
        ft5f = ft5f + fs3f;
        F(a3, 0x20) = ft5;
        fs0 = fs2 + fs2f;
        fs2f = fs5 * ft2;
        fs0 = fs0 + fs2f;
        F(a3, 0x24) = ft5f;
        fs2 = fs4 * fv0;
        fs5 = F(a3, 0x38);
        fs3 = fs4 * fv0f;
        fs2 = fs2 + ft2f;
        fs2f = fs4f * fv1f;
        fs3 = fs3 + ft3;
        fs3f = fs4f * ft0;
        fs0f = fs2 + fs2f;
        fs1 = fs3 + fs3f;
        fs2 = fs5 * ft1;
        F(a3, 0x28) = fs0;
        fs2f = fs5 * ft1f;
        fs0f = fs0f + fs2;
        fs2 = fs4 * fv1;
        F(a3, 0x30) = fs0f;
        fs1 = fs1 + fs2f;
        fs2 = fs2 + ft3f;
        fs2f = fs4f * ft0f;
        F(a3, 0x34) = fs1;
        fs1f = fs2 + fs2f;
        fs2f = fs5 * ft2;
        fs1f = fs1f + fs2f;
        F(a3, 0x38) = fs1f;
        t5--;
        t2p += 0x10;
        if (t5 == 0) {
            break;
        }
        /* func_8061A8AC: next bone; parent is the previous bone (kept in registers) or reloaded */
        t6 = LBU(t2p, 1);
        a3 = s0p + (t6 << 6);
        t7 = LBU(t2p, 0);
        if (t7 == s1) {
            s1 = t6;
            fv0 = fa0;
            fv0f = fa0f;
            fv1 = fa1;
            fv1f = fa1f;
            ft0 = ft4;
            ft0f = ft4f;
            ft1 = ft5;
            ft1f = ft5f;
            ft2 = fs0;
            ft2f = fs0f;
            ft3 = fs1;
            fs0f = F(a3, 0x0);
            ft3f = fs1f;
        } else {
            s1 = t6;
            fs0f = F(a3, 0x0);
            a1 = s0p + (t7 << 6);
            fv0 = F(a1, 0x0);
            fv0f = F(a1, 0x4);
            fv1 = F(a1, 0x8);
            fv1f = F(a1, 0x10);
            ft0 = F(a1, 0x14);
            ft0f = F(a1, 0x18);
            ft1 = F(a1, 0x20);
            ft1f = F(a1, 0x24);
            ft2 = F(a1, 0x28);
            ft2f = F(a1, 0x30);
            ft3 = F(a1, 0x34);
            ft3f = F(a1, 0x38);
        }
    }

    /*
     * L8061AAB4: every bone matrix -> N64 Mtx in place (s15.16; integer halves in words 0..7, fraction
     * halves in words 8..15). The asm stores big-endian halfwords; Mtx words are native u32 on the port,
     * so each word is assembled with the same value the N64 word held. Column 3 is (0,0,0,1).
     */
    {
        u8 *m = LP(a0, 8);
        u32 cnt = LBU(a2, 0x20);

        do {
            u32 x[12];
            u32 r;

            for (r = 0; r < 4; r++) {
                x[r * 3 + 0] = cvtws(F(m, r * 0x10 + 0x0) * 65536.0f);
                x[r * 3 + 1] = cvtws(F(m, r * 0x10 + 0x4) * 65536.0f);
                x[r * 3 + 2] = cvtws(F(m, r * 0x10 + 0x8) * 65536.0f);
            }
            for (r = 0; r < 4; r++) {
                u32 c0 = x[r * 3 + 0];
                u32 c1 = x[r * 3 + 1];
                u32 c2 = x[r * 3 + 2];
                SW(m, r * 8 + 0x0, (c0 & 0xFFFF0000u) | (c1 >> 16));
                SW(m, r * 8 + 0x4, (c2 & 0xFFFF0000u) + (r == 3 ? 1u : 0u));
                SW(m, r * 8 + 0x20, (c0 << 16) | (c1 & 0xFFFFu));
                SW(m, r * 8 + 0x24, c2 << 16);
            }
            m += 0x40;
            cnt--;
        } while (cnt != 0);
    }
}
