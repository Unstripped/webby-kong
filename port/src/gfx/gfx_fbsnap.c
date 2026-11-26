/* GPU copies of colour images the game copied into RDRAM (issue #35).
 *
 * A few effects build their image on the CPU out of a framebuffer the port had to sync back to RDRAM at the N64
 * size: the screen transition's and the screen wipe's freeze frame (fbCopySetAlpha) and the camera's sepia photo
 * (playerViewCaptureSepiaPhoto). The game then draws that RDRAM image back as 16-bit texture tiles, so the result
 * is a 320x240 image however high the render scale is.
 *
 * This module keeps, beside the RDRAM copy the game made, a GPU copy of the same pixels at render scale. A tile
 * whose texels come straight out of such a range (gfxTmemTileSource) is drawn from the GPU copy instead, with the
 * tile's texture coordinates remapped into it. The RDRAM copy is never touched: game code reads and writes exactly
 * the bytes it would without this, and a copy is used only while the RDRAM bytes still hash to what the GPU copy
 * was taken from (checked once per gfx task), so a freed and reused buffer or any write the port did not see
 * falls back to the texels in RDRAM.
 *
 * Alpha is the one thing the CPU does change: the copy's bit 0 is the coverage/alpha bit, and the transition masks
 * a six-pixel column strip out of it every frame. Every write the port is told about refreshes the GPU copy's
 * alpha channel from RDRAM, so the tear line looks as it does without the GPU copies. */
#include "gfx/gfx_gl.h"

#define SNAP_SLOTS 4

enum { SNAP_FREE, SNAP_IMAGE, SNAP_TILES };

typedef struct {
    u32 kind;
    u32 addr, len;          /* the RDRAM copy */
    u32 width, height;      /* its N64 size: what the tile coordinates are in */
    u32 tileW, tileH, cols; /* SNAP_TILES: the tile grid inside the range */
    GLuint tex, fbo;
    u32 texW, texH;
    u64 hash;      /* the RDRAM bytes this copy stands for */
    u64 colorHash; /* the same without the coverage bits */
    u32 checkedTask; /* task the hash was last checked in */
    u32 used;        /* last task a tile came from it: the oldest slot is reused */
} FbSnap;

static FbSnap sSnaps[SNAP_SLOTS];
static u32 sSnapTask = 1;
static GLuint sSnapAlphaProg, sSnapSepiaProg, sSnapLerpProg, sSnapAlphaTex;
static u8 *sSnapAlpha; /* RDRAM alpha staging, one byte per N64 pixel */

static const char *kAlphaFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n" /* one byte per N64 pixel: the copy's bit 0 */
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = vec4(0.0, 0.0, 0.0, texture(uTex, vUv).r); }\n";
/* color5551ToSepia (decomp/src/global/103AB0_view_effects.c) on the rendered pixel: the same arithmetic on the
 * 5-bit components the CPU would have read, so a pixel of the high-res image turns the colour the N64-sized copy
 * would have given it */
static const char *kSepiaFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uSrc;\n" /* source rect, normalised */
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec4 c = texture(uTex, uSrc.xy + vUv * uSrc.zw);\n"
    "    vec3 c5 = floor(floor(c.rgb * 255.0 + 0.5) / 8.0);\n" /* the 5 bits gfxFbSyncRdram would have written */
    "    float sum = c5.r + c5.g + c5.b;\n"
    "    float a = sum >= 48.0 ? floor((sum - 48.0) / 45.0 * 18.0) : 0.0;\n"
    "    float f = sum / 93.0;\n"
    "    fragColor = vec4(floor(28.0 * f + 3.0) / 31.0, floor(19.0 * f + 3.0) / 31.0, a / 31.0, c.a);\n"
    "}\n";
/* color5551LerpToWhite (same file): every 5-bit component towards 31 by t */
static const char *kLerpFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uT;\n"
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec4 c = texture(uTex, vUv);\n"
    "    vec3 c5 = floor(floor(c.rgb * 255.0 + 0.5) / 8.0);\n"
    "    fragColor = vec4(floor((c5 - 31.0) * uT + 31.0) / 31.0, c.a);\n"
    "}\n";

/* the bytes a copy stands for, and the same without the coverage bits: the game may set or clear a pixel's
 * coverage bit (the transition's mask strip) without changing what the pixel looks like, and then only the
 * copy's alpha has to follow. Both hashes come out of one pass over the range. */
