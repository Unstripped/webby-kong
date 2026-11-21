/* Colour images as FBOs (RGBA8 at the runtime render scale, optionally multisampled) sharing one depth buffer;
 * RDRAM coherence: CPU writes are uploaded before the next draw/present, CPU reads sync the GPU image back first.
 * RDRAM colour is big-endian RGBA5551; the RDRAM depth buffer is native u16 in N64 z format. */
#include "gfx/gfx_gl.h"

#define MAX_FBS 16

static GfxFb sFbs[MAX_FBS];
static int sFbCount;
GLuint gGfxDepthTex;
int gGfxDepthDirty;
u32 gGfxDepthW, gGfxDepthH;
u32 gGfxScale = 2;
static GLuint sDepthFbo;
static GLuint sDepthMsRb, sDepthResolveFbo; /* MSAA: depth is drawn into sDepthMsRb, resolved into gGfxDepthTex */
static GLuint sStageTex;
static GLuint sDepthReadProg, sDepthReadVao, sDepthReadTarget, sDepthReadFbo;
static GLuint sCopyProg;
static GLuint sDownTex, sDownFbo; /* N64-sized colour readback target */
static u32 sDownW, sDownH;
static u8 *sScratch;
static u32 sDepthPhysW, sDepthPhysH, sDepthOx, sDepthScale;
static float sWinAspect = 4.0f / 3.0f;
static GLint sMaxDim;
static u32 sScaleOverride; /* 0: follow the window height */
static u32 sLoggedScale, sLoggedViW;
static int sMsaaRequest = 4;
static int sSamples; /* 0: MSAA off */
/* where the last present put its image on the canvas (gfxFbPresentedToN64, the debug pixel probe) */
static struct PresentedImage {
    int winW, winH, vx, vy, vw, vh;
    u32 physW, physH, ox, scale;
} sPresented;

/* the shared depth goes on colour FBOs of its size; the rest detach it so they stay complete */
static void attachDepth(const GfxFb *fb) {
    int same = gGfxDepthTex != 0 && fb->physW == sDepthPhysW && fb->physH == sDepthPhysH;
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    if (sSamples) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, same ? sDepthMsRb : 0);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, same ? gGfxDepthTex : 0, 0);
    }
}

static void ensureDepth(const GfxFb *fb) {
    int i;
    if (gGfxDepthTex != 0 && gGfxDepthW == fb->width && gGfxDepthH == fb->height && sDepthPhysW == fb->physW &&
        sDepthPhysH == fb->physH) {
        return;
    }
    if (gGfxDepthTex == 0) {
        GLenum none = GL_NONE;
        glGenTextures(1, &gGfxDepthTex);
        glGenFramebuffers(1, &sDepthFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, sDepthFbo);
        glDrawBuffers(1, &none);
    }
    if (sSamples && sDepthMsRb == 0) { /* first MSAA depth, or MSAA turned on live (portGfxSetMsaa) */
        GLenum none = GL_NONE;
        glGenRenderbuffers(1, &sDepthMsRb);
        glGenFramebuffers(1, &sDepthResolveFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, sDepthResolveFbo);
        glDrawBuffers(1, &none);
    }
    gGfxDepthW = fb->width;
    gGfxDepthH = fb->height;
    sDepthPhysW = fb->physW;
    sDepthPhysH = fb->physH;
    sDepthOx = fb->ox;
    sDepthScale = fb->scale;
    glBindTexture(GL_TEXTURE_2D, gGfxDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, sDepthPhysW, sDepthPhysH, 0, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (sSamples) {
        glBindRenderbuffer(GL_RENDERBUFFER, sDepthMsRb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, sSamples, GL_DEPTH_COMPONENT24, sDepthPhysW, sDepthPhysH);
        glBindFramebuffer(GL_FRAMEBUFFER, sDepthResolveFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gGfxDepthTex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, sDepthFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sDepthMsRb);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, sDepthFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gGfxDepthTex, 0);
    }
    for (i = 0; i < sFbCount; i++) {
        attachDepth(&sFbs[i]);
    }
}

static const char *kQuadVs =
    "#version 300 es\n"
    "out vec2 vUv;\n"
    "void main() { vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)); vUv = p;"
    " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
