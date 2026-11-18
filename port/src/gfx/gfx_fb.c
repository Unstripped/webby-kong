/* Colour images as FBOs (RGBA8 at PORT_GFX_SCALE x) sharing one depth texture; RDRAM coherence:
 * CPU writes are uploaded before the next draw/present, CPU reads sync the GPU image back first.
 * RDRAM colour is big-endian RGBA5551; the RDRAM depth buffer is native u16 in N64 z format. */
#include "gfx/gfx_gl.h"

#define MAX_FBS 16

static GfxFb sFbs[MAX_FBS];
static int sFbCount;
GLuint gGfxDepthTex;
int gGfxDepthDirty;
u32 gGfxDepthW, gGfxDepthH;
static GLuint sDepthFbo;
static GLuint sStageTex, sStageFbo;
static GLuint sDepthReadProg, sDepthReadVao, sDepthReadTarget, sDepthReadFbo;
static u8 *sScratch;
static u32 sDepthPhysW, sDepthPhysH, sDepthOx;
static float sWinAspect = 4.0f / 3.0f;
static GLint sMaxTex;

static void ensureDepth(const GfxFb *fb) {
    if (gGfxDepthTex != 0 && gGfxDepthW == fb->width && gGfxDepthH == fb->height && sDepthPhysW == fb->physW) {
        return;
    }
    if (gGfxDepthTex == 0) {
        glGenTextures(1, &gGfxDepthTex);
        glGenFramebuffers(1, &sDepthFbo);
    }
    gGfxDepthW = fb->width;
    gGfxDepthH = fb->height;
    sDepthPhysW = fb->physW;
    sDepthPhysH = fb->physH;
    sDepthOx = fb->ox;
    glBindTexture(GL_TEXTURE_2D, gGfxDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, sDepthPhysW, sDepthPhysH, 0, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, sDepthFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gGfxDepthTex, 0);
    {
        GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
    }
    /* attach to same-size colour FBOs; detach from the rest so they stay complete for blits and readbacks */
    {
        int i;
        for (i = 0; i < sFbCount; i++) {
            int same = sFbs[i].physW == fb->physW && sFbs[i].physH == fb->physH;
            glBindFramebuffer(GL_FRAMEBUFFER, sFbs[i].fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, same ? gGfxDepthTex : 0, 0);
        }
    }
}

static const char *kQuadVs =
    "#version 300 es\n"
    "out vec2 vUv;\n"
    "void main() { vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)); vUv = p;"
    " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *kDepthReadFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform highp sampler2D uDepth;\n"
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    float d = texture(uDepth, vUv).r;\n"
    "    float v = floor(d * 16777215.0 + 0.5);\n"
    "    float r = floor(v / 65536.0); v -= r * 65536.0;\n"
    "    float g = floor(v / 256.0); float b = v - g * 256.0;\n"
    "    fragColor = vec4(r, g, b, 255.0) / 255.0;\n"
    "}\n";

void gfxFbInit(void) {
    glGenTextures(1, &sStageTex);
    glGenFramebuffers(1, &sStageFbo);
    sDepthReadProg = gfxCompileProgram(kQuadVs, kDepthReadFs);
    glGenVertexArrays(1, &sDepthReadVao);
    glGenTextures(1, &sDepthReadTarget);
    glGenFramebuffers(1, &sDepthReadFbo);
    sScratch = malloc(640 * 480 * PORT_GFX_SCALE * PORT_GFX_SCALE * 4);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &sMaxTex);
}

void gfxFbSetWindowAspect(float aspect) {
    sWinAspect = aspect;
}

/* VI-sized images widen to the window aspect (Hor+); offscreen images keep their N64 size */
u32 gfxFbPhysWidth(u32 width) {
    u32 base = width * PORT_GFX_SCALE, h = width * 3 / 4 * PORT_GFX_SCALE, w;
    if (width != gPortViWidth || sWinAspect <= 4.0f / 3.0f) {
        return base;
    }
    w = (u32)(h * sWinAspect + 0.5f);
    if (sMaxTex > 0 && w > (u32)sMaxTex) {
        w = (u32)sMaxTex;
    }
    if (w <= base) {
        return base;
    }
    return base + ((w - base) & ~1u); /* even margin keeps ox integral */
}