static void hashRange(u32 addr, u32 len, u64 *full, u64 *colors) {
    const u8 *p = (const u8 *)addr;
    u64 h = 1469598103934665603ull, c = 1469598103934665603ull;
    u32 i;
    for (i = 0; i + 1 < len; i += 2) {
        h = (h ^ p[i]) * 1099511628211ull;
        h = (h ^ p[i + 1]) * 1099511628211ull;
        c = (c ^ p[i]) * 1099511628211ull;
        c = (c ^ (u8)(p[i + 1] & 0xFE)) * 1099511628211ull; /* big-endian RGBA5551: bit 0 is coverage */
    }
    *full = h;
    *colors = c;
}

static FbSnap *snapFind(u32 addr, u32 len) {
    int i;
    for (i = 0; i < SNAP_SLOTS; i++) {
        if (sSnaps[i].kind != SNAP_FREE && addr < sSnaps[i].addr + sSnaps[i].len && addr + len > sSnaps[i].addr) {
            return &sSnaps[i];
        }
    }
    return NULL;
}

/* the slot for a new copy: one that already covers this range, a free one, or the one unused longest */
static FbSnap *snapSlot(u32 addr, u32 len) {
    FbSnap *s = snapFind(addr, len), *old;
    int i;
    if (s != NULL) {
        return s;
    }
    for (i = 0; i < SNAP_SLOTS; i++) {
        if (sSnaps[i].kind == SNAP_FREE) {
            return &sSnaps[i];
        }
    }
    old = &sSnaps[0];
    for (i = 1; i < SNAP_SLOTS; i++) {
        if (sSnaps[i].used < old->used) {
            old = &sSnaps[i];
        }
    }
    return old;
}

static void snapTarget(FbSnap *s, u32 w, u32 h) {
    if (s->tex == 0) {
        glGenTextures(1, &s->tex);
        glGenFramebuffers(1, &s->fbo);
    }
    if (s->texW != w || s->texH != h) {
        s->texW = w;
        s->texH = h;
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    }
}

/* draw the quad into `s` with `prog`; the caller sets the program's own uniforms afterwards through drawQuad */
static void snapPass(const FbSnap *s, GLuint prog) {
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glViewport(0, 0, s->texW, s->texH);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(gGfxQuadVao);
}

/* the copy's alpha from RDRAM bit 0, nearest-upscaled: the only part of the copy the game changes after it made
 * it (the transition's mask strip) */
