/*
 * C transcription of decomp/asm/global/77B90.s (handwritten collision math:
 * 64-bit plane tests with dmult/ld/sd, sphere vs. triangle tests).
 *
 * The asm passes state between its internal routines in non-ABI registers:
 *   $t3,$t4,$t5  sphere centre (world units * 8), $t6 sphere radius
 *   $s0          current CollisionBox (or static tri before func_80673324)
 *   $fp          index of the current static triangle in its cell
 *   $s2          |plane distance| from func_8067457C (also clobbered by func_80674884)
 *   $v1,$a0,$a1  closest point on the plane from func_8067457C
 *   $t7          hit flag of func_8067470C / func_80674884 / func_80674A5C
 * These are modelled as file-static variables (sT3.., sS0, sFp, sS2, sV1..)
 * and return values. Entry points behave exactly as observed through their
 * ABI plus their memory writes.
 *
 * Box layout (CollisionBox, include/collision.h, size 0x58) is mirrored as
 * Box below; game-side prototypes take CollisionBox *, which is the same
 * wasm signature (i32) as the Box * used here.
 */
#include "port_internal.h"
#include <math.h>

typedef struct Box {
    /* 0x00 */ s64 nx;
    /* 0x08 */ s64 ny;
    /* 0x10 */ s64 nz;
    /* 0x18 */ s64 d;
    /* 0x20 */ f32 len;
    /* 0x24 */ s32 x0, y0, z0;
    /* 0x30 */ s32 x1, y1, z1;
    /* 0x3C */ s32 x2, y2, z2;
    /* 0x48 */ s16 unk48;
    /* 0x4A */ u8 axis;
    /* 0x4B */ u8 unk4B;
    /* 0x4C */ u8 unk4C;
    /* 0x4D */ u8 unk4D;
    /* 0x4E */ s8 unk4E;
    /* 0x4F */ u8 unk4F;
    /* 0x50 */ u8 unk50;
    /* 0x51 */ u8 unk51;
    /* 0x52 */ u8 pad52[6];
} Box;
_Static_assert(sizeof(Box) == 0x58, "CollisionBox size");

/* CollisionStaticTriOut (70FD0.c), stride 0x28 */
typedef struct StaticTri {
    s64 nx, ny, nz, d;
    f32 len;
    s16 minY;
    s16 maxY;
} StaticTri;
_Static_assert(sizeof(StaticTri) == 0x28, "CollisionStaticTriOut size");

/* CollisionStaticTriIn (70FD0.c), stride 0x18 */
typedef struct StaticTriIn {
    s16 x0, y0, z0, x1, y1, z1, x2, y2, z2;
    s16 normalYaw;
    u8 unk14;
    u8 unk15;
    u8 flags;
} StaticTriIn;
_Static_assert(sizeof(StaticTriIn) == 0x18, "CollisionStaticTriIn size");

/* BufferCacheEntry (70FD0.c), stride 0x18 */
typedef struct Cell {
    void *tris;
    void *rawTris;
    u32 lastUsedFrame;
    s32 romAddr;
    s32 size;
    s32 triCount;
} Cell;

/* game functions */
void collisionLoadStaticCell(s32);
void actorUpdateFacingAndOffset(void * /* CollisionBox * */, s32, s32, s32);
s32 actorCheckCameraBoundaryCondition(s8);