GfxFb *gfxFbFind(u32 addr) {
    int i;
    for (i = 0; i < sFbCount; i++) {
        if (sFbs[i].addr == addr) {
            return &sFbs[i];
        }
    }
    return NULL;
}

GfxFb *gfxFbFor(u32 addr, u32 width) {
    GfxFb *fb = gfxFbFind(addr);
    u32 height = width * 3 / 4;
    u32 physW = gfxFbPhysWidth(width);
    if (fb != NULL && fb->width == width && fb->physW == physW) {
        return fb;
    }
    if (fb == NULL) {
        if (sFbCount == MAX_FBS) {
            portAbort("PORT: too many colour images");
        }
        fb = &sFbs[sFbCount++];
        memset(fb, 0, sizeof *fb);
        glGenFramebuffers(1, &fb->fbo);
        glGenTextures(1, &fb->color);
    }
    fb->addr = addr;
    fb->width = width;
    fb->height = height;
    fb->physW = physW;
    fb->physH = height * PORT_GFX_SCALE;
    fb->ox = (physW - width * PORT_GFX_SCALE) / 2;
    fb->cpuDirty = 1;
    fb->gpuDirty = 0;
    fb->everDrawn = 0;
    ensureDepth(fb);
    glBindTexture(GL_TEXTURE_2D, fb->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fb->physW, fb->physH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gGfxDepthTex, 0);
    return fb;
}

