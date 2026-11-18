#ifndef PORT_GFX_H
#define PORT_GFX_H
#include "port_internal.h"

#define PORT_GFX_SCALE 2

/* ---- RDP state snapshot handed to the backend with every batch ---- */
typedef struct {
    u8 fmt, siz, palette, cms, cmt, masks, maskt, shifts, shiftt;
    u16 line, tmem;
    u16 uls, ult, lrs, lrt; /* 10.2 */
} GfxTile;

typedef struct {
    u32 cimg, cimgWidth, cimgSiz;
    u32 zimg;
    u32 combineHi, combineLo;
    u32 otherH, otherL;
    u32 geometry;
    u8 prim[4], env[4], fog[4], blend[4];
    u8 primLodFrac;
    u32 fillColor;
    u16 primDepth;
    s32 scissor[4]; /* ulx, uly, lrx, lry in screen pixels (N64 space) */
    int texOn;
    int texTile;
    GfxTile tiles[8];
} GfxState;

/* one vertex as drawn: clip-space position, shade color (0..1), texture coords in texels of the render tile */
typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float s, t;
} GfxVtx;

/* decoded texture (RGBA8, rows top-down); key identifies TMEM contents + params */
typedef struct {
    u32 width, height;
    u64 key;
    const u8 *rgba;
} GfxTexImage;

extern GfxState gGfx;
extern float gGfxTargetWide; /* current colour image: FBO width / (N64 width * PORT_GFX_SCALE), >= 1 */
extern u8 gGfxTmem[4096];

/* gfx_tmem.c */
void gfxTmemLoadBlock(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs, u32 dxt);
void gfxTmemLoadTile(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs, u32 lrt);
void gfxTmemLoadTlut(const GfxState *st, u32 timg, int tile, u32 count);
/* decode tile `t` into an RGBA8 image (pointer valid until next decode); returns 0 if empty */
int gfxTmemDecode(const GfxState *st, int t, GfxTexImage *out);
u32 gfxTileWidth(const GfxTile *t);
u32 gfxTileHeight(const GfxTile *t);

/* backend (gfx_gl.c or gfx_null.c) */
void gfxBackendInit(void);
void gfxBackendBeginTask(void);
void gfxBackendEndTask(void);
void gfxBackendSetColorImage(u32 addr, u32 width, u32 siz);
void gfxBackendDrawTriangles(const GfxState *st, const GfxVtx *v, u32 count);
/* screen-space rectangle in N64 pixels (lrx/lry exclusive) */
void gfxBackendFillRect(const GfxState *st, float ulx, float uly, float lrx, float lry);
void gfxBackendTexRect(const GfxState *st, int tile, float ulx, float uly, float lrx, float lry,
                       float s, float t, float dsdx, float dtdy, int flip);
void gfxBackendPresent(u32 fb, int black);
void gfxBackendCpuWrote(u32 addr, u32 len);
void gfxBackendSyncRdram(u32 addr, u32 len);

void gfxWarnOnce(const char *key, const char *fmt, ...);
u8 *gfxHost(u32 segAddr);
#endif
