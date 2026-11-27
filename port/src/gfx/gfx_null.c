/* Headless backend: the interpreter walks and validates every display list; nothing is drawn. */
#include "gfx/gfx.h"

void gfxBackendInit(void) {}
void gfxBackendBeginTask(void) {}
void gfxBackendEndTask(void) {}
void gfxBackendSetColorImage(u32 addr, u32 width, u32 siz) {
    (void)addr;
    (void)width;
    (void)siz;
}

void gfxBackendDrawTriangles(const GfxState *st, const GfxVtx *v, u32 count, const GfxLit *lit) {
    (void)v;
    (void)count;
    (void)lit;
    if (st->texOn) {
        GfxTexImage img;
        (void)gfxTmemDecode(st, st->texTile, &img);
    }
}

void gfxBackendFillRect(const GfxState *st, float ulx, float uly, float lrx, float lry) {
    (void)st;
    (void)ulx;
    (void)uly;
    (void)lrx;
    (void)lry;
}

void gfxBackendTexRect(const GfxState *st, int tile, float ulx, float uly, float lrx, float lry, float s, float t,
                       float dsdx, float dtdy, int flip) {
    GfxTexImage img;
    (void)ulx;
    (void)uly;
    (void)lrx;
    (void)lry;
    (void)s;
    (void)t;
    (void)dsdx;
    (void)dtdy;
    (void)flip;
    (void)gfxTmemDecode(st, tile, &img);
}

void gfxBackendPresent(u32 fb, int black) {
    (void)fb;
    (void)black;
}

void gfxBackendCpuWrote(u32 addr, u32 len) {
    (void)addr;
    (void)len;
}

void gfxBackendSyncRdram(u32 addr, u32 len) {
    (void)addr;
    (void)len;
}

void gfxBackendFbSnapshot(u32 dst, u32 src, u32 len) {
    (void)dst;
    (void)src;
    (void)len;
}

void gfxBackendFbSepia(u32 dst, u32 src, u32 x, u32 y, u32 cols, u32 rows, u32 tileW, u32 tileH) {
    (void)dst;
    (void)src;
    (void)x;
    (void)y;
    (void)cols;
    (void)rows;
    (void)tileW;
    (void)tileH;
}

void gfxBackendFbLerpWhite(u32 dst, u32 src, u32 len, float t) {
    (void)dst;
    (void)src;
    (void)len;
    (void)t;
}

void gfxBackendBlankSides(const GfxState *st) {
    (void)st;
}

int gfxBackendInterpLevel(void) {
    return 0;
}

int gfxBackendSetInterp(int level) {
    (void)level;
    return 0;
}

void gfxBackendInterpBegin(void) {}

int gfxBackendInterpEnd(int slot) {
    (void)slot;
    return 0;
}

int gfxBackendPresentInterp(int slot) {
    (void)slot;
    return 0;
}

int portGfxWideSpanX(f32 *left, f32 *right) {
    (void)left;
    (void)right;
    return 0;
}

f32 portGfxWideHalfYaw(f32 halfYawDeg, f32 widen) {
    (void)widen;
    return halfYawDeg;
}

void portGfxWideClipX(s16 *left, s16 *right) {
    (void)left;
    (void)right;
}

void gfxBackendQsRanges(QsSink *out) {
    gfxQsRanges(out);
}

void gfxBackendQsRestored(void) {}
