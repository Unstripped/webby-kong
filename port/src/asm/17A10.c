/*
 * C transcription of decomp/asm/global/17A10.s (handwritten): fast sine/cosine.
 * Coefficients are the generated global rodata at 0x80757320..0x8075734C; operation order follows the asm.
 */
#include "port_internal.h"
#include <math.h>

extern f32 D_80757320; /* pi/2 */
extern f32 D_80757324;
extern f32 D_80757328;
extern f32 D_8075732C;
extern f32 D_80757330;
extern f32 D_80757334; /* 1/pi */
extern f32 D_80757338; /* pi, high part */
extern f32 D_8075733C; /* pi, low part */
extern f32 D_80757340;
extern f32 D_80757344;
extern f32 D_80757348;
extern f32 D_8075734C;

/*
 * sinApprox: classifies |x| by (float bits >> 22) & 0x1FF (exponent and top mantissa bit):
 * < 0xE6 returns x; < 0xFF (|x| < 1.5) evaluates the odd polynomial directly; >= 0x136 (|x| >= 2^28)
 * returns 0; otherwise reduces by n = round(x / pi) in two parts and negates for odd n.
 */
f32 sinApprox(f32 x) {
    u32 bits;
    s32 e;
    f32 x2;
    f32 p;
    s32 n;
    f32 fn;
    f32 r;

    memcpy(&bits, &x, sizeof(bits));
    e = (s32)(bits >> 22) & 0x1FF;
    if (e < 0xFF) {
        if (e < 0xE6) {
            return x;
        }
        x2 = x * x;
        p = D_80757324 * x2;
        p = D_80757328 + p;
        p = p * x2;
        p = D_8075732C + p;
        p = p * x2;
        p = D_80757330 + p;
        p = p * (x2 * x);
        return p + x;
    }
    if (!(e < 0x136)) {
        return 0.0f;
    }
    n = (s32)lrintf(D_80757334 * x);
    fn = (f32)n;
    x = x - D_80757338 * fn;
    x = x - D_8075733C * fn;
    x2 = x * x;
    p = D_80757340 * x2;
    p = D_80757344 + p;
    p = p * x2;
    p = D_80757348 + p;
    p = p * x2;
    p = D_8075734C + p;
    p = p * x2;
    p = p * x;
    r = p + x;
    if (n & 1) {
        r = -r;
    }
    return r;
}

/* cosApprox: adds pi/2 and falls through into sinApprox. */
f32 cosApprox(f32 x) {
    x = x + D_80757320;
    return sinApprox(x);
}