static void snapRefreshAlpha(FbSnap *s) {
    const u8 *src = (const u8 *)s->addr;
    u32 w = s->width, h = s->height, x, y;
    if (sSnapAlpha == NULL) {
        sSnapAlpha = malloc(640 * 480);
        if (sSnapAlpha == NULL) {
            portAbort("PORT: framebuffer copy out of memory");
        }
    }
    if (w * h > 640 * 480) {
        return;
    }
    for (y = 0; y < h; y++) {
        u8 *o = &sSnapAlpha[y * w]; /* row 0 of the texture is the copy's top row, as the copy is top-down */
        for (x = 0; x < w; x++) {
            u32 px = s->kind == SNAP_TILES
                         ? ((y / s->tileH) * s->cols + x / s->tileW) * s->tileW * s->tileH +
                               (y % s->tileH) * s->tileW + x % s->tileW
                         : y * w + x;
            o[x] = (src[px * 2 + 1] & 1) ? 255 : 0;
        }
    }
    if (sSnapAlphaTex == 0) {
        glGenTextures(1, &sSnapAlphaTex);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sSnapAlphaTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, sSnapAlpha);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (sSnapAlphaProg == 0) {
        sSnapAlphaProg = gfxCompileProgram(gGfxQuadVs, kAlphaFs);
    }
    snapPass(s, sSnapAlphaProg);
    glBindTexture(GL_TEXTURE_2D, sSnapAlphaTex);
    glUniform1i(glGetUniformLocation(sSnapAlphaProg, "uTex"), 0);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* the slot keeps no image any more; its texture is the largest thing the port holds (11 MB at scale 6) */
static void snapRelease(FbSnap *s) {
    s->kind = SNAP_FREE;
    s->addr = 0;
    s->len = 0;
    if (s->tex != 0) {
        glDeleteTextures(1, &s->tex);
        glDeleteFramebuffers(1, &s->fbo);
        s->tex = 0;
        s->fbo = 0;
        s->texW = 0;
        s->texH = 0;
    }
}

static int snapValidate(FbSnap *s); /* defined with the other RDRAM checks below */

/* the colour image whose GPU pixels a copy of `src` stands for, or NULL when there is none to copy (never drawn,
 * or RDRAM is newer than it: the caller then leaves the effect on its RDRAM texels) */
static GfxFb *snapSource(u32 src, u32 width, u32 height) {
    GfxFb *fb = gfxFbFind(src);
    if (fb == NULL || !fb->everDrawn || fb->cpuDirty || fb->width != width || fb->height < height) {
        return NULL;
    }
    gfxFbResolve(fb);
    return fb;
}

void gfxFbSnapOfImage(u32 dst, u32 src, u32 len) {
    GfxFb *fb;
    FbSnap *s;
    u32 w = gPortViWidth, h = w * 3 / 4;
    if (len != w * h * 2) {
        return;
    }
    fb = snapSource(src, w, h);
    s = snapSlot(dst, len);
    if (fb == NULL) {
        snapRelease(s); /* the copy is all RDRAM has: draw it as the game wrote it */
        return;
    }
    snapTarget(s, w * fb->scale, h * fb->scale);
    s->kind = SNAP_IMAGE;
    s->addr = dst;
    s->len = len;
    s->width = w;
    s->height = h;
    s->used = sSnapTask;
    /* the colour image's 4:3 part, flipped: the copy's rows run top-down */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->resolveFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->fbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(fb->ox, 0, fb->ox + w * fb->scale, h * fb->scale, 0, (GLint)s->texH, (GLint)s->texW, 0,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    snapRefreshAlpha(s);
    hashRange(dst, len, &s->hash, &s->colorHash);
    s->checkedTask = sSnapTask;
}

void gfxFbSnapSepia(u32 dst, u32 src, u32 x, u32 y, u32 cols, u32 rows, u32 tileW, u32 tileH) {
    u32 w = cols * tileW, h = rows * tileH, len = w * h * 2;
    u32 vw = gPortViWidth, vh = vw * 3 / 4;
    GfxFb *fb = snapSource(src, vw, vh);
    FbSnap *s = snapSlot(dst, len);
    if (fb == NULL || x + w > vw || y + h > vh) {
        snapRelease(s);
        return;
    }
    snapTarget(s, w * fb->scale, h * fb->scale);
    s->kind = SNAP_TILES;
    s->addr = dst;
    s->len = len;
    s->width = w;
    s->height = h;
    s->tileW = tileW;
    s->tileH = tileH;
    s->cols = cols;
    s->used = sSnapTask;
    if (sSnapSepiaProg == 0) {
        sSnapSepiaProg = gfxCompileProgram(gGfxQuadVs, kSepiaFs);
    }
    snapPass(s, sSnapSepiaProg);
    glBindTexture(GL_TEXTURE_2D, fb->color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(glGetUniformLocation(sSnapSepiaProg, "uTex"), 0);
    /* the region in the image's texture, bottom-up: vUv.y 0 is the region's top row */
    glUniform4f(glGetUniformLocation(sSnapSepiaProg, "uSrc"),
                (fb->ox + x * fb->scale) / (float)fb->physW, (fb->physH - y * fb->scale) / (float)fb->physH,
                w * fb->scale / (float)fb->physW, -(float)(h * fb->scale) / (float)fb->physH);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    snapRefreshAlpha(s);
    hashRange(dst, len, &s->hash, &s->colorHash);
    s->checkedTask = sSnapTask;
}

void gfxFbSnapLerpWhite(u32 dst, u32 src, u32 len, float t) {
    FbSnap *from = snapFind(src, 2), *s;
    FbSnap copy;
    if (from != NULL && from->addr == src && from->len == len && from->checkedTask != sSnapTask) {
        snapValidate(from);
    }
    if (from == NULL || from->kind == SNAP_FREE || from->addr != src || from->len != len) {
        s = snapFind(dst, 2);
        if (s != NULL) {
            snapRelease(s); /* nothing to fade from: the fade draws its own RDRAM texels */
        }
        return;
    }
    copy = *from;
    s = snapSlot(dst, from->len);
    if (s == from) {
        return; /* the game never fades a buffer into itself; nothing sane to do */
    }
    snapTarget(s, copy.texW, copy.texH);
    s->kind = copy.kind;
    s->addr = dst;
    s->len = copy.len;
    s->width = copy.width;
    s->height = copy.height;
    s->tileW = copy.tileW;
    s->tileH = copy.tileH;
    s->cols = copy.cols;
    s->used = sSnapTask;
    if (sSnapLerpProg == 0) {
        sSnapLerpProg = gfxCompileProgram(gGfxQuadVs, kLerpFs);
    }
    snapPass(s, sSnapLerpProg);
    glBindTexture(GL_TEXTURE_2D, copy.tex);
    glUniform1i(glGetUniformLocation(sSnapLerpProg, "uTex"), 0);
    glUniform1f(glGetUniformLocation(sSnapLerpProg, "uT"), t);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    snapRefreshAlpha(s);
    hashRange(dst, s->len, &s->hash, &s->colorHash);
    s->checkedTask = sSnapTask;
}

/* the copy still stands for what RDRAM holds? Nothing but the coverage bits may differ - that is the
 * transition's mask strip, which the copy's alpha then follows. A write that changes colours (the screen wipe's
 * blur builds its image that way) or anything else that took the buffer over leaves the copy behind, and the
 * effect draws its own RDRAM texels again. Returns 1 when a GL pass ran. */
static int snapValidate(FbSnap *s) {
    u64 full, colors;
    hashRange(s->addr, s->len, &full, &colors);
    s->checkedTask = sSnapTask;
    if (colors != s->colorHash) {
        snapRelease(s);
        return 0;
    }
    if (full == s->hash) {
        return 0;
    }
    snapRefreshAlpha(s);
    s->hash = full;
    return 1;
}

int gfxFbSnapCpuWrote(u32 addr, u32 len) {
    FbSnap *s = snapFind(addr, len);
    return s != NULL && snapValidate(s);
}

void gfxFbSnapBeginTask(void) {
    sSnapTask++;
}

GLuint gfxFbSnapTile(const GfxTileSource *src, float *origin, float *size) {
    FbSnap *s = snapFind(src->addr, 2);
    u32 off, x0, y0;
    if (s == NULL || src->addr < s->addr) {
        return 0;
    }
    off = (src->addr - s->addr) / 2;
    if (s->kind == SNAP_IMAGE) {
        if (src->stride != s->width) {
            return 0;
        }
        x0 = off % s->width;
        y0 = off / s->width;
    } else {
        u32 texels = s->tileW * s->tileH, tile = off / texels;
        if (off % texels != 0 || src->width != s->tileW || src->height != s->tileH || src->stride != s->tileW ||
            tile >= (s->width / s->tileW) * (s->height / s->tileH)) {
            return 0;
        }
        x0 = (tile % s->cols) * s->tileW;
        y0 = (tile / s->cols) * s->tileH;
    }
    if (x0 >= s->width || y0 >= s->height) {
        return 0;
    }
    /* a tile load's lr is inclusive, so the last column's rect reaches one texel past the image; the copy clamps
     * there, and the mesh's texture coordinates stay inside it */
    if (s->checkedTask != sSnapTask) {
        /* the bytes the copy stands for must still be in RDRAM: anything else wrote over them */
        snapValidate(s);
        if (s->kind == SNAP_FREE) {
            return 0;
        }
    }
    s->used = sSnapTask;
    origin[0] = (float)x0;
    origin[1] = (float)y0;
    size[0] = (float)s->width;
    size[1] = (float)s->height;
    return s->tex;
}

void gfxFbSnapReset(void) {
    int i;
    for (i = 0; i < SNAP_SLOTS; i++) {
        snapRelease(&sSnaps[i]);
    }
}

void gfxFbSnapQsRanges(QsSink *out) {
    QS_KEEP(out, sSnaps);
    QS_KEEP(out, sSnapTask);
    QS_KEEP(out, sSnapAlphaProg);
    QS_KEEP(out, sSnapSepiaProg);
    QS_KEEP(out, sSnapLerpProg);
    QS_KEEP(out, sSnapAlphaTex);
    QS_KEEP(out, sSnapAlpha);
}