/* bss (asm/global/data/165D50_807F70B0.bss.s) */
extern Cell *sStaticCollisionCells;
extern s32 sStaticCollisionCellCount;
extern u8 gCollisionHit;
extern Box D_807FB550;
extern s32 D_807FB5A8, D_807FB5AC, D_807FB5B0;
extern u16 *D_807FB5B4, *D_807FB5B8;
extern Cell *D_807FB5BC;
extern StaticTri *D_807FB5C0;
extern s32 gCollisionPlanePointX, gCollisionPlanePointY, gCollisionPlanePointZ, gCollisionPlaneDist;
extern Box *gCollisionNearestBox, *gCollisionHitBox;
extern s32 gCollisionNearestTri;
extern s32 gStaticCollisionCellSizeX, gStaticCollisionCellSizeZ;
extern s16 gStaticCollisionCellsX;
extern s16 sHitBoxYawMin, sHitBoxYawMax, sHitBoxBackYawMin, sHitBoxBackYawMax;
extern s16 D_807FB5F4, gCollisionNearestTriIndex, sCollisionTriMinYMargin, sCollisionTriMaxYMargin;
extern u8 D_807FB5FC, gCollisionDeferredPush, D_807FB5FE, gCollisionDeferredHit;
extern u8 gCollisionResolvePass, sCollisionHitPart, sDamageSurfaceContact, D_807FB603;
extern u8 sWallProbeActive, sBoundaryTwoSided, gCollisionArcWidened, D_807FB607;
extern Box *D_807FB608;
extern s32 D_807FB60C, D_807FB610, D_807FB614, D_807FB618;
extern u8 gCollisionNearestFound, D_807FB61D;

/* register model */
static s32 sT3, sT4, sT5, sT6;
static Box *sS0;
static s32 sFp;
static s64 sS2;
static s64 sV1, sA0, sA1;

#define MUL64(a, b) ((s64)((u64)(s64)(a) * (u64)(s64)(b)))
#define ADD64(a, b) ((s64)((u64)(s64)(a) + (u64)(s64)(b)))
#define SUB32(a, b) ((s32)((u32)(a) - (u32)(b)))

/* func_80672E90: record the previous sphere position (D_807FB5A8/AC/B0). */
void func_80672E90(s32 x, s32 y, s32 z) {
    D_807FB5A8 = x;
    D_807FB5AC = y;
    D_807FB5B0 = z;
}

/* func_80672EBC + func_80672F94: pick the static collision cell for (x, z) and load it. */
static void func_80672EBC(s32 x, s32 z) {
    s32 cell;

    if (x > 0) {
        x = x / gStaticCollisionCellSizeX;
        if (z > 0) {
            z = z / gStaticCollisionCellSizeZ;
            cell = x + (s32)((u32)z * (u32)(s32)gStaticCollisionCellsX);
        } else {
            cell = 0;
        }
    } else {
        cell = 0;
    }
    D_807FB5F4 = (s16)cell;
    collisionLoadStaticCell(cell); /* func_80672F94: saves every register around the call */
}

/* func_80673240: static tri Y range [(minY-D5F8)*8, (maxY+D5FA)*8] strictly contains sphere Y. */
static s32 func_80673240(void) {
    StaticTri *t = (StaticTri *)sS0;
    s32 lo = (s32)((u32)SUB32(t->minY, sCollisionTriMinYMargin) << 3);
    s32 hi = (s32)((u32)((u32)(s32)t->maxY + (u32)(s32)sCollisionTriMaxYMargin) << 3);

    return lo < sT4 && sT4 < hi;
}

/* func_80673324: expand static tri sS0 (index sFp) into the scratch box D_807FB550; sS0 := &D_807FB550. */
static void func_80673324(void) {
    StaticTri *src = (StaticTri *)sS0;
    Box *b = &D_807FB550;
    StaticTriIn *in;
    s64 ax, ay, az;

    b->nx = src->nx;
    b->ny = src->ny;
    b->nz = src->nz;
    b->d = src->d;
    b->len = src->len;
    in = (StaticTriIn *)((u8 *)D_807FB5BC->rawTris + sFp * 24);
    b->x0 = (s32)((u32)(s32)in->x0 << 3);
    b->y0 = (s32)((u32)(s32)in->y0 << 3);
    b->z0 = (s32)((u32)(s32)in->z0 << 3);
    b->x1 = (s32)((u32)(s32)in->x1 << 3);
    b->y1 = (s32)((u32)(s32)in->y1 << 3);
    b->z1 = (s32)((u32)(s32)in->z1 << 3);
    b->x2 = (s32)((u32)(s32)in->x2 << 3);
    b->y2 = (s32)((u32)(s32)in->y2 << 3);
    b->z2 = (s32)((u32)(s32)in->z2 << 3);
    b->unk48 = in->normalYaw;
    b->unk4C = in->unk14;
    ax = b->nx < 0 ? (s64)(0 - (u64)b->nx) : b->nx;
    ay = b->ny < 0 ? (s64)(0 - (u64)b->ny) : b->ny;
    az = b->nz < 0 ? (s64)(0 - (u64)b->nz) : b->nz;
    if (!(az < ax) && !(az < ay)) {
        b->axis = 0;
    } else if (!(ay < ax) && !(ay < az)) {
        b->axis = 1;
    } else {
        b->axis = 2;
    }
    b->unk4D = 0;
    b->unk4F = 0;
    b->unk51 = (in->flags & 4) ? 1 : 0;
    b->unk4B = 1;
    b->unk50 = in->unk15;
    D_807FB5C0 = src;
    sS0 = b;
}