/* one fragment per N64 pixel: the texel the RDRAM pixel's top-left GL pixel holds */
static const char *kDepthReadFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D uDepth;\n"
    "uniform int uOx;\n"
    "uniform int uScale;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    float d = texelFetch(uDepth, ivec2(uOx + p.x * uScale, p.y * uScale + uScale - 1), 0).r;\n"
    "    float v = floor(d * 16777215.0 + 0.5);\n"
    "    float r = floor(v / 65536.0); v -= r * 65536.0;\n"
    "    float g = floor(v / 256.0); float b = v - g * 256.0;\n"
    "    fragColor = vec4(r, g, b, 255.0) / 255.0;\n"
    "}\n";
static const char *kCopyFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uSrc;\n"
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = texture(uTex, uSrc.xy + vUv * uSrc.zw); }\n";

int gfxFbQsRanges(QsRange *out) {
    int n = 0;
    out[n++] = QS_RANGE(sFbs);
    out[n++] = QS_RANGE(sFbCount);
    out[n++] = QS_RANGE(gGfxDepthTex);
    out[n++] = QS_RANGE(gGfxDepthDirty);
    out[n++] = QS_RANGE(gGfxDepthW);
    out[n++] = QS_RANGE(gGfxDepthH);
    out[n++] = QS_RANGE(gGfxScale);
    out[n++] = QS_RANGE(sDepthFbo);
    out[n++] = QS_RANGE(sDepthMsRb);
    out[n++] = QS_RANGE(sDepthResolveFbo);
    out[n++] = QS_RANGE(sStageTex);
    out[n++] = QS_RANGE(sDepthReadProg);
    out[n++] = QS_RANGE(sDepthReadVao);
    out[n++] = QS_RANGE(sDepthReadTarget);
    out[n++] = QS_RANGE(sDepthReadFbo);
    out[n++] = QS_RANGE(sCopyProg);
    out[n++] = QS_RANGE(sDownTex);
    out[n++] = QS_RANGE(sDownFbo);
    out[n++] = QS_RANGE(sDownW);
    out[n++] = QS_RANGE(sDownH);
    out[n++] = QS_RANGE(sScratch);
    out[n++] = QS_RANGE(sDepthPhysW);
    out[n++] = QS_RANGE(sDepthPhysH);
    out[n++] = QS_RANGE(sDepthOx);
    out[n++] = QS_RANGE(sDepthScale);
    out[n++] = QS_RANGE(sWinAspect);
    out[n++] = QS_RANGE(sMaxDim);
    out[n++] = QS_RANGE(sScaleOverride);
    out[n++] = QS_RANGE(sLoggedScale);
    out[n++] = QS_RANGE(sLoggedViW);
    out[n++] = QS_RANGE(sMsaaRequest);
    out[n++] = QS_RANGE(sSamples);
    out[n++] = QS_RANGE(sPresented);
    return n;
}

/* RDRAM was replaced: every colour image re-uploads from it, nothing on the GPU is newer */
void gfxFbQsRestored(void) {
    int i;
    for (i = 0; i < sFbCount; i++) {
        sFbs[i].cpuDirty = 1;
        sFbs[i].gpuDirty = 0;
    }
    gGfxDepthDirty = 0;
}

int portGfxSetScale(int scale) {
    if (scale < 0 || scale > 8) {
        portLog("PORT: ignoring scale %d (0..8)", scale);
        return 0;
    }
    sScaleOverride = (u32)scale; /* the next task's gfxFbSetWindowSize picks it up; gfxFbFor resizes the images */
    return 1;
}

static int samplesFor(int request) {
    GLint max = 0;
    int s;
    glGetIntegerv(GL_MAX_SAMPLES, &max);
    s = request > max ? max : request;
    return s < 2 ? 0 : s;
}

int portGfxMaxSamples(void) {
    GLint max = 0;
    if (sScratch == NULL) {
        return 0;
    }
    glGetIntegerv(GL_MAX_SAMPLES, &max);
    return max;
}

/* before gfxFbInit only stores the request; after it, rebuilds every colour image's multisample storage between
 * frames, carrying the drawn image over through the single-sample texture */