/* RDRAM (BE RGBA5551, rows top-down) -> FBO colour */
void gfxFbUploadRdram(GfxFb *fb) {
    const u8 *src = (const u8 *)fb->addr;
    u32 x, y, w = fb->width, h = fb->height;
    for (y = 0; y < h; y++) {
        /* GL rows are bottom-up */
        u8 *o = &sScratch[((h - 1 - y) * w) * 4];
        for (x = 0; x < w; x++, o += 4) {
            u16 c = portReadBE16(src + (y * w + x) * 2);
            o[0] = ((c >> 11) & 0x1F) * 255 / 31;
            o[1] = ((c >> 6) & 0x1F) * 255 / 31;
            o[2] = ((c >> 1) & 0x1F) * 255 / 31;
            o[3] = 255;
        }
    }
    glBindTexture(GL_TEXTURE_2D, sStageTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, sScratch);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sStageFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sStageTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb->fbo);
    glDisable(GL_SCISSOR_TEST);
    if (fb->physW > w * PORT_GFX_SCALE) {
        /* side areas of a CPU-written image are black */
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBlitFramebuffer(0, 0, w, h, fb->ox, 0, fb->ox + w * PORT_GFX_SCALE, h * PORT_GFX_SCALE, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    fb->cpuDirty = 0;
}

void gfxFbBind(GfxFb *fb) {
    ensureDepth(fb); /* the shared depth follows the bound image's size */
    if (fb->cpuDirty) {
        gfxFbUploadRdram(fb);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glViewport(0, 0, fb->physW, fb->physH);
}

void gfxFbClearDepth(void) {
    if (gGfxDepthTex == 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sDepthFbo);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    gGfxDepthDirty = 1;
}

void gfxFbPresent(u32 addr, int black, int winW, int winH) {
    GfxFb *fb = black ? NULL : gfxFbFind(addr);
    int vw, vh, vx, vy;
    if (!black && fb == NULL && addr != 0) {
        /* never drawn by the GPU: show the CPU-written RDRAM image */
        fb = gfxFbFor(addr, gPortViWidth);
    }
    if (fb != NULL && fb->cpuDirty) {
        gfxFbUploadRdram(fb);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, winW, winH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (fb == NULL) {
        return;
    }
    if ((s64)winW * fb->physH > (s64)winH * fb->physW) {
        vh = winH;
        vw = (int)((s64)winH * fb->physW / fb->physH);
    } else {
        vw = winW;
        vh = (int)((s64)winW * fb->physH / fb->physW);
    }
    vx = (winW - vw) / 2;
    vy = (winH - vh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->fbo);
    glBlitFramebuffer(0, 0, fb->physW, fb->physH, vx, vy, vx + vw, vy + vh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void gfxFbCpuWrote(u32 addr, u32 len) {
    int i;
    for (i = 0; i < sFbCount; i++) {
        GfxFb *fb = &sFbs[i];
        u32 end = fb->addr + fb->width * fb->height * 2;
        if (addr < end && addr + len > fb->addr) {
            fb->cpuDirty = 1;
        }
    }
}

/* RDP z format: 18-bit z -> 3-bit exponent, 11-bit mantissa, 2-bit dz (0) */
static u16 zCompress(u32 z18) {
    static const struct { u32 shift; u32 add; } kExp[8] = {
        { 6, 0x00000 }, { 5, 0x20000 }, { 4, 0x30000 }, { 3, 0x38000 },
        { 2, 0x3C000 }, { 1, 0x3E000 }, { 0, 0x3F000 }, { 0, 0x3F800 },
    };
    u32 e;
    for (e = 0; e < 7; e++) {
        u32 next = e < 6 ? kExp[e + 1].add : 0x3F800;
        if (z18 < next) {
            break;
        }
    }
    return (u16)((e << 13) | ((((z18 - kExp[e].add) >> kExp[e].shift) & 0x7FF) << 2));
}

static void syncDepth(u32 addr, u32 len) {
    u32 w = gGfxDepthW, h = gGfxDepthH, x, y;
    u32 sw = w * PORT_GFX_SCALE, sh = h * PORT_GFX_SCALE;
    u16 *dst = (u16 *)addr;
    u32 n = len / 2 < w * h ? len / 2 : w * h; /* RDRAM past the requested range belongs to someone else */
    glBindTexture(GL_TEXTURE_2D, sDepthReadTarget);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sDepthPhysW, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, sDepthReadFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sDepthReadTarget, 0);
    glViewport(0, 0, sDepthPhysW, sh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(sDepthReadProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gGfxDepthTex);
    glUniform1i(glGetUniformLocation(sDepthReadProg, "uDepth"), 0);
    glBindVertexArray(sDepthReadVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(sDepthOx, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, sScratch);
    for (y = 0; y < h && y * w < n; y++) {
        for (x = 0; x < w && y * w + x < n; x++) {
            const u8 *p = &sScratch[(((sh - 1 - y * PORT_GFX_SCALE) * sw) + x * PORT_GFX_SCALE) * 4];
            u32 d24 = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            dst[y * w + x] = zCompress(d24 >> 6);
        }
    }
}

void gfxFbSyncRdram(u32 addr, u32 len) {
    int i;
    if (gGfxDepthDirty && gGfx.zimg != 0 && addr < gGfx.zimg + gGfxDepthW * gGfxDepthH * 2 && addr + len > gGfx.zimg) {
        syncDepth(gGfx.zimg, len);
        gGfxDepthDirty = 0;
    }
    for (i = 0; i < sFbCount; i++) {
        GfxFb *fb = &sFbs[i];
        u32 w = fb->width, h = fb->height, x, y;
        u32 sw = w * PORT_GFX_SCALE, sh = h * PORT_GFX_SCALE;
        u32 lo, hi, end = fb->addr + w * h * 2;
        u8 *dst;
        if (!(addr < end && addr + len > fb->addr) || !fb->gpuDirty) {
            continue;
        }
        /* only the pixels the caller reads: the image's real height is unknown (assumed 4:3), and RDRAM past
         * it belongs to someone else */
        lo = (addr > fb->addr ? addr - fb->addr : 0) / 2;
        hi = ((addr + len < end ? addr + len : end) - fb->addr + 1) / 2;
        glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
        glReadPixels(fb->ox, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, sScratch);
        dst = (u8 *)fb->addr;
        for (y = lo / w; y < h && y * w < hi; y++) {
            for (x = 0; x < w; x++) {
                u32 px = y * w + x;
                const u8 *p = &sScratch[(((sh - 1 - y * PORT_GFX_SCALE) * sw) + x * PORT_GFX_SCALE) * 4];
                u16 c;
                if (px < lo || px >= hi) {
                    continue;
                }
                c = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | 1);
                dst[px * 2] = c >> 8;
                dst[px * 2 + 1] = c & 0xFF;
            }
        }
        if (lo == 0 && hi >= w * h) {
            fb->gpuDirty = 0;
        }
    }
}