/*
 * func_8067457C: plane distance of the sphere centre to box sS0.
 * Sets sS2 = |round((n.c + d) / len)|; hit iff sS2 <= radius.
 * On hit sV1/sA0/sA1 = closest point on the plane; on miss the asm leaves
 * $v1 = ny, $a0 = nz and $a1 untouched (= a1In).
 */
static s32 func_8067457C(s64 a1In) {
    Box *b = sS0;
    s64 nx = b->nx, ny = b->ny, nz = b->nz, d = b->d;
    f32 len = b->len;
    s64 dot, s2, s3;
    f32 q, l2;

    dot = ADD64(ADD64(MUL64(nx, sT3), MUL64(ny, sT4)), MUL64(nz, sT5));
    s2 = ADD64(dot, d);
    s2 = llrintf((f32)s2 / len);
    if (s2 < 0) {
        s2 = (s64)(0 - (u64)s2);
    }
    sS2 = s2;
    if ((s64)sT6 < s2) {
        sV1 = ny;
        sA0 = nz;
        sA1 = a1In;
        return 0;
    }
    s3 = (s64)(0 - (u64)d);
    s3 = (s64)((u64)s3 - (u64)dot);
    l2 = len * len;
    q = (f32)s3 / l2;
    sV1 = ADD64(llrintf(q * (f32)nx), sT3);
    sA0 = ADD64(llrintf(q * (f32)ny), sT4);
    sA1 = ADD64(llrintf(q * (f32)nz), sT5);
    sS2 = s2;
    return 1;
}

/*
 * func_80674688 + func_8067470C: project the triangle and the closest point onto
 * the plane perpendicular to the dominant axis and test point-in-triangle.
 */
static s32 func_8067470C(void) {
    Box *b = sS0;
    s32 v0, v1, a0, a1, a2, a3, t0, t1, t4, t5, t2;
    f32 px, py, cx, cy, bx, by, e, f;

    /* func_80674688 */
    if (b->axis == 0) {
        t0 = (s32)sV1; t1 = (s32)sA0;
        v0 = b->x0; v1 = b->y0; a0 = b->x1; a1 = b->y1; a2 = b->x2; a3 = b->y2;
    } else if (b->axis == 1) {
        t0 = (s32)sV1; t1 = (s32)sA1;
        v0 = b->x0; v1 = b->z0; a0 = b->x1; a1 = b->z1; a2 = b->x2; a3 = b->z2;
    } else {
        t0 = (s32)sA0; t1 = (s32)sA1;
        v0 = b->y0; v1 = b->z0; a0 = b->y1; a1 = b->z1; a2 = b->y2; a3 = b->z2;
    }

    /* func_8067470C */
    cx = (f32)(s32)((u32)v0 + (u32)a0) / 2.0f;
    cy = (f32)(s32)((u32)v1 + (u32)a1) / 2.0f;
    px = (f32)t0;
    cx = (f32)a2 + cx;
    cx = cx / 2.0f;
    cy = (f32)a3 + cy;
    py = (f32)t1;
    cy = cy / 2.0f;
    for (t2 = 3; t2 != 0; t2--) {
        if (t2 == 3) {
            bx = (f32)v0; by = (f32)v1;
            t4 = SUB32(a1, v1); t5 = SUB32(a0, v0);
        } else if (t2 == 2) {
            bx = (f32)v0; by = (f32)v1;
            t4 = SUB32(a3, v1); t5 = SUB32(a2, v0);
        } else {
            bx = (f32)a0; by = (f32)a1;
            t4 = SUB32(a3, a1); t5 = SUB32(a2, a0);
        }
        e = (px - bx) * (f32)t4;
        e = e - (py - by) * (f32)t5;
        if (e == 0.0f) {
            continue;
        }
        f = (cx - bx) * (f32)t4;
        f = f - (cy - by) * (f32)t5;
        if (!(e > 0.0f)) {
            if (f < 0.0f) {
                continue;
            }
            return 0;
        }
        if (!(f > 0.0f)) {
            return 0;
        }
    }
    return 1;
}

