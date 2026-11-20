/* WebGL2 backend: SDL window + GLES 3.0 context, texture cache, RDP state -> GL state, draws. */
#include "gfx/gfx_gl.h"
#include <SDL2/SDL.h>
#include <emscripten.h>
#include <math.h>
#include <stdlib.h>

#define TEX_CACHE_SIZE 4096
#define WIN_W 960
#define WIN_H 720

typedef struct {
    u64 key;
    GLuint tex;
    u32 w, h;
} TexEntry;

static SDL_Window *sWindow;
static SDL_GLContext sContext;
static GLuint sVao, sVbo;
static TexEntry sTexCache[TEX_CACHE_SIZE];
static int sTexCount;
static GLuint sWhiteTex;
static GfxFb *sTarget;
static int sTargetIsDepth;
static u32 sTaskSerial;
static u8 *sMipChain; /* decoded mip levels staged for upload (bindMipTiles) */
static size_t sMipChainCap;

/* frame grab for web/debug.js: requested from JS, filled after the next present */
static int sCaptureReq, sCaptureW, sCaptureH;
static u32 sCaptureSeq;
static u8 *sCapturePx;
static size_t sCaptureCap;

int gfxBackendQsRanges(QsRange *out) {
    int n = 0;
    out[n++] = QS_RANGE(sWindow);
    out[n++] = QS_RANGE(sContext);
    out[n++] = QS_RANGE(sVao);
    out[n++] = QS_RANGE(sVbo);
    out[n++] = QS_RANGE(sTexCache);
    out[n++] = QS_RANGE(sTexCount);
    out[n++] = QS_RANGE(sWhiteTex);
    out[n++] = QS_RANGE(sTarget);
    out[n++] = QS_RANGE(sTargetIsDepth);
    out[n++] = QS_RANGE(sTaskSerial);
    out[n++] = QS_RANGE(sCaptureReq);
    out[n++] = QS_RANGE(sCaptureW);
    out[n++] = QS_RANGE(sCaptureH);
    out[n++] = QS_RANGE(sCaptureSeq);
    out[n++] = QS_RANGE(sCapturePx);
    out[n++] = QS_RANGE(sCaptureCap);
    out[n++] = QS_RANGE(sMipChain);
    out[n++] = QS_RANGE(sMipChainCap);
    n += gfxFbQsRanges(out + n);
    n += gfxCombinerQsRanges(out + n);
    return n;
}

static void texCacheFlush(void) {
    int i;
    for (i = 0; i < sTexCount; i++) {
        glDeleteTextures(1, &sTexCache[i].tex);
    }
    sTexCount = 0;
}

/* RDRAM and TMEM were replaced: cached textures and framebuffer images no longer match them */
void gfxBackendQsRestored(void) {
    texCacheFlush();
    sTarget = NULL;
    sTargetIsDepth = 0;
    gfxFbQsRestored();
}

void gfxBackendInit(void) {
    u8 white[4] = { 255, 255, 255, 255 };
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        portAbort("PORT: SDL video init failed: %s", SDL_GetError());
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    sWindow = SDL_CreateWindow("Donkey Kong 64", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, WIN_W, WIN_H,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (sWindow == NULL) {
        portAbort("PORT: SDL window failed: %s", SDL_GetError());
    }
    sContext = SDL_GL_CreateContext(sWindow);
    if (sContext == NULL) {
        portAbort("PORT: GLES 3.0 context failed: %s", SDL_GetError());
    }
    glGenVertexArrays(1, &sVao);
    glGenBuffers(1, &sVbo);
    glBindVertexArray(sVao);
    glBindBuffer(GL_ARRAY_BUFFER, sVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GfxVtx), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GfxVtx), (void *)16);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GfxVtx), (void *)32);
    glGenTextures(1, &sWhiteTex);
    glBindTexture(GL_TEXTURE_2D, sWhiteTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    gfxFbInit();
}

void gfxBackendBeginTask(void) {
    int w, h;
    SDL_GL_GetDrawableSize(sWindow, &w, &h);
    gfxFbSetWindowSize(w, h);
    sTaskSerial++;
    sTarget = NULL;
    gGfxTargetWide = 1.0f;
    sTargetIsDepth = 0;
}

void gfxBackendEndTask(void) {
    glBindVertexArray(sVao);
}

