/* WebGL2 backend: SDL window + GLES 3.0 context, texture cache, RDP state -> GL state, draws. */
#include "gfx/gfx_gl.h"
#include "gfx/gfx_texpack.h"
#include <SDL2/SDL.h>
#include <emscripten.h>
#include <math.h>
#include <stdlib.h>

#define TEX_CACHE_SIZE 4096
#define TEX_INDEX_SIZE (2 * TEX_CACHE_SIZE)
#define WIN_W 960
#define WIN_H 720

typedef struct {
    u64 key;
    GLuint tex;
    u32 w, h;
    u32 sampling; /* setSampling's packed parameters last set on tex, 0 unknown */
    s32 pack;     /* gfxTexpackLookup's pack entry replacing this texture, -1 none */
} TexEntry;

static SDL_Window *sWindow;
static SDL_GLContext sContext;
static GLuint sVao, sVbo;
static TexEntry sTexCache[TEX_CACHE_SIZE];
static int sTexCount;
static u16 sTexIndex[TEX_INDEX_SIZE]; /* texCacheFind's hash index; a quickload's texCacheFlush clears it */
static GLuint sWhiteTex;
static GfxFb *sTarget;
static int sTargetIsDepth;
static u32 sTaskSerial;
static u8 *sMipChain; /* decoded mip levels staged for upload (bindMipTiles) */
static size_t sMipChainCap;

/* GL state shadow for the draw path: every WebGL call crosses into JS and Chrome's validation, so applyState and
 * draw skip calls that change nothing. All-ones means unknown; glsReset() follows anything else that touches this
 * state (gfx_fb.c helpers, presents, readbacks, task boundaries). */
static struct {
    GLuint prog, vao, vbo, tex[2];
    int unit;
    s8 blend, blendFunc, depthTest, depthMask, polyOffset, scissorTest;
    GLenum depthFunc;
    GLint scissor[4];
} sGls;
static u32 sWhiteSampling;

static void glsReset(void) {
    memset(&sGls, 0xFF, sizeof sGls);
    sWhiteSampling = 0;
}

static void glsUseProgram(GLuint p) {
    if (sGls.prog != p) {
        glUseProgram(p);
        sGls.prog = p;
    }
}

/* the active unit only matters to calls that act on a unit's bound texture: binds, uploads, parameters */
static void glsUnit(int unit) {
    if (sGls.unit != unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        sGls.unit = unit;
    }
}