/* func_80674884: does any triangle edge segment intersect the sphere. Clobbers $s2. */
static s32 func_80674884(void) {
    Box *b = sS0;
    s32 s2;

    for (s2 = 3; s2 != 0; s2--) {
        s32 px, py, pz, qx, qy, qz;
        s64 dx, dy, dz, fx, fy, fz, a, bb, c, disc;
        f32 r, nb, a2, t;

        if (s2 == 3) {
            px = b->x0; py = b->y0; pz = b->z0; qx = b->x1; qy = b->y1; qz = b->z1;
        } else if (s2 == 2) {
            px = b->x0; py = b->y0; pz = b->z0; qx = b->x2; qy = b->y2; qz = b->z2;
        } else {
            px = b->x1; py = b->y1; pz = b->z1; qx = b->x2; qy = b->y2; qz = b->z2;
        }
        fx = SUB32(px, sT3); dx = SUB32(qx, px);
        fy = SUB32(py, sT4); dy = SUB32(qy, py);
        fz = SUB32(pz, sT5); dz = SUB32(qz, pz);
        a = ADD64(ADD64(MUL64(dx, dx), MUL64(dy, dy)), MUL64(dz, dz));
        bb = ADD64(ADD64(MUL64(dx, fx), MUL64(dy, fy)), MUL64(dz, fz));
        bb = (s64)((u64)bb << 1);
        c = ADD64(ADD64(MUL64(fx, fx), MUL64(fy, fy)), MUL64(fz, fz));
        c = (s64)((u64)c - (u64)MUL64(sT6, sT6));
        disc = (s64)((u64)MUL64(bb, bb) - ((u64)MUL64(a, c) << 2));
        if (disc < 0) {
            continue;
        }
        r = (f32)disc;
        nb = (f32)(s64)(0 - (u64)bb);
        r = sqrtf(r);
        a2 = (f32)(s64)((u64)a << 1);
        t = (nb + r) / a2;
        if (!(t < 0.0f) && !(t > 1.0f)) {
            sS2 = s2;
            return 1;
        }
        t = (nb - r) / a2;
        if (t < 0.0f) {
            continue;
        }
        if (!(t > 1.0f)) {
            sS2 = s2;
            return 1;
        }
    }
    sS2 = 0;
    return 0;
}

/* func_80674A5C: is vertex 0 within the sphere radius (rounded distance). */
static s32 func_80674A5C(void) {
    Box *b = sS0;
    s64 dx = SUB32(b->x0, sT3);
    s64 dy = SUB32(b->y0, sT4);
    s64 dz = SUB32(b->z0, sT5);
    s64 sum = ADD64(ADD64(MUL64(dx, dx), MUL64(dy, dy)), MUL64(dz, dz));
    s64 dist = llrintf(sqrtf((f32)sum));

    return !((s64)sT6 < dist);
}

/* func_80674688..func_80674A5C chain used by every loop: returns $t7. */
static s32 sphereTouchesTri(void) {
    if (func_8067470C()) {
        return 1;
    }
    if (func_80674884()) {
        return 1;
    }
    return func_80674A5C();
}

