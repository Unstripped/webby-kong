#ifndef PORT_GFX_H
#define PORT_GFX_H
#include "port_internal.h"

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
    int texLevels; /* G_TEXTURE max mip level: texture LOD uses tiles texTile..texTile + texLevels */
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
extern float gGfxTargetWide; /* current colour image: FBO width / (N64 width * GfxFb.scale), >= 1 */
extern u8 gGfxTmem[4096];

/* gfx_tmem.c */
void gfxTmemLoadBlock(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs, u32 dxt);
void gfxTmemLoadTile(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs, u32 lrt);
void gfxTmemLoadTlut(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs, u32 lrt);
/* decode tile `t` into an RGBA8 image (pointer valid until next decode); returns 0 if empty */
int gfxTmemDecode(const GfxState *st, int t, GfxTexImage *out);
/* the size and key gfxTmemDecode would return, without decoding (rgba NULL); 0 if empty */
int gfxTmemKey(const GfxState *st, int t, GfxTexImage *out);
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
/* PORT_GFX_BLANK_SIDES: black a widened colour image left and right of the game's scissor */
void gfxBackendBlankSides(const GfxState *st);
void gfxBackendPresent(u32 fb, int black);
void gfxBackendCpuWrote(u32 addr, u32 len);
void gfxBackendSyncRdram(u32 addr, u32 len);
/* frame interpolation (gfx.c): the backend draws a VI-image task's matrix-blended replays into the in-between image
 * (offscreen images and RDRAM are left alone), keeps each in a slot and presents a slot on request. The null
 * backend has none. */
int gfxBackendInterpLevel(void); /* the player setting: 0 = 30 fps; 1, 2, 3 = up to 60, 120, 240 */
int gfxBackendSetInterp(int level); /* returns the value in effect */
void gfxBackendInterpBegin(void);
int gfxBackendInterpEnd(int slot); /* 1 when the replay drew the VI image, now kept in `slot` */
int gfxBackendPresentInterp(int slot);
void gfxInterpReset(void);
int gfxInterpQsRanges(QsRange *out);

void gfxWarnOnce(const char *key, const char *fmt, ...);
u8 *gfxHost(u32 segAddr);

/* debug pixel probe (web/src/game/debug/api.ts): the next gfx task records the RDP state of every triangle covering
 * N64 pixel (x, y); gfxProbeResult is "" until that task ran */
void gfxProbeArm(float x, float y);
const char *gfxProbeResult(void);
#endif
