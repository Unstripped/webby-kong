#ifndef PORT_GFX_GL_H
#define PORT_GFX_GL_H
#include "gfx/gfx.h"
#include <GLES3/gl3.h>

/* combiner program (gfx_combiner.c) */
typedef struct {
    GLuint prog;
    GLint uTex0, uTex1, uPrim, uEnv, uFog, uBlend, uPrimLod, uTile0, uTile1, uSize0, uSize1, uAlphaRef, uPrimDepth;
    u8 usesTex0, usesTex1;
} GfxProgram;

typedef struct {
    u32 combineHi, combineLo;
    u8 cycle;      /* 0: 1-cycle, 1: 2-cycle, 2: copy, 3: fill */
    u8 alphaMode;  /* 0 none, 1 threshold (blend alpha), 2 dither/coverage (0.5) */
    u8 fogBlend;   /* blender mixes fog by shade alpha in cycle 1 */
    u8 primDepth;  /* z source = primitive depth */
} GfxProgramKey;

const GfxProgram *gfxProgramGet(const GfxProgramKey *key);
GLuint gfxCompileProgram(const char *vs, const char *fs);

/* framebuffers (gfx_fb.c) */
typedef struct {
    u32 addr;
    u32 width, height; /* N64 pixels */
    u32 physW, physH;  /* FBO size in GL pixels */
    u32 ox;            /* GL x of the N64 image inside the FBO */
    u32 sideTask;      /* gfx task that last blanked the side areas (gfx_gl.c) */
    GLuint fbo, color;
    int cpuDirty;      /* RDRAM newer than the texture */
    int gpuDirty;      /* texture drawn since the last RDRAM sync */
    int everDrawn;
} GfxFb;

extern int gGfxDepthDirty; /* depth texture drawn since the last RDRAM depth sync */
extern GLuint gGfxDepthTex;
extern u32 gGfxDepthW, gGfxDepthH;

void gfxFbInit(void);
GfxFb *gfxFbFor(u32 addr, u32 width);
void gfxFbSetWindowAspect(float aspect);
u32 gfxFbPhysWidth(u32 width);
GfxFb *gfxFbFind(u32 addr);
void gfxFbBind(GfxFb *fb);
void gfxFbUploadRdram(GfxFb *fb);
void gfxFbClearDepth(void);
void gfxFbPresent(u32 addr, int black, int winW, int winH);
void gfxFbCpuWrote(u32 addr, u32 len);
void gfxFbSyncRdram(u32 addr, u32 len);

/* quicksave: host statics kept across a restore, and post-restore invalidation */
int gfxFbQsRanges(QsRange *out);
int gfxCombinerQsRanges(QsRange *out);
void gfxFbQsRestored(void);
#endif