/* func_8067443C: angular span between *D_807FB5B4 and *D_807FB5B8 (12-bit angles). */
static s32 func_8067443C(void) {
    s32 lo = *D_807FB5B4;
    s32 hi = *D_807FB5B8;

    if (hi < lo) {
        return hi + (0xFFF - lo);
    }
    hi = hi - lo;
    if (hi > 0x800) {
        hi -= 0x800;
    }
    return hi;
}

/* func_80674330: widen the arc [*D_807FB5B4, *D_807FB5B8] by [a0, a1]; flag gCollisionArcWidened if it grew. */
static void func_80674330(s32 a0, s32 a1) {
    s32 t0;
    u32 v0, v1, ua0, ua1;

    if (a0 < 0) {
        a0 += 0xFFF;
    }
    if (a0 >= 0x1000) {
        a0 -= 0xFFF;
    }
    if (a1 < 0) {
        a1 += 0xFFF;
    }
    if (a1 >= 0x1000) {
        a1 -= 0xFFF;
    }
    t0 = func_8067443C();
    v0 = *D_807FB5B4;
    v1 = *D_807FB5B8;
    if (v0 == 0 && v1 == 0xFFF) {
        *D_807FB5B4 = (u16)a0;
        *D_807FB5B8 = (u16)a1;
        return;
    }
    v1 <<= 20;
    ua1 = (u32)a1 << 20;
    v0 <<= 20;
    ua0 = (u32)a0 << 20;
    if (!((s32)(ua1 - v1) > 0)) {
        v1 = ua1;
    }
    if ((s32)(ua0 - v0) > 0) {
        v0 = ua0;
    }
    v0 >>= 20;
    v1 >>= 20;
    *D_807FB5B4 = (u16)v0;
    *D_807FB5B8 = (u16)v1;
    if (t0 < func_8067443C()) {
        gCollisionArcWidened = 1;
    }
}

/* func_80674080: track the nearest hit (D_807FB608..618) for the current sphere. */
static void func_80674080(s32 a1In) {
    func_8067457C(a1In);
    if (sS2 < (s64)D_807FB60C) {
        D_807FB608 = sS0;
        D_807FB60C = (s32)sS2;
        D_807FB610 = (s32)((u32)((u32)SUB32((s32)sV1, sT3) << 3) + (u32)(s32)sV1) >> 3;
        D_807FB614 = (s32)((u32)((u32)SUB32((s32)sA0, sT4) << 3) + (u32)(s32)sA0) >> 3;
        D_807FB618 = (s32)((u32)((u32)SUB32((s32)sA1, sT5) << 3) + (u32)(s32)sA1) >> 3;
    }
}

/* func_80674150: nearest hit from the previous position (radius 800); records gCollisionNearestBox/DC/F6. */
static void func_80674150(void) {
    s32 t3 = sT3, t4 = sT4, t5 = sT5, t6 = sT6;
    s64 v1 = sV1, a0 = sA0, a1 = sA1, s2 = sS2;
    Box *s0 = sS0;

    sT3 = D_807FB5A8;
    sT4 = D_807FB5AC;
    sT5 = D_807FB5B0;
    sT6 = 0x320;
    if (func_8067457C(a1) && sS2 < (s64)gCollisionPlaneDist) {
        gCollisionNearestBox = sS0;
        gCollisionPlaneDist = (s32)sS2;
        gCollisionNearestTri = (s32)(u32)D_807FB5C0;
        gCollisionNearestTriIndex = (s16)sFp;
        gCollisionNearestFound = 1;
    }
    sT3 = t3; sT4 = t4; sT5 = t5; sT6 = t6;
    sV1 = v1; sA0 = a0; sA1 = a1; sS2 = s2;
    sS0 = s0;
}

