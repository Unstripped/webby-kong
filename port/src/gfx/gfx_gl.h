#ifndef PORT_GFX_GL_H
#define PORT_GFX_GL_H
#include "gfx/gfx.h"
#include <GLES3/gl3.h>

/* combiner program (gfx_combiner.c) */
typedef struct {
    GLuint prog;
    GLint uTex0, uTex1, uPrim, uEnv, uFog, uBlend, uPrimLod, uTile0, uTile1, uSize0, uSize1, uAlphaRef, uPrimDepth;
    GLint uLodScale, uMaxLevel, uMipTile, uMipSize;
    u8 usesTex0, usesTex1;
    /* last values set (gfx_gl.c skips unchanged uniforms); NaN until first set */
    float cPrim[4], cEnv[4], cFog[4], cBlend[4], cTile0[4], cTile1[4], cSize0[2], cSize1[2];
    float cPrimLod, cAlphaRef, cPrimDepth, cLodScale, cMaxLevel;
} GfxProgram;

typedef struct {
    u32 combineHi, combineLo;
    u8 cycle;      /* 0: 1-cycle, 1: 2-cycle, 2: copy, 3: fill */
    u8 alphaMode;  /* 0 none, 1 threshold (blend alpha), 2 dither/coverage (0.5) */
    u8 fogBlend;   /* blender mixes fog by shade alpha in cycle 1 */
    u8 primDepth;  /* z source = primitive depth */
    u8 lod;        /* texture LOD: TEXEL0/TEXEL1 are per-pixel mip levels of uTex0 */
} GfxProgramKey;

GfxProgram *gfxProgramGet(const GfxProgramKey *key);
extern int gGfxProgramBuilt; /* gfxProgramGet compiled a program (left it current): set, cleared by gfx_gl.c */
GLuint gfxCompileProgram(const char *vs, const char *fs);

/* full-screen quad without a VBO (gfx_fb.c): gl_VertexID positions, vUv 0..1 */
extern const char *gGfxQuadVs;
extern GLuint gGfxQuadVao;

/* GPU-side framebuffer effects (gfx_fbsnap.c): an RDRAM copy of a colour image that the game draws back as
 * texture tiles is drawn from a GPU copy of that image at render scale instead of from its 320x240 texels. The
 * RDRAM copy is left exactly as the game wrote it. */
void gfxFbSnapOfImage(u32 dst, u32 src, u32 len); /* dst holds a copy of colour image src */
/* dst holds `cols` x `rows` tiles of `tileW` x `tileH` sepia texels from (x, y) of colour image src */
void gfxFbSnapSepia(u32 dst, u32 src, u32 x, u32 y, u32 cols, u32 rows, u32 tileW, u32 tileH);
void gfxFbSnapLerpWhite(u32 dst, u32 src, u32 len, float t); /* dst holds src faded towards white by t */
int gfxFbSnapCpuWrote(u32 addr, u32 len); /* the game wrote the range; 1 when the copy's alpha was redrawn */
void gfxFbSnapBeginTask(void);                      /* each copy is re-checked against RDRAM once per task */
/* the copy a tile's texels come from: its texture, the tile's origin and the copy's size in N64 pixels; 0 when
 * the tile does not come from one (the caller decodes the texels as usual) */
GLuint gfxFbSnapTile(const GfxTileSource *src, float *origin, float *size);
void gfxFbSnapReset(void); /* RDRAM was replaced (quickload): no copy stands for it any more */
void gfxFbSnapQsRanges(QsSink *out);

/* framebuffers (gfx_fb.c) */
typedef struct {
    u32 addr;
    u32 width, height; /* N64 pixels */
    u32 physW, physH;  /* FBO size in GL pixels */
    u32 scale;         /* GL pixels per N64 pixel this image was allocated at */
    u32 ox;            /* GL x of the N64 image inside the FBO */
    u32 sideTask;      /* gfx task that last blanked the side areas (gfx_gl.c) */
    GLuint fbo;        /* draw target: msColor (MSAA) or color, plus the shared depth */
    GLuint color;      /* single-sample RGBA8 texture: present/readback source */
    GLuint msColor;    /* MSAA colour renderbuffer, 0 when MSAA is off */
    GLuint resolveFbo; /* FBO whose only attachment is color */
    int cpuDirty;      /* RDRAM newer than the texture */
    int gpuDirty;      /* texture drawn since the last RDRAM sync */
    int resolveDirty;  /* fbo drawn since color was last resolved from it */
    int everDrawn;
} GfxFb;

extern int gGfxDepthDirty; /* depth texture drawn since the last RDRAM depth sync */
extern u32 gGfxDepthSerial; /* bumped by every depth write; gfxFbDepthPrefetch's data is current while unchanged */
extern GLuint gGfxDepthTex;
extern u32 gGfxDepthW, gGfxDepthH;
extern u32 gGfxScale; /* GL pixels per N64 pixel for images allocated now (follows the window height) */

void gfxFbInit(void);
GfxFb *gfxFbFor(u32 addr, u32 width);
void gfxFbSetWindowSize(int w, int h);
u32 gfxFbPhysWidth(u32 width);
GfxFb *gfxFbFind(u32 addr);
void gfxFbBind(GfxFb *fb);
void gfxFbDrew(GfxFb *fb);
void gfxFbResolve(GfxFb *fb);
void gfxFbUploadRdram(GfxFb *fb);
void gfxFbClearDepth(void);
void gfxFbDepthPrefetch(void); /* end of a task: start reading the depth image back without waiting for it */
void gfxFbDepthCollect(void);  /* copy a finished prefetch to host memory now, while nothing newer is queued */
void gfxFbPresent(u32 addr, int black, int winW, int winH);
/* frame interpolation: the in-between image, shaped like the VI image `real`, with its own depth */
GfxFb *gfxFbInterpFor(const GfxFb *real);
void gfxFbInterpClearDepth(void);
void gfxFbInterpStore(int slot); /* the replay just drawn, resolved into slot 0..6 */
int gfxFbPresentInterp(int slot, int winW, int winH); /* 0 when the slot holds no current image */
/* a point of the drawable (fractions, top-left origin) in N64 pixels of the last presented image; 0 off-image */
int gfxFbPresentedToN64(float fx, float fy, float *x, float *y);
void gfxFbCpuWrote(u32 addr, u32 len);
void gfxFbSyncRdram(u32 addr, u32 len);

/* quicksave: host statics kept across a restore, and post-restore invalidation */
void gfxFbQsRanges(QsSink *out);
void gfxCombinerQsRanges(QsSink *out);
void gfxFbQsRestored(void);
/* gfx_gl.c for gfx_texpack.c: drop every cached texture (looked up again on next use); forget the GL state shadow
 * after GL calls made outside a frame */
void gfxGlTexCacheFlush(void);
void gfxGlStateReset(void);
#endif