void gfxBackendSetColorImage(u32 addr, u32 width, u32 siz) {
    (void)siz;
    if (addr == gGfx.zimg) {
        sTargetIsDepth = 1;
        sTarget = NULL;
        gGfxTargetWide = 1.0f;
        return;
    }
    sTargetIsDepth = 0;
    sTarget = gfxFbFor(addr, width);
    gfxFbBind(sTarget);
    gGfxTargetWide = sTarget->physW / (float)(sTarget->width * sTarget->scale);
    if (sTarget->ox > 0 && sTarget->sideTask != sTaskSerial) {
        /* the game only covers its 4:3 image; blank the side areas once per task so frames that draw no
         * full-width clear don't show stale pixels there */
        u32 sh = sTarget->physH, right = sTarget->ox + sTarget->width * sTarget->scale;
        sTarget->sideTask = sTaskSerial;
        glEnable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glScissor(0, 0, sTarget->ox, sh);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(right, 0, sTarget->physW - right, sh);
        glClear(GL_COLOR_BUFFER_BIT);
        sTarget->resolveDirty = 1;
    }
}

/* ---------------------------------------------------------------- textures */

static GLenum wrapMode(u8 cm) {
    if (cm & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (cm & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void tileParamsFor(const GfxTile *t, float *tileParams) {
    tileParams[0] = t->shifts == 0 ? 1.0f : t->shifts <= 10 ? 1.0f / (float)(1 << t->shifts) : (float)(1 << (16 - t->shifts));
    tileParams[1] = t->shiftt == 0 ? 1.0f : t->shiftt <= 10 ? 1.0f / (float)(1 << t->shiftt) : (float)(1 << (16 - t->shiftt));
    tileParams[2] = t->uls / 4.0f;
    tileParams[3] = t->ult / 4.0f;
}

/* cached GL texture for (key, w, h), or 0 */
static GLuint texCacheFind(u64 key, u32 w, u32 h) {
    int i;
    for (i = 0; i < sTexCount; i++) {
        if (sTexCache[i].key == key && sTexCache[i].w == w && sTexCache[i].h == h) {
            return sTexCache[i].tex;
        }
    }
    return 0;
}

/* new cached GL texture, bound to the active unit; the caller uploads its levels */
static GLuint texCacheAdd(u64 key, u32 w, u32 h) {
    GLuint tex;
    if (sTexCount == TEX_CACHE_SIZE) {
        texCacheFlush();
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    sTexCache[sTexCount].key = key;
    sTexCache[sTexCount].tex = tex;
    sTexCache[sTexCount].w = w;
    sTexCache[sTexCount].h = h;
    sTexCount++;
    return tex;
}

static void setSampling(const GfxState *st, const GfxTile *t, int mipmapped) {
    int linear = (st->otherH & (3u << G_MDSFT_TEXTFILT)) != G_TF_POINT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode(t->cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode(t->cmt));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mipmapped ? (linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                              : (linear ? GL_LINEAR : GL_NEAREST));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
}

static void bindTile(const GfxState *st, int tile, int unit, float *tileParams, float *size) {
    const GfxTile *t = &st->tiles[tile & 7];
    GfxTexImage img;
    GLuint tex = sWhiteTex;
    tileParamsFor(t, tileParams);
    size[0] = 1.0f;
    size[1] = 1.0f;
    glActiveTexture(GL_TEXTURE0 + unit);
    if (gfxTmemDecode(st, tile & 7, &img)) {
        size[0] = (float)img.width;
        size[1] = (float)img.height;
        tex = texCacheFind(img.key, img.width, img.height);
        if (tex == 0) {
            tex = texCacheAdd(img.key, img.width, img.height);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba);
        }
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    setSampling(st, t, 0);
}

/* texture LOD: tiles tile..tile + maxLevel as the GL mips of one texture on unit 0. Each level must be half the
 * previous (N64 mip chains are); the chain stops at the first that isn't. mipTile/mipSize get each level's tile
 * params and size (level 0 also in tileParams/size). Returns the last level chained, 0 when there is no chain
 * (the caller binds the tile alone). */
static int bindMipTiles(const GfxState *st, int tile, int maxLevel, float *tileParams, float *size,
                        float mipTile[8][4], float mipSize[8][2]) {
    GfxTexImage img;
    u32 w0 = 0, h0 = 0, off[8], w[8], h[8];
    u64 key = 0x6C6F64636861696Eull; /* never equals a single tile's key */
    int n = -1, k;
    GLuint tex;
    for (k = 0; k <= maxLevel && k < 8; k++) {
        size_t need;
        if (!gfxTmemDecode(st, (tile + k) & 7, &img)) {
            break;
        }
        if (k == 0) {
            w0 = img.width;
            h0 = img.height;
            off[0] = 0;
        } else if (img.width != (w0 >> k ? w0 >> k : 1) || img.height != (h0 >> k ? h0 >> k : 1)) {
            break;
        } else {
            off[k] = off[k - 1] + w[k - 1] * h[k - 1] * 4;
        }
        w[k] = img.width;
        h[k] = img.height;
        need = off[k] + (size_t)w[k] * h[k] * 4;
        if (need > sMipChainCap) {
            u8 *p = realloc(sMipChain, need * 2);
            if (p == NULL) {
                break;
            }
            sMipChain = p;
            sMipChainCap = need * 2;
        }
        memcpy(sMipChain + off[k], img.rgba, (size_t)w[k] * h[k] * 4);
        key = (key ^ img.key) * 1099511628211ull;
        n = k;
    }
    if (n < 1) {
        return 0;
    }
    key = (key ^ (u64)n) * 1099511628211ull;
    glActiveTexture(GL_TEXTURE0);
    tex = texCacheFind(key, w0, h0);
    if (tex == 0) {
        tex = texCacheAdd(key, w0, h0);
        for (k = 0; k <= n; k++) {
            glTexImage2D(GL_TEXTURE_2D, k, GL_RGBA8, w[k], h[k], 0, GL_RGBA, GL_UNSIGNED_BYTE, sMipChain + off[k]);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    setSampling(st, &st->tiles[tile & 7], 1);
    for (k = 0; k <= n; k++) {
        tileParamsFor(&st->tiles[(tile + k) & 7], mipTile[k]);
        mipSize[k][0] = (float)w[k];
        mipSize[k][1] = (float)h[k];
    }
    memcpy(tileParams, mipTile[0], sizeof mipTile[0]);
    size[0] = (float)w0;
    size[1] = (float)h0;
    return n;
}

/* ---------------------------------------------------------------- render state */

/* x span covering the image up to overscan margins (<= width/16 per side) on a widened target: extends into
 * the side areas. Unwidened targets (ox == 0) keep the game's exact scissors and rects. */
static int spansWidth(float x0, float x1) {
    float w = (float)sTarget->width;
    return sTarget->ox > 0 && x0 <= w / 16.0f && x1 >= w - w / 16.0f;
}

/* N64 scissor (pixels, y down) -> GL scissor in the bound FBO. On a widened target each edge within the overscan
 * margin of the image edge moves out to the wide edge on its own: the map clips a chunk seen through a portal to
 * the portal's screen rect, and a rect touching one side must still reach into that side area. */
static void scissorN64(s32 x0, s32 y0, s32 x1, s32 y1) {
    s32 h = (s32)sTarget->height, s = (s32)sTarget->scale, px0, px1;
    float w = (float)sTarget->width;
    px0 = (s32)sTarget->ox + x0 * s;
    px1 = (s32)sTarget->ox + x1 * s;
    if (sTarget->ox > 0) {
        if ((float)x0 <= w / 16.0f) {
            px0 = 0;
        }
        if ((float)x1 >= w - w / 16.0f) {
            px1 = (s32)sTarget->physW;
        }
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(px0, (h - y1) * s, px1 - px0, (y1 - y0) * s);
}

static void applyState(const GfxState *st, int forRect, int tile) {
    GfxProgramKey key;
    const GfxProgram *p;
    u32 cyc = (st->otherH >> G_MDSFT_CYCLETYPE) & 3;
    u32 om = st->otherL;
    u32 blend1 = om >> 16;
    int lastCycleShift = cyc == 1 ? 0 : 2; /* 2-cycle: memory blend in cycle 2 (low bits) */
    u32 bp = (blend1 >> (12 + lastCycleShift)) & 3, ba = (blend1 >> (8 + lastCycleShift)) & 3;
    u32 bm = (blend1 >> (4 + lastCycleShift)) & 3, bb = (blend1 >> (0 + lastCycleShift)) & 3;
    u32 fp = (blend1 >> 14) & 3, fa = (blend1 >> 10) & 3; /* cycle 1 P/A */
    float tile0[4], tile1[4], size0[2], size1[2], mipTile[8][4], mipSize[8][2];
    int t0 = forRect ? tile : st->texTile, maxLevel;
    memset(&key, 0, sizeof key);
    key.combineHi = st->combineHi;
    key.combineLo = st->combineLo;
    key.cycle = (u8)cyc;
    if ((om & 3) == G_AC_THRESHOLD) {
        key.alphaMode = 1;
    } else if ((om & 3) == G_AC_DITHER || ((om & ALPHA_CVG_SEL) && (om & CVG_X_ALPHA) && !(om & FORCE_BL))) {
        key.alphaMode = cyc == 2 ? 1 : 2;
    }
    key.fogBlend = (st->geometry & G_FOG) && fp == G_BL_CLR_FOG && fa == G_BL_A_SHADE && cyc < 2;
    key.primDepth = (om & G_ZS_PRIM) != 0;
    /* texture LOD: the RDP computes it only in 2-cycle mode; a 1-cycle draw samples its render tile even with G_TL_LOD
     * left set by an earlier draw. Triangles only: rects have no per-pixel LOD. */
    maxLevel = 0;
    if (!forRect && st->texOn && st->texLevels > 0 && (st->otherH & G_TL_LOD) && cyc == 1) {
        maxLevel = bindMipTiles(st, t0, st->texLevels, tile0, size0, mipTile, mipSize);
    }
    key.lod = maxLevel > 0;
    p = gfxProgramGet(&key);
    glUseProgram(p->prog);
    glUniform4f(p->uPrim, st->prim[0] / 255.0f, st->prim[1] / 255.0f, st->prim[2] / 255.0f, st->prim[3] / 255.0f);
    glUniform4f(p->uEnv, st->env[0] / 255.0f, st->env[1] / 255.0f, st->env[2] / 255.0f, st->env[3] / 255.0f);
    glUniform4f(p->uFog, st->fog[0] / 255.0f, st->fog[1] / 255.0f, st->fog[2] / 255.0f, st->fog[3] / 255.0f);
    glUniform4f(p->uBlend, st->blend[0] / 255.0f, st->blend[1] / 255.0f, st->blend[2] / 255.0f, st->blend[3] / 255.0f);
    glUniform1f(p->uPrimLod, st->primLodFrac / 255.0f);
    glUniform1f(p->uAlphaRef, st->blend[3] / 255.0f);
    glUniform1f(p->uPrimDepth, (st->primDepth & 0x7FFF) / 32767.0f);
    if (key.lod) {
        /* both texels come from the mip chain on unit 0; LOD counts tile0 texels per N64 pixel */
        glUniform1f(p->uLodScale, (float)sTarget->scale);
        glUniform1f(p->uMaxLevel, (float)maxLevel);
        glUniform4fv(p->uMipTile, maxLevel + 1, &mipTile[0][0]);
        glUniform2fv(p->uMipSize, maxLevel + 1, &mipSize[0][0]);
    } else if (p->usesTex0 && (st->texOn || forRect)) {
        bindTile(st, t0, 0, tile0, size0);
    } else {
        tile0[0] = tile0[1] = 1.0f;
        tile0[2] = tile0[3] = 0.0f;
        size0[0] = size0[1] = 1.0f;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sWhiteTex);
    }
    if (p->usesTex1 && !key.lod && (st->texOn || forRect)) {
        bindTile(st, (t0 + 1) & 7, 1, tile1, size1);
    } else {
        tile1[0] = tile1[1] = 1.0f;
        tile1[2] = tile1[3] = 0.0f;
        size1[0] = size1[1] = 1.0f;
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sWhiteTex);
    }
    glUniform4fv(p->uTile0, 1, tile0);
    glUniform4fv(p->uTile1, 1, tile1);
    glUniform2fv(p->uSize0, 1, size0);
    glUniform2fv(p->uSize1, 1, size1);

    /* blending: IN*A_IN + MEM*(1-A) under FORCE_BL */
    if ((om & FORCE_BL) && bp == G_BL_CLR_IN && ba == G_BL_A_IN && bm == G_BL_CLR_MEM && bb == G_BL_1MA && cyc < 2) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    /* depth */
    if (cyc >= 2) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    } else {
        int zcmp = (om & Z_CMP) && (st->geometry & G_ZBUFFER || forRect);
        int zupd = (om & Z_UPD) != 0;
        if (zcmp || zupd) {
            /* RDP z compare on a fully covered pixel: opaque, interpenetrating and translucent modes pass only when
             * strictly nearer (a coplanar redraw fails: Galleon's gold stacks draw a white untextured copy of their
             * textured tops after them); decal passes on equal z (polygon offset below) */
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(!zcmp ? GL_ALWAYS : (om & ZMODE_DEC) == ZMODE_DEC ? GL_LEQUAL : GL_LESS);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(zupd ? GL_TRUE : GL_FALSE);
    }
    if ((om & ZMODE_DEC) == ZMODE_DEC) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-2.0f, -2.0f);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    /* scissor (N64 pixels, y down) */
    if (sTarget != NULL) {
        s32 x0 = st->scissor[0], y0 = st->scissor[1], x1 = st->scissor[2], y1 = st->scissor[3];
        if (x1 > x0 && y1 > y0) {
            scissorN64(x0, y0, x1, y1);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }
}

static void draw(const GfxVtx *v, u32 count) {
    glBindVertexArray(sVao);
    glBindBuffer(GL_ARRAY_BUFFER, sVbo);
    glBufferData(GL_ARRAY_BUFFER, count * sizeof(GfxVtx), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, count);
    if (sTarget != NULL) {
        gfxFbDrew(sTarget);
        gGfxDepthDirty = 1;
    }
}

void gfxBackendDrawTriangles(const GfxState *st, const GfxVtx *v, u32 count) {
    if (sTarget == NULL) {
        return;
    }
    applyState(st, 0, st->texTile);
    draw(v, count);
}

static void screenVtx(GfxVtx *o, float x, float y, float z, float s, float t, float r, float g, float b, float a) {
    float h = (float)sTarget->height;
    o->x = (sTarget->ox + x * sTarget->scale) / (sTarget->physW * 0.5f) - 1.0f;
    o->y = 1.0f - y / (h / 2);
    o->z = z;
    o->w = 1.0f;
    o->r = r;
    o->g = g;
    o->b = b;
    o->a = a;
    o->s = s;
    o->t = t;
}

void gfxBackendFillRect(const GfxState *st, float ulx, float uly, float lrx, float lry) {
    u32 cyc = (st->otherH >> G_MDSFT_CYCLETYPE) & 3;
    if (sTargetIsDepth) {
        if (cyc == 3) {
            gfxFbClearDepth();
        }
        return;
    }
    if (sTarget == NULL) {
        return;
    }
    if (cyc == 3) {
        u16 c = (u16)(st->fillColor >> 16);
        gfxFbBind(sTarget);
        scissorN64((s32)ulx, (s32)uly, (s32)lrx, (s32)lry);
        glClearColor(((c >> 11) & 0x1F) / 31.0f, ((c >> 6) & 0x1F) / 31.0f, ((c >> 1) & 0x1F) / 31.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        gfxFbDrew(sTarget);
        return;
    }
    {
        GfxVtx q[6];
        if (spansWidth(ulx, lrx)) {
            /* full-width fills (clears, fades) cover the side areas too */
            ulx = -(float)sTarget->ox / sTarget->scale;
            lrx = (float)(sTarget->physW - sTarget->ox) / sTarget->scale;
        }
        /* rectangles have no shade on the RDP: a SHADE input reads 0 (the arcade's 1-cycle backdrop is black) */
        applyState(st, 1, 0);
        screenVtx(&q[0], ulx, uly, 0, 0, 0, 0, 0, 0, 0);
        screenVtx(&q[1], lrx, uly, 0, 0, 0, 0, 0, 0, 0);
        screenVtx(&q[2], ulx, lry, 0, 0, 0, 0, 0, 0, 0);
        q[3] = q[2];
        q[4] = q[1];
        screenVtx(&q[5], lrx, lry, 0, 0, 0, 0, 0, 0, 0);
        draw(q, 6);
    }
}

void gfxBackendTexRect(const GfxState *st, int tile, float ulx, float uly, float lrx, float lry, float s, float t,
                       float dsdx, float dtdy, int flip) {
    GfxVtx q[6];
    float s1, t1;
    if (sTarget == NULL) {
        return;
    }
    s1 = s + dsdx * (flip ? (lry - uly) : (lrx - ulx));
    t1 = t + dtdy * (flip ? (lrx - ulx) : (lry - uly));
    applyState(st, 1, tile);
    if (!flip) {
        screenVtx(&q[0], ulx, uly, 0, s, t, 0, 0, 0, 0);
        screenVtx(&q[1], lrx, uly, 0, s1, t, 0, 0, 0, 0);
        screenVtx(&q[2], ulx, lry, 0, s, t1, 0, 0, 0, 0);
        screenVtx(&q[5], lrx, lry, 0, s1, t1, 0, 0, 0, 0);
    } else {
        screenVtx(&q[0], ulx, uly, 0, s, t, 0, 0, 0, 0);
        screenVtx(&q[1], lrx, uly, 0, s, t1, 0, 0, 0, 0);
        screenVtx(&q[2], ulx, lry, 0, s1, t, 0, 0, 0, 0);
        screenVtx(&q[5], lrx, lry, 0, s1, t1, 0, 0, 0, 0);
    }
    q[3] = q[2];
    q[4] = q[1];
    draw(q, 6);
}

void gfxBackendPresent(u32 fb, int black) {
    int w, h;
    SDL_GL_GetDrawableSize(sWindow, &w, &h);
    gfxFbPresent(fb, black, w, h);
    if (sCaptureReq && w > 0 && h > 0) {
        size_t need = (size_t)w * (size_t)h * 4;
        if (sCaptureCap < need) {
            u8 *px = realloc(sCapturePx, need);
            if (px == NULL) {
                sCaptureReq = 0;
                return;
            }
            sCapturePx = px;
            sCaptureCap = need;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, sCapturePx);
        sCaptureW = w;
        sCaptureH = h;
        sCaptureReq = 0;
        sCaptureSeq++;
    }
}

/* debug pixel probe at a point of the canvas (fractions of its drawable, top-left origin); 0 when the point is off
 * the presented image. The result (port_debug_probe_result) fills once the next gfx task ran. */
EMSCRIPTEN_KEEPALIVE int port_debug_probe(float fx, float fy) {
    float x, y;
    if (!gfxFbPresentedToN64(fx, fy, &x, &y)) {
        return 0;
    }
    gfxProbeArm(x, y);
    return 1;
}

EMSCRIPTEN_KEEPALIVE const char *port_debug_probe_result(void) {
    return gfxProbeResult();
}

EMSCRIPTEN_KEEPALIVE void port_debug_capture_request(void) {
    sCaptureReq = 1;
}

EMSCRIPTEN_KEEPALIVE u32 port_debug_capture_seq(void) {
    return sCaptureSeq;
}

EMSCRIPTEN_KEEPALIVE u8 *port_debug_capture_pixels(void) {
    return sCapturePx;
}

EMSCRIPTEN_KEEPALIVE int port_debug_capture_width(void) {
    return sCaptureW;
}

EMSCRIPTEN_KEEPALIVE int port_debug_capture_height(void) {
    return sCaptureH;
}

void gfxBackendCpuWrote(u32 addr, u32 len) {
    gfxFbCpuWrote(addr, len);
}

void gfxBackendSyncRdram(u32 addr, u32 len) {
    gfxFbSyncRdram(addr, len);
}

int portGfxWideSpanX(f32 *left, f32 *right) {
    float half = gfxFbPhysWidth(gPortViWidth) / (float)gGfxScale * 0.5f, mid = gPortViWidth * 0.5f;
    if (half <= mid) {
        return 0;
    }
    *left = mid - half;
    *right = mid + half;
    return 1;
}

f32 portGfxWideHalfYaw(f32 halfYawDeg, f32 widen) {
    if (widen <= 1.0f || halfYawDeg >= 89.0f) {
        return halfYawDeg;
    }
    /* tan form keeps the widened angle below 90 degrees */
    return atanf(tanf(halfYawDeg * (float)M_PI / 180.0f) * widen) * 180.0f / (float)M_PI;
}

void portGfxWideClipX(s16 *left, s16 *right) {
    s32 w = (s32)gPortViWidth;
    f32 l, r;
    if (!portGfxWideSpanX(&l, &r)) {
        return;
    }
    /* same overscan tolerance as spansWidth(): an edge within width/16 of the image edge is the image edge */
    if (*left <= w / 16 && floorf(l) - 1.0f < *left) {
        *left = (s16)(floorf(l) - 1.0f);
    }
    if (*right >= w - w / 16 && ceilf(r) + 1.0f > *right) {
        *right = (s16)(ceilf(r) + 1.0f);
    }
}