/* func_80673B78: box gate by unk4B (0 never, 1 always, else previous position >= 0x15 away). */
static s32 func_80673B78(void) {
    u8 k = sS0->unk4B;
    s32 t3, t4, t5, t6, hit;

    if (k == 0) {
        return 0;
    }
    if (k == 1) {
        return 1;
    }
    t3 = sT3; t4 = sT4; t5 = sT5; t6 = sT6;
    sT3 = D_807FB5A8;
    sT4 = D_807FB5AC;
    sT5 = D_807FB5B0;
    sT6 = 0x320;
    hit = func_8067457C(sA1);
    sT3 = t3; sT4 = t4; sT5 = t5; sT6 = t6;
    if (!hit) {
        return 0;
    }
    return !(sS2 < 0x15);
}

/* func_80673A40: is v1 in the (possibly wrapping) range [lo, hi]; else returns v0 unchanged. */
static s32 func_80673A40(s32 lo, s32 hi, s32 v1, s32 v0) {
    if (!(hi < lo)) {
        if (v1 < lo || hi < v1) {
            return v0;
        }
        return 1;
    }
    if (!(v1 < lo)) {
        return 1;
    }
    if (hi < v1) {
        return v0;
    }
    return 1;
}

/* func_806739D8: box yaw (unk48) inside [D5EC,D5EE] or [D5F0,D5F2]. */
static s32 func_806739D8(void) {
    s32 v0 = 0;
    s32 yaw = (u16)sS0->unk48;

    v0 = func_80673A40((u16)sHitBoxYawMin, (u16)sHitBoxYawMax, yaw, v0);
    if (v0 == 0) {
        v0 = func_80673A40((u16)sHitBoxBackYawMin, (u16)sHitBoxBackYawMax, yaw, v0);
    }
    return v0;
}

/* func_80673A94: unreferenced in the asm and the game; which side of the plane the previous position is on vs. unk4C. */
static __attribute__((unused)) s32 func_80673A94(void) {
    Box *b = sS0;
    s64 v = ADD64(ADD64(ADD64(MUL64(b->nx, D_807FB5A8), MUL64(b->ny, D_807FB5AC)), MUL64(b->nz, D_807FB5B0)), b->d);
    s32 front = (b->d > 0) ? (v > 0) : (v < 0);

    if (front) {
        return b->unk4C != 0;
    }
    return b->unk4C == 0;
}

/* func_80673D48: process one confirmed hit against box sS0. */
static void func_80673D48(void) {
    Box *b = sS0;
    s64 v;
    s32 yaw, a0, a1, front, tmp;

    if (sWallProbeActive != 0) {
        /* func_80673C34 */
        actorUpdateFacingAndOffset(b, D_807FB5A8, D_807FB5AC, D_807FB5B0);
        return;
    }
    if ((s8)b->unk50 != -1) {
        /* func_80673FDC */
        if (actorCheckCameraBoundaryCondition((s8)b->unk50) == 0) {
            return;
        }
    }
    if (b->unk4F != 0) {
        sCollisionHitPart = b->unk4F;
        return;
    }
    gCollisionHit = 1;
    if (b->unk51 != 0) {
        sDamageSurfaceContact |= 1;
        return;
    }
    if (D_807FB61D != 0) {
        D_807FB607 = 1;
    }
    v = ADD64(ADD64(ADD64(MUL64(b->nx, D_807FB5A8), MUL64(b->ny, D_807FB5AC)), MUL64(b->nz, D_807FB5B0)), b->d);
    yaw = (u16)b->unk48;
    front = (b->d > 0) ? (v > 0) : (v < 0);
    if (front) {
        a0 = yaw - 0x400;
        a1 = yaw + 0x400;
        if (b->unk4C != 0) {
            goto L_F50;
        }
    } else {
        a0 = yaw + 0x400;
        a1 = yaw - 0x400;
        if (b->unk4C == 0) {
            goto L_F50;
        }
    }
    /* .L80673EC4 */
    if (D_807FB61D != 0) {
        func_80674080(a1);
    }
    if (sBoundaryTwoSided != 0) {
        func_80674150();
    }
    if (D_807FB603 == 0 || D_807FB5FE != 1 || gCollisionDeferredPush == 0) {
        return;
    }
    tmp = a0;
    a0 = a1;
    a1 = tmp;
    if (gCollisionResolvePass != 1) {
        return;
    }
    goto L_FA4;

L_F50:
    if (D_807FB5FE == 1 && gCollisionDeferredPush != 0) {
        if (gCollisionResolvePass == 1) {
            goto L_FA4;
        }
        gCollisionHitBox = b;
        gCollisionDeferredHit = 1;
        return;
    }
    func_80674150();
L_FA4:
    D_807FB5FC = 1;
    func_80674330(a0, a1);
}