int portGfxSetMsaa(int msaa) {
    int samples, i;
    if (msaa != 0 && msaa != 1 && msaa != 2 && msaa != 4 && msaa != 8) {
        portLog("PORT: ignoring msaa %d (0,1,2,4,8)", msaa);
        return sSamples;
    }
    sMsaaRequest = msaa;
    if (sScratch == NULL) {
        return 0;
    }
    samples = samplesFor(msaa);
    if (samples == sSamples) {
        return sSamples;
    }
    for (i = 0; i < sFbCount; i++) {
        gfxFbResolve(&sFbs[i]); /* with the old sSamples: the colour texture now holds everything drawn */
        if (sFbs[i].msColor != 0) {
            glDeleteRenderbuffers(1, &sFbs[i].msColor);
            sFbs[i].msColor = 0;
        }
        sFbs[i].scale = 0; /* forces gfxFbFor to reallocate */
    }
    if (sDepthMsRb != 0) {
        glDeleteRenderbuffers(1, &sDepthMsRb);
        glDeleteFramebuffers(1, &sDepthResolveFbo);
        sDepthMsRb = sDepthResolveFbo = 0;
    }
    sSamples = samples;
    sDepthPhysW = 0; /* ensureDepth reallocates */
    /* eager: gfxFbPresent uploads into images found by gfxFbFind, which never reallocates */
    for (i = 0; i < sFbCount; i++) {
        gfxFbFor(sFbs[i].addr, sFbs[i].width);
    }
    portLog("PORT: msaa %d", sSamples);
    return sSamples;
}

void gfxFbInit(void) {
    GLint maxTex = 0, maxRb = 0;
    glGenTextures(1, &sStageTex);
    sDepthReadProg = gfxCompileProgram(kQuadVs, kDepthReadFs);
    sCopyProg = gfxCompileProgram(kQuadVs, kCopyFs);
    glGenVertexArrays(1, &sDepthReadVao);
    glGenTextures(1, &sDepthReadTarget);
    glGenFramebuffers(1, &sDepthReadFbo);
    glGenTextures(1, &sDownTex);
    glGenFramebuffers(1, &sDownFbo);
    sScratch = malloc(640 * 480 * 4); /* uploads and readbacks are N64-sized at any scale */
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRb);
    sMaxDim = maxTex < maxRb ? maxTex : maxRb;
    sSamples = samplesFor(sMsaaRequest);
    portLog("PORT: msaa %d", sSamples);
}

/* the drawable size picks the scale for images allocated from now on: at least the canvas height (present only
 * downsamples), capped at 6x and at the GL size limits */
void gfxFbSetWindowSize(int w, int h) {
    u32 imgH = gPortViWidth * 3 / 4, s;
    sWinAspect = h > 0 ? (float)w / h : 4.0f / 3.0f;
    if (sScaleOverride != 0) {
        s = sScaleOverride;
    } else {
        s = h > 0 && imgH > 0 ? ((u32)h + imgH - 1) / imgH : 2;
        if (s > 6) {
            s = 6;
        }
    }
    if (s < 1) {
        s = 1;
    }
    while (s > 1 && (gPortViWidth * s > (u32)sMaxDim || imgH * s > (u32)sMaxDim)) {
        s--;
    }
    gGfxScale = s;
    if (s != sLoggedScale || gPortViWidth != sLoggedViW) {
        sLoggedScale = s;
        sLoggedViW = gPortViWidth;
        portLog("PORT: render scale %u (%ux%u)", s, gPortViWidth * s, imgH * s);
    }
}