static void glsBindTex(int unit, GLuint tex) {
    if (sGls.tex[unit] != tex) {
        glsUnit(unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        sGls.tex[unit] = tex;
    }
}

static void glsCap(GLenum cap, s8 *cached, int on) {
    if (*cached != on) {
        if (on) {
            glEnable(cap);
        } else {
            glDisable(cap);
        }
        *cached = (s8)on;
    }
}

static void glsDepthFunc(GLenum f) {
    if (sGls.depthFunc != f) {
        glDepthFunc(f);
        sGls.depthFunc = f;
    }
}

static void glsDepthMask(int on) {
    if (sGls.depthMask != on) {
        glDepthMask(on ? GL_TRUE : GL_FALSE);
        sGls.depthMask = (s8)on;
    }
}

static void glsScissor(GLint x, GLint y, GLint w, GLint h) {
    glsCap(GL_SCISSOR_TEST, &sGls.scissorTest, 1);
    if (sGls.scissor[0] != x || sGls.scissor[1] != y || sGls.scissor[2] != w || sGls.scissor[3] != h) {
        glScissor(x, y, w, h);
        sGls.scissor[0] = x;
        sGls.scissor[1] = y;
        sGls.scissor[2] = w;
        sGls.scissor[3] = h;
    }
}

static void glsBindGeometry(void) {
    if (sGls.vao != sVao) {
        glBindVertexArray(sVao);
        sGls.vao = sVao;
    }
    if (sGls.vbo != sVbo) {
        glBindBuffer(GL_ARRAY_BUFFER, sVbo);
        sGls.vbo = sVbo;
    }
}

/* per-program uniform shadows (GfxProgram.c*): uniforms are program state, so they survive glUseProgram */
static void glsU4(GLint loc, float *c, float a, float b, float d, float e) {
    if (c[0] != a || c[1] != b || c[2] != d || c[3] != e) {
        glUniform4f(loc, a, b, d, e);
        c[0] = a;
        c[1] = b;
        c[2] = d;
        c[3] = e;
    }
}

static void glsU2(GLint loc, float *c, const float *v) {
    if (c[0] != v[0] || c[1] != v[1]) {
        glUniform2fv(loc, 1, v);
        c[0] = v[0];
        c[1] = v[1];
    }
}

static void glsU1(GLint loc, float *c, float v) {
    if (*c != v) {
        glUniform1f(loc, v);
        *c = v;
    }
}

/* frame grab for web/src/game/debug/api.ts: requested from JS, filled after the next present */
static int sCaptureReq, sCaptureW, sCaptureH;
static u32 sCaptureSeq;
static u8 *sCapturePx;
static size_t sCaptureCap;

/* frame interpolation (gfx.c): during a replay VI images draw into the in-between image and offscreen images are
 * skipped; each replay's image is then kept in a slot until gfx.c presents it */
static int sInterpLevel = 1; /* player setting (?interp, Esc menu): a keep range, so a quickload leaves it as set */
static struct {
    int pass;  /* a replay is drawing */
    int drawn; /* it drew a VI image */
} sInterpGl;

void gfxBackendQsRanges(QsSink *out) {
    QS_KEEP(out, sWindow);
    QS_KEEP(out, sContext);
    QS_KEEP(out, sVao);
    QS_KEEP(out, sVbo);
    QS_KEEP(out, sTexCache);
    QS_KEEP(out, sTexCount);
    QS_KEEP(out, sWhiteTex);
    QS_KEEP(out, sTarget);
    QS_KEEP(out, sTargetIsDepth);
    QS_KEEP(out, sTaskSerial);
    QS_KEEP(out, sCaptureReq);
    QS_KEEP(out, sCaptureW);
    QS_KEEP(out, sCaptureH);
    QS_KEEP(out, sCaptureSeq);
    QS_KEEP(out, sCapturePx);
    QS_KEEP(out, sCaptureCap);
    QS_KEEP(out, sMipChain);
    QS_KEEP(out, sMipChainCap);
    QS_KEEP(out, sInterpGl);
    QS_KEEP(out, sInterpLevel);
    gfxInterpQsRanges(out);
    gfxTexpackQsRanges(out);
    gfxFbQsRanges(out);
    gfxFbSnapQsRanges(out);
    gfxCombinerQsRanges(out);
}

static void texCacheFlush(void) {
    int i;
    for (i = 0; i < sTexCount; i++) {
        glDeleteTextures(1, &sTexCache[i].tex);
    }
    sTexCount = 0;
    memset(sTexIndex, 0, sizeof sTexIndex);
    sGls.tex[0] = sGls.tex[1] = ~0u; /* deleted textures unbind */
}

void gfxGlTexCacheFlush(void) {
    texCacheFlush();
}

void gfxGlStateReset(void) {
    glsReset();
}

/* RDRAM and TMEM were replaced: cached textures and framebuffer images no longer match them */
void gfxBackendQsRestored(void) {
    texCacheFlush();
    sTarget = NULL;
    glsReset();
    sTargetIsDepth = 0;
    gfxInterpReset();
    gfxFbQsRestored();
    gfxFbSnapReset(); /* the RDRAM the GPU copies stood for is gone; the effects fall back to its texels */
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
    glsReset();
}

void gfxBackendBeginTask(void) {
    int w, h;
    SDL_GL_GetDrawableSize(sWindow, &w, &h);
    gfxFbSetWindowSize(w, h);
    sTaskSerial++;
    gfxFbSnapBeginTask();
    sTarget = NULL;
    gGfxTargetWide = 1.0f;
    sTargetIsDepth = 0;
    glsReset(); /* presents and readbacks since the last task changed GL state behind the shadow */
}

void gfxBackendEndTask(void) {
    gfxFbDepthPrefetch();
    glBindVertexArray(sVao);
}

/* black over a widened target's columns outside [left, right) (physical pixels) */
static void clearSides(GfxFb *fb, u32 left, u32 right) {
    u32 sh = fb->physH;
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glScissor(0, 0, left, sh);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(right, 0, fb->physW - right, sh);
    glClear(GL_COLOR_BUFFER_BIT);
    fb->resolveDirty = 1;
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
    if (sInterpGl.pass) {
        /* VI images draw into the in-between image; offscreen images keep what the first pass drew */
        GfxFb *real = width == gPortViWidth ? gfxFbFind(addr) : NULL;
        sTarget = real != NULL ? gfxFbInterpFor(real) : NULL;
        if (sTarget == NULL) {
            gGfxTargetWide = 1.0f;
            return;
        }
        sInterpGl.drawn = 1;
    } else {
        sTarget = gfxFbFor(addr, width);
    }
    gfxFbBind(sTarget);
    gGfxTargetWide = sTarget->physW / (float)(sTarget->width * sTarget->scale);
    if (sTarget->ox > 0 && sTarget->sideTask != sTaskSerial) {
        /* the game only covers its 4:3 image; blank the side areas once per task so frames that draw no
         * full-width clear don't show stale pixels there */
        sTarget->sideTask = sTaskSerial;
        clearSides(sTarget, sTarget->ox, sTarget->ox + sTarget->width * sTarget->scale);
    }
    glsReset(); /* gfxFbFor/gfxFbBind may have uploaded or copied images with their own GL state */
}

/* the widened scissor lets the screen wipe's 4:3 capture (and tiles it draws past its edge) reach the overscan
 * border and the side areas, beside live scene: black everything left and right of the game's scissor, as the N64
 * shows it */
void gfxBackendBlankSides(const GfxState *st) {
    s32 x0 = st->scissor[0], x1 = st->scissor[2], w;
    if (sTarget == NULL || sTarget->ox == 0) {
        return;
    }
    w = (s32)sTarget->width;
    x0 = x0 < 0 ? 0 : x0 > w ? w : x0;
    x1 = x1 < x0 ? x0 : x1 > w ? w : x1;
    gfxFbBind(sTarget);
    clearSides(sTarget, sTarget->ox + (u32)x0 * sTarget->scale, sTarget->ox + (u32)x1 * sTarget->scale);
    gfxFbDrew(sTarget);
    glsReset();
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

/* texture cache lookup: open addressing over sTexCache, entry index + 1 (0 empty); rebuilt empty by a flush */
static u32 texCacheSlot(u64 key, u32 w, u32 h) {
    u64 x = key ^ ((u64)w << 40) ^ ((u64)h << 20);
    x ^= x >> 31;
    return (u32)(x * 0x9E3779B97F4A7C15ull >> 40) & (TEX_INDEX_SIZE - 1);
}

/* cached GL texture for (key, w, h), or NULL */
static TexEntry *texCacheFind(u64 key, u32 w, u32 h) {
    u32 s;
    for (s = texCacheSlot(key, w, h); sTexIndex[s] != 0; s = (s + 1) & (TEX_INDEX_SIZE - 1)) {
        TexEntry *e = &sTexCache[sTexIndex[s] - 1];
        if (e->key == key && e->w == w && e->h == h) {
            return e;
        }
    }
    return NULL;
}

/* new cached GL texture, bound to `unit`; the caller uploads its levels */
static TexEntry *texCacheAdd(u64 key, u32 w, u32 h, int unit) {
    TexEntry *e;
    GLuint tex;
    u32 s;
    if (sTexCount == TEX_CACHE_SIZE) {
        texCacheFlush();
    }
    glGenTextures(1, &tex);
    glsBindTex(unit, tex);
    e = &sTexCache[sTexCount++];
    e->key = key;
    e->tex = tex;
    e->w = w;
    e->h = h;
    e->sampling = 0;
    e->pack = -1;
    for (s = texCacheSlot(key, w, h); sTexIndex[s] != 0; s = (s + 1) & (TEX_INDEX_SIZE - 1)) {
    }
    sTexIndex[s] = (u16)sTexCount;
    return e;
}

/* wrap and filter of the texture bound to `unit`; `cache` holds what was last set on it (0 unknown). mipmapped: 0 none,
 * 1 an N64 mip chain (bindMipTiles), 2 a pack replacement's GL mips */
static void setSampling(const GfxState *st, const GfxTile *t, int unit, int mipmapped, u32 *cache) {
    int linear = (st->otherH & (3u << G_MDSFT_TEXTFILT)) != G_TF_POINT;
    u32 packed = 1u | (u32)(t->cms & 3) << 1 | (u32)(t->cmt & 3) << 3 | (u32)linear << 5 | (u32)mipmapped << 6;
    GLint minFilter = linear ? GL_LINEAR : GL_NEAREST;
    if (*cache == packed) {
        return;
    }
    *cache = packed;
    if (mipmapped == 1) {
        minFilter = linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    } else if (mipmapped == 2) {
        minFilter = linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
    }
    glsUnit(unit);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode(t->cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode(t->cmt));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
}

/* tile's cached texture, decoded and uploaded (bound to `unit`) on a miss, which is also when packs are looked up
 * and the dump sees it; NULL when the tile is empty. img gets the size and key. */
static TexEntry *tileEntry(const GfxState *st, int tile, int unit, GfxTexImage *img) {
    TexEntry *e;
    if (!gfxTmemKey(st, tile & 7, img)) {
        return NULL;
    }
    e = texCacheFind(img->key, img->width, img->height);
    if (e == NULL && gfxTmemDecode(st, tile & 7, img)) {
        e = texCacheAdd(img->key, img->width, img->height, unit);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img->width, img->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->rgba);
        e->pack = gfxTexpackLookup(st, tile & 7, img);
    }
    return e;
}

/* a loaded pack texture replaces tile (texture LOD then draws it alone, with its own GL mips) */
static int tileReplaced(const GfxState *st, int tile) {
    GfxTexImage img;
    TexEntry *e;
    if (!gfxTexpackActive()) {
        return 0;
    }
    e = tileEntry(st, tile, 0, &img);
    return e != NULL && gfxTexpackTexture(e->pack) != NULL;
}

static void bindTile(const GfxState *st, int tile, int unit, float *tileParams, float *size) {
    const GfxTile *t = &st->tiles[tile & 7];
    GfxTexImage img;
    TexEntry *e;
    GfxPackTex *r;
    tileParamsFor(t, tileParams);
    size[0] = 1.0f;
    size[1] = 1.0f;
    {
        /* a tile of an RDRAM copy of a colour image: draw it from the GPU copy of that image (issue #35), with
         * the tile's texture coordinates moved to its place in the copy. The copy is sampled bilinearly at render
         * scale; the tile's own wrap and filter would only describe its 320x240 texels. */
        GfxTileSource ts;
        float origin[2];
        GLuint snap;
        if (gfxTmemTileSource(st, tile, &ts) && (snap = gfxFbSnapTile(&ts, origin, size)) != 0) {
            glsBindTex(unit, snap);
            tileParams[2] -= origin[0];
            tileParams[3] -= origin[1];
            return;
        }
    }
    e = tileEntry(st, tile, unit, &img);
    if (e == NULL) {
        glsBindTex(unit, sWhiteTex);
        setSampling(st, t, unit, 0, &sWhiteSampling);
        return;
    }
    /* the N64 size, whatever a replacement's: the shader's normalised UVs map an image of any size */
    size[0] = (float)img.width;
    size[1] = (float)img.height;
    r = gfxTexpackTexture(e->pack);
    if (r == NULL) {
        glsBindTex(unit, e->tex);
        setSampling(st, t, unit, 0, &e->sampling);
        return;
    }
    glsBindTex(unit, r->tex);
    if (r->shiftHalf) {
        /* RT64's shift: half an original texel */
        tileParams[2] -= 0.5f;
        tileParams[3] -= 0.5f;
    }
    setSampling(st, t, unit, r->mipmapped ? 2 : 0, &r->sampling);
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
    TexEntry *e;
    for (k = 0; k <= maxLevel && k < 8; k++) {
        if (!gfxTmemKey(st, (tile + k) & 7, &img)) {
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
        key = (key ^ img.key) * 1099511628211ull;
        n = k;
    }
    if (n < 1) {
        return 0;
    }
    key = (key ^ (u64)n) * 1099511628211ull;
    e = texCacheFind(key, w0, h0);
    if (e == NULL) {
        size_t need = off[n] + (size_t)w[n] * h[n] * 4;
        if (need > sMipChainCap) {
            u8 *p = realloc(sMipChain, need * 2);
            if (p == NULL) {
                return 0;
            }
            sMipChain = p;
            sMipChainCap = need * 2;
        }
        for (k = 0; k <= n; k++) {
            gfxTmemDecode(st, (tile + k) & 7, &img);
            memcpy(sMipChain + off[k], img.rgba, (size_t)w[k] * h[k] * 4);
        }
        e = texCacheAdd(key, w0, h0, 0);
        for (k = 0; k <= n; k++) {
            glTexImage2D(GL_TEXTURE_2D, k, GL_RGBA8, w[k], h[k], 0, GL_RGBA, GL_UNSIGNED_BYTE, sMipChain + off[k]);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n);
    }
    glsBindTex(0, e->tex);
    setSampling(st, &st->tiles[tile & 7], 0, 1, &e->sampling);
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
    glsScissor(px0, (h - y1) * s, px1 - px0, (y1 - y0) * s);
}

static void applyState(const GfxState *st, int forRect, int tile) {
    GfxProgramKey key;
    GfxProgram *p;
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
    if (!forRect && st->texOn && st->texLevels > 0 && (st->otherH & G_TL_LOD) && cyc == 1 && !tileReplaced(st, t0)) {
        maxLevel = bindMipTiles(st, t0, st->texLevels, tile0, size0, mipTile, mipSize);
    }
    key.lod = maxLevel > 0;
    p = gfxProgramGet(&key);
    if (gGfxProgramBuilt) {
        gGfxProgramBuilt = 0;
        sGls.prog = ~0u; /* building made the new program current */
    }
    glsUseProgram(p->prog);
    glsU4(p->uPrim, p->cPrim, st->prim[0] / 255.0f, st->prim[1] / 255.0f, st->prim[2] / 255.0f, st->prim[3] / 255.0f);
    glsU4(p->uEnv, p->cEnv, st->env[0] / 255.0f, st->env[1] / 255.0f, st->env[2] / 255.0f, st->env[3] / 255.0f);
    glsU4(p->uFog, p->cFog, st->fog[0] / 255.0f, st->fog[1] / 255.0f, st->fog[2] / 255.0f, st->fog[3] / 255.0f);
    glsU4(p->uBlend, p->cBlend, st->blend[0] / 255.0f, st->blend[1] / 255.0f, st->blend[2] / 255.0f,
          st->blend[3] / 255.0f);
    glsU1(p->uPrimLod, &p->cPrimLod, st->primLodFrac / 255.0f);
    glsU1(p->uAlphaRef, &p->cAlphaRef, st->blend[3] / 255.0f);
    glsU1(p->uPrimDepth, &p->cPrimDepth, (st->primDepth & 0x7FFF) / 32767.0f);
    if (key.lod) {
        /* both texels come from the mip chain on unit 0; LOD counts tile0 texels per N64 pixel */
        glsU1(p->uLodScale, &p->cLodScale, (float)sTarget->scale);
        glsU1(p->uMaxLevel, &p->cMaxLevel, (float)maxLevel);
        glUniform4fv(p->uMipTile, maxLevel + 1, &mipTile[0][0]);
        glUniform2fv(p->uMipSize, maxLevel + 1, &mipSize[0][0]);
    } else if (p->usesTex0 && (st->texOn || forRect)) {
        bindTile(st, t0, 0, tile0, size0);
    } else {
        tile0[0] = tile0[1] = 1.0f;
        tile0[2] = tile0[3] = 0.0f;
        size0[0] = size0[1] = 1.0f;
        glsBindTex(0, sWhiteTex);
    }
    if (p->usesTex1 && !key.lod && (st->texOn || forRect)) {
        bindTile(st, (t0 + 1) & 7, 1, tile1, size1);
    } else {
        tile1[0] = tile1[1] = 1.0f;
        tile1[2] = tile1[3] = 0.0f;
        size1[0] = size1[1] = 1.0f;
        glsBindTex(1, sWhiteTex);
    }
    glsU4(p->uTile0, p->cTile0, tile0[0], tile0[1], tile0[2], tile0[3]);
    glsU4(p->uTile1, p->cTile1, tile1[0], tile1[1], tile1[2], tile1[3]);
    glsU2(p->uSize0, p->cSize0, size0);
    glsU2(p->uSize1, p->cSize1, size1);

    /* blending: IN*A_IN + MEM*(1-A) under FORCE_BL */
    if ((om & FORCE_BL) && bp == G_BL_CLR_IN && ba == G_BL_A_IN && bm == G_BL_CLR_MEM && bb == G_BL_1MA && cyc < 2) {
        glsCap(GL_BLEND, &sGls.blend, 1);
        if (sGls.blendFunc != 1) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            sGls.blendFunc = 1;
        }
    } else {
        glsCap(GL_BLEND, &sGls.blend, 0);
    }
    /* depth */
    if (cyc >= 2) {
        glsCap(GL_DEPTH_TEST, &sGls.depthTest, 0);
        glsDepthMask(0);
    } else {
        int zcmp = (om & Z_CMP) && (st->geometry & G_ZBUFFER || forRect);
        int zupd = (om & Z_UPD) != 0;
        if (zcmp || zupd) {
            /* RDP z compare on a fully covered pixel: opaque, interpenetrating and translucent modes pass only when
             * strictly nearer (a coplanar redraw fails: Galleon's gold stacks draw a white untextured copy of their
             * textured tops after them); decal passes on equal z (polygon offset below) */
            glsCap(GL_DEPTH_TEST, &sGls.depthTest, 1);
            glsDepthFunc(!zcmp ? GL_ALWAYS : (om & ZMODE_DEC) == ZMODE_DEC ? GL_LEQUAL : GL_LESS);
        } else {
            glsCap(GL_DEPTH_TEST, &sGls.depthTest, 0);
        }
        glsDepthMask(zupd);
    }
    if ((om & ZMODE_DEC) == ZMODE_DEC) {
        /* the offset itself never changes; set with the enable */
        if (sGls.polyOffset != 1) {
            glPolygonOffset(-2.0f, -2.0f);
        }
        glsCap(GL_POLYGON_OFFSET_FILL, &sGls.polyOffset, 1);
    } else {
        glsCap(GL_POLYGON_OFFSET_FILL, &sGls.polyOffset, 0);
    }
    /* scissor (N64 pixels, y down) */
    if (sTarget != NULL) {
        s32 x0 = st->scissor[0], y0 = st->scissor[1], x1 = st->scissor[2], y1 = st->scissor[3];
        if (x1 > x0 && y1 > y0) {
            scissorN64(x0, y0, x1, y1);
        } else {
            glsCap(GL_SCISSOR_TEST, &sGls.scissorTest, 0);
        }
    }
}

static void draw(const GfxVtx *v, u32 count) {
    glsBindGeometry();
    glBufferData(GL_ARRAY_BUFFER, count * sizeof(GfxVtx), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, count);
    if (sTarget != NULL) {
        gfxFbDrew(sTarget);
        if (!sInterpGl.pass) { /* the in-between image has its own depth */
            gGfxDepthDirty = 1;
            gGfxDepthSerial++;
        }
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
            if (sInterpGl.pass) {
                gfxFbInterpClearDepth();
            } else {
                gfxFbClearDepth();
            }
            glsReset();
        }
        return;
    }
    if (sTarget == NULL) {
        return;
    }
    if (cyc == 3) {
        u16 c = (u16)(st->fillColor >> 16);
        gfxFbBind(sTarget);
        glsReset();
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

/* web/src/game/debug/api.ts frame grab: the canvas as just presented */
static void captureIfRequested(int w, int h) {
    size_t need;
    if (!sCaptureReq || w <= 0 || h <= 0) {
        return;
    }
    need = (size_t)w * (size_t)h * 4;
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

void gfxBackendPresent(u32 fb, int black) {
    int w, h;
    SDL_GL_GetDrawableSize(sWindow, &w, &h);
    gfxFbPresent(fb, black, w, h);
    captureIfRequested(w, h);
}

int gfxBackendInterpLevel(void) {
    return sInterpLevel;
}

int gfxBackendSetInterp(int level) {
    if (level >= 0 && level <= 3) {
        sInterpLevel = level;
    }
    return sInterpLevel;
}

void gfxBackendInterpBegin(void) {
    gfxFbDepthCollect(); /* before the replay's commands queue up behind the readback */
    sInterpGl.pass = 1;
    sInterpGl.drawn = 0;
    glsReset();
    sTarget = NULL;
    sTargetIsDepth = 0;
    gGfxTargetWide = 1.0f;
}

int gfxBackendInterpEnd(int slot) {
    sInterpGl.pass = 0;
    if (sInterpGl.drawn) {
        gfxFbInterpStore(slot);
    }
    sTarget = NULL;
    sTargetIsDepth = 0;
    gGfxTargetWide = 1.0f;
    glsReset(); /* the slot copy changed framebuffer bindings and scissor */
    glBindVertexArray(sVao);
    return sInterpGl.drawn;
}

int gfxBackendPresentInterp(int slot) {
    int w, h;
    SDL_GL_GetDrawableSize(sWindow, &w, &h);
    if (!gfxFbPresentInterp(slot, w, h)) {
        return 0;
    }
    captureIfRequested(w, h);
    return 1;
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
    if (gfxFbSnapCpuWrote(addr, len)) {
        glsReset(); /* an alpha refresh of a GPU copy drew with its own state */
    }
}

/* the GPU-side framebuffer effects (issue #35): the game told the port what an RDRAM buffer it just filled is a
 * copy of, so the port can keep the same pixels at render scale beside it */
void gfxBackendFbSnapshot(u32 dst, u32 src, u32 len) {
    gfxFbSnapOfImage(dst, src, len);
    glsReset();
}

void gfxBackendFbSepia(u32 dst, u32 src, u32 x, u32 y, u32 cols, u32 rows, u32 tileW, u32 tileH) {
    gfxFbSnapSepia(dst, src, x, y, cols, rows, tileW, tileH);
    glsReset();
}

void gfxBackendFbLerpWhite(u32 dst, u32 src, u32 len, float t) {
    gfxFbSnapLerpWhite(dst, src, len, t);
    glsReset();
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