/* Shared loop of func_806730A4 / func_806734E4 over the tris of the sphere's static cell. */
static s32 staticCellLoop(s32 x, s32 y, s32 z, s32 r, s32 resolve) {
    s32 idx;
    Cell *cell;
    StaticTri *p, *end;

    sT3 = x;
    sT4 = y;
    sT5 = z;
    sT6 = r;
    func_80672EBC(x, z);
    idx = D_807FB5F4;
    if (!(idx < sStaticCollisionCellCount)) {
        return 0;
    }
    cell = (Cell *)((u8 *)sStaticCollisionCells + (u32)idx * 0x18);
    D_807FB5BC = cell;
    p = (StaticTri *)cell->tris;
    end = (StaticTri *)((u8 *)p + (u32)cell->triCount * 0x28);
    sFp = 0;
    while (p != end) {
        sS0 = (Box *)p;
        if (func_80673240() && func_8067457C(z /* $a1 unused on miss */)) {
            func_80673324();
            if (sphereTouchesTri()) {
                if (!resolve) {
                    return 1;
                }
                func_80673D48();
            }
            p = D_807FB5C0;
        }
        sFp++;
        p++;
    }
    return 0;
}

/* func_806730A4: does the sphere touch any static collision tri. */
u8 func_806730A4(s32 x, s32 y, s32 z, s32 r) {
    Box *s0 = sS0;
    s32 fp = sFp;
    s32 ret = staticCellLoop(x, y, z, r, 0);

    sS0 = s0;
    sFp = fp;
    return ret;
}

/* func_806734E4: resolve the sphere against static collision; arc results via arcMin/arcMax. */
u8 func_806734E4(s32 x, s32 y, s32 z, s32 r, s16 *arcMin, s16 *arcMax) {
    Box *s0 = sS0;
    s32 fp = sFp;

    D_807FB607 = 0;
    D_807FB60C = 999999;
    D_807FB61D = 1;
    D_807FB5FC = 0;
    sCollisionHitPart = 0;
    gCollisionNearestFound = 0;
    gCollisionPlaneDist = 999999;
    D_807FB5FE = 0;
    D_807FB5B4 = (u16 *)arcMin;
    D_807FB5B8 = (u16 *)arcMax;
    staticCellLoop(x, y, z, r, 1);
    sS0 = s0;
    sFp = fp;
    return D_807FB5FC;
}

/*
 * collisionBuildHitBox: rebuild the scratch box for static tri `tri` (index `index`
 * in cell D_807FB5BC) and return it. The asm copies $s0 into $v0 before
 * returning, so $v0 == &D_807FB550; the game's declaration
 * `CollisionBox *collisionBuildHitBox(s32, s16)` (70FD0.c) already matches.
 */
Box *collisionBuildHitBox(s32 tri, s16 index) {
    Box *s0 = sS0;
    s32 fp = sFp;
    Box *ret;

    sS0 = (Box *)(u32)tri;
    sFp = index;
    func_80673324();
    ret = sS0;
    sS0 = s0;
    sFp = fp;
    return ret;
}

/*
 * func_80673708: resolve the sphere against boxes [start, end); arc results via arcMin/arcMax.
 * gCollisionNearestTriIndex written by func_80674150 on this path takes $fp, which here is the caller's
 * unrelated $s8; the port uses the last static-tri index instead (sFp).
 */