/* VI-sized images widen to the window aspect (Hor+); offscreen images keep their N64 size */
u32 gfxFbPhysWidth(u32 width) {
    u32 base = width * gGfxScale, h = width * 3 / 4 * gGfxScale, w;
    if (width != gPortViWidth || sWinAspect <= 4.0f / 3.0f) {
        return base;
    }
    w = (u32)(h * sWinAspect + 0.5f);
    if (sMaxDim > 0 && w > (u32)sMaxDim) {
        w = (u32)sMaxDim;
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

/* draw tex's normalised source rect into fb's GL columns [dx, dx + dw), rows [0, dh). A quad, not a blit: GLES3
 * can't blit into a multisample FBO. Callers rebind their own state; the renderer sets it per draw. */
static void copyTexInto(GfxFb *fb, GLuint tex, float sx, float sy, float sw, float sh, u32 dx, u32 dw, u32 dh,
                        GLenum filter) {
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glViewport(dx, 0, dw, dh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glUseProgram(sCopyProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glUniform1i(glGetUniformLocation(sCopyProg, "uTex"), 0);
    glUniform4f(glGetUniformLocation(sCopyProg, "uSrc"), sx, sy, sw, sh);
    glBindVertexArray(sDepthReadVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    fb->resolveDirty = 1;
}

static void clearBlack(GfxFb *fb) {
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    fb->resolveDirty = 1;
}

static void checkComplete(GLuint fbo, u32 physW, u32 physH) {
    GLenum status;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        portAbort("PORT: colour image FBO incomplete 0x%x (%ux%u, msaa %d)", status, physW, physH, sSamples);
    }
}

GfxFb *gfxFbFor(u32 addr, u32 width) {
    GfxFb *fb = gfxFbFind(addr);
    u32 height = width * 3 / 4;
    u32 physW = gfxFbPhysWidth(width);
    GLuint oldColor = 0;
    u32 oldPhysW = 0, oldOx = 0, oldSpan = 0;
    if (fb != NULL && fb->width == width && fb->physW == physW && fb->scale == gGfxScale) {
        return fb;
    }
    if (fb == NULL) {
        if (sFbCount == MAX_FBS) {
            portAbort("PORT: too many colour images");
        }
        fb = &sFbs[sFbCount++];
        memset(fb, 0, sizeof *fb);
        glGenFramebuffers(1, &fb->fbo);
        glGenFramebuffers(1, &fb->resolveFbo);
    } else if (fb->gpuDirty && !fb->cpuDirty) {
        /* resized while the GPU image is newer than RDRAM: carry the drawn frame over instead of re-uploading
         * stale RDRAM (the scale follows every window height change). Pending CPU writes win, as in gfxFbBind. */
        gfxFbResolve(fb);
        oldColor = fb->color;
        oldPhysW = fb->physW;
        oldOx = fb->ox;
        oldSpan = fb->width * fb->scale;
        fb->color = 0;
    }
    if (fb->color == 0) {
        glGenTextures(1, &fb->color);
    }
    if (sSamples && fb->msColor == 0) {
        glGenRenderbuffers(1, &fb->msColor);
    }
    fb->addr = addr;
    fb->width = width;
    fb->height = height;
    fb->physW = physW;
    fb->scale = gGfxScale;
    fb->physH = height * fb->scale;
    fb->ox = (physW - width * fb->scale) / 2;
    ensureDepth(fb);
    glBindTexture(GL_TEXTURE_2D, fb->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fb->physW, fb->physH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->resolveFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->color, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    if (sSamples) {
        glBindRenderbuffer(GL_RENDERBUFFER, fb->msColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, sSamples, GL_RGBA8, fb->physW, fb->physH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fb->msColor);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->color, 0);
    }
    attachDepth(fb);
    checkComplete(fb->fbo, fb->physW, fb->physH);
    checkComplete(fb->resolveFbo, fb->physW, fb->physH);
    if (oldColor != 0) {
        clearBlack(fb);
        copyTexInto(fb, oldColor, oldOx / (float)oldPhysW, 0, oldSpan / (float)oldPhysW, 1, fb->ox,
                    width * fb->scale, fb->physH, GL_LINEAR);
        glDeleteTextures(1, &oldColor);
        fb->cpuDirty = 0;
        fb->gpuDirty = 1;
    } else {
        fb->cpuDirty = 1;
        fb->gpuDirty = 0;
        fb->everDrawn = 0;
    }
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
    if (fb->physW > w * fb->scale) {
        /* side areas of a CPU-written image are black */
        clearBlack(fb);
    }
    copyTexInto(fb, sStageTex, 0, 0, 1, 1, fb->ox, w * fb->scale, h * fb->scale, GL_NEAREST);
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

void gfxFbDrew(GfxFb *fb) {
    fb->gpuDirty = 1;
    fb->everDrawn = 1;
    fb->resolveDirty = 1;
}

/* MSAA: bring the single-sample colour texture up to date with what was drawn */
void gfxFbResolve(GfxFb *fb) {
    if (sSamples && fb->resolveDirty) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb->resolveFbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, fb->physW, fb->physH, 0, 0, fb->physW, fb->physH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    fb->resolveDirty = 0;
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
    if (fb != NULL) {
        if (fb->cpuDirty) {
            gfxFbUploadRdram(fb);
        }
        gfxFbResolve(fb);
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
    sPresented.winW = winW;
    sPresented.winH = winH;
    sPresented.vx = vx;
    sPresented.vy = vy;
    sPresented.vw = vw;
    sPresented.vh = vh;
    sPresented.physW = fb->physW;
    sPresented.physH = fb->physH;
    sPresented.ox = fb->ox;
    sPresented.scale = fb->scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->resolveFbo);
    glBlitFramebuffer(0, 0, fb->physW, fb->physH, vx, vy, vx + vw, vy + vh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

int gfxFbPresentedToN64(float fx, float fy, float *x, float *y) {
    const struct PresentedImage *p = &sPresented;
    float px, py;
    if (p->vw <= 0 || p->vh <= 0 || p->scale == 0) {
        return 0;
    }
    px = (fx * p->winW - p->vx) / p->vw;
    py = (fy * p->winH - p->vy) / p->vh; /* the letterbox is symmetric, so top and bottom margins match */
    if (px < 0 || px >= 1 || py < 0 || py >= 1) {
        return 0;
    }
    *x = (px * p->physW - p->ox) / p->scale;
    *y = py * p->physH / p->scale;
    return 1;
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
    u16 *dst = (u16 *)addr;
    u32 n = len / 2 < w * h ? len / 2 : w * h; /* RDRAM past the requested range belongs to someone else */
    if (sSamples) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sDepthFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sDepthResolveFbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, sDepthPhysW, sDepthPhysH, 0, 0, sDepthPhysW, sDepthPhysH, GL_DEPTH_BUFFER_BIT,
                          GL_NEAREST);
    }
    glBindTexture(GL_TEXTURE_2D, sDepthReadTarget);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, sDepthReadFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sDepthReadTarget, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(sDepthReadProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gGfxDepthTex);
    glUniform1i(glGetUniformLocation(sDepthReadProg, "uDepth"), 0);
    glUniform1i(glGetUniformLocation(sDepthReadProg, "uOx"), (GLint)sDepthOx);
    glUniform1i(glGetUniformLocation(sDepthReadProg, "uScale"), (GLint)sDepthScale);
    glBindVertexArray(sDepthReadVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, sScratch);
    for (y = 0; y < h && y * w < n; y++) {
        for (x = 0; x < w && y * w + x < n; x++) {
            const u8 *p = &sScratch[((h - 1 - y) * w + x) * 4];
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
        u32 lo, hi, end = fb->addr + w * h * 2;
        u8 *dst;
        if (!(addr < end && addr + len > fb->addr) || !fb->gpuDirty) {
            continue;
        }
        /* only the pixels the caller reads: the image's real height is unknown (assumed 4:3), and RDRAM past
         * it belongs to someone else */
        lo = (addr > fb->addr ? addr - fb->addr : 0) / 2;
        hi = ((addr + len < end ? addr + len : end) - fb->addr + 1) / 2;
        gfxFbResolve(fb);
        if (sDownW != w || sDownH != h) {
            sDownW = w;
            sDownH = h;
            glBindTexture(GL_TEXTURE_2D, sDownTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glBindFramebuffer(GL_FRAMEBUFFER, sDownFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sDownTex, 0);
        }
        /* downsample on the GPU: the readback is N64-sized at any render scale */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->resolveFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sDownFbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(fb->ox, 0, fb->ox + w * fb->scale, h * fb->scale, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sDownFbo);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, sScratch);
        dst = (u8 *)fb->addr;
        for (y = lo / w; y < h && y * w < hi; y++) {
            for (x = 0; x < w; x++) {
                u32 px = y * w + x;
                const u8 *p = &sScratch[((h - 1 - y) * w + x) * 4];
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