s32 func_80673708(s32 x, s32 y, s32 z, s32 r, Box *start, Box *end, s16 *arcMin, s16 *arcMax) {
    Box *s0 = sS0;
    s32 t3 = sT3, t4 = sT4, t5 = sT5, t6 = sT6;

    D_807FB5FC = 0;
    gCollisionPlaneDist = 999999;
    D_807FB5FE = 1;
    gCollisionDeferredHit = 0;
    sCollisionHitPart = 0;
    gCollisionNearestFound = 0;
    D_807FB5B4 = (u16 *)arcMin;
    D_807FB5B8 = (u16 *)arcMax;
    sT3 = x;
    sT4 = y;
    sT5 = z;
    sT6 = r;
    for (sS0 = start; sS0 != end; sS0++) {
        if (!func_8067457C(z) || !sphereTouchesTri()) {
            continue;
        }
        if (func_80673B78()) {
            func_80673D48();
            if (gCollisionDeferredHit != 0) {
                break;
            }
        } else if (sS0->unk51 != 0) {
            sDamageSurfaceContact |= 2;
        }
    }
    sS0 = s0;
    sT3 = t3; sT4 = t4; sT5 = t5; sT6 = t6;
    return D_807FB5FC;
}

/* func_806738D0: does the sphere touch (plane distance only) any gated box whose yaw is in range. */
s32 func_806738D0(s32 x, s32 y, s32 z, s32 r, Box *start, Box *end) {
    Box *s0 = sS0;
    s32 t3 = sT3, t4 = sT4, t5 = sT5, t6 = sT6;
    s32 ret = 0;

    sT3 = x;
    sT4 = y;
    sT5 = z;
    sT6 = r;
    for (sS0 = start; sS0 != end; sS0++) {
        if (func_8067457C(z) && func_80673B78() && func_806739D8()) {
            ret = 1;
            break;
        }
    }
    sS0 = s0;
    sT3 = t3; sT4 = t4; sT5 = t5; sT6 = t6;
    return ret;
}

/* func_806742C0: widen the arc *arcMin..*arcMax by [a, b]. */
void func_806742C0(s16 a, s16 b, s16 *arcMin, s16 *arcMax) {
    D_807FB5B4 = (u16 *)arcMin;
    D_807FB5B8 = (u16 *)arcMax;
    func_80674330(a, b);
}

/*
 * func_806744A8: closest point on box's plane to the sphere centre -> gCollisionPlanePointX/C8/CC,
 * |distance| -> gCollisionPlaneDist. On a miss the asm stores its leftover $v1 = ny, $a0 = nz,
 * $a1 = x (reproduced).
 */
s32 func_806744A8(Box *box, s32 x, s32 y, s32 z, s32 r) {
    Box *s0 = sS0;
    s32 t3 = sT3, t4 = sT4, t5 = sT5, t6 = sT6;
    s32 hit;

    sS0 = box;
    sT3 = x;
    sT4 = y;
    sT5 = z;
    sT6 = r;
    hit = func_8067457C(x);
    gCollisionPlanePointX = (s32)sV1;
    gCollisionPlanePointY = (s32)sA0;
    gCollisionPlanePointZ = (s32)sA1;
    gCollisionPlaneDist = (s32)sS2;
    sS0 = s0;
    sT3 = t3; sT4 = t4; sT5 = t5; sT6 = t6;
    return hit;
}

/* collisionSpheresOverlap: |c1 - c2| < r1 + r2 with early per-axis X/Z rejects. */
s32 collisionSpheresOverlap(s32 x1, s32 y1, s32 z1, s32 r1, s32 x2, s32 y2, s32 z2, s32 r2) {
    s32 dx = SUB32(x1, x2);
    s32 rs = (s32)((u32)r1 + (u32)r2);
    s32 dy, dz;
    s64 sum;

    if (dx < 0) {
        dx = SUB32(0, dx);
    }
    if (!(dx < rs)) {
        return 0;
    }
    dz = SUB32(z1, z2);
    if (dz < 0) {
        dz = SUB32(0, dz);
    }
    if (!(dz < rs)) {
        return 0;
    }
    dy = SUB32(y1, y2);
    sum = ADD64(ADD64(MUL64(dx, dx), MUL64(dy, dy)), MUL64(dz, dz));
    return sqrtf((f32)sum) < (f32)rs;
}
