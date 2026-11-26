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
static GLuint sDepthReadProg, sDepthReadTarget, sDepthReadFbo;
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
/* Present pass (issue #33): the finished image reaches the canvas through full-screen shader passes - an area
 * downsample, optional FXAA and CAS sharpening, ordered dither - instead of one bilinear blit. With every option
 * off the blit is kept, so that output is bit for bit the one from before the pass. */
static struct {
    int downsample; /* 0: bilinear, the blit's filter; 1: area (box) */
    int sharpen;    /* CAS strength, 0 (off) to 10 */
    int fxaa;
    int dither;
    int ready; /* gfxFbInit ran: a setter can compile its program instead of the present that needs it */
} sPost = { 1, 0, 0, 0, 0 };
typedef struct {
    GLuint prog;
    GLint uTex, uDither, uSize, uWidth, uTaps, uTexel, uAmount;
} PostProg;
enum { kPostResample, kPostFxaa, kPostSharpen, kPostProgCount };
static PostProg sPostProg[kPostProgCount]; /* compiled on first use */
static struct {
    GLuint tex, fbo;
    u32 w, h;
} sPostBuf[2]; /* canvas-sized ping-pong, for the passes that do not write the canvas */
/* where the last present put its image on the canvas (gfxFbPresentedToN64, the debug pixel probe) */
static struct PresentedImage {
    int winW, winH, vx, vy, vw, vh;
    u32 physW, physH, ox, scale;
} sPresented;
/* the in-between image (frame interpolation): the VI image's twin with its own depth buffer, so the replays leave
 * every colour image, the shared depth and RDRAM as the game frame left them. Each replay's result is copied
 * (resolved) into a slot, single-sampled at the same size, until it is presented. */
#define INTERP_SLOTS 7 /* in-between images per game frame at 240 fps */
static struct {
    GfxFb fb;
    GLuint depth;  /* depth renderbuffer, multisampled like the colour images */
    int samples;   /* sSamples the storage was made for */
    GLuint slotTex[INTERP_SLOTS], slotFbo[INTERP_SLOTS];
    u32 slotW[INTERP_SLOTS], slotH[INTERP_SLOTS]; /* storage size, 0 none */
} sInterpImg;
/* Depth prefetch: the depth image read into a pixel-pack buffer at the end of every task that drew depth, so the
 * game's read on a later VI (sun/flare occlusion, every frame) copies finished data instead of stalling on the GPU
 * work queued since (the in-between image). Valid while no depth write followed (gGfxDepthSerial). */
u32 gGfxDepthSerial;
static struct {
    GLuint pbo;
    u32 serial, w, h;
    int valid;
    int collected; /* data holds the buffer: copied before the next VI queues the in-between image */
    u8 *data;
} sDepthPrefetch;

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
    gGfxDepthSerial++;
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

GLuint gGfxQuadVao;
const char *gGfxQuadVs =
    "#version 300 es\n"
    "out vec2 vUv;\n"
    "void main() { vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)); vUv = p;"
    " gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
/* two N64 pixels per fragment (x = 2 * fragment x and the one after), each the texel its RDRAM pixel's top-left GL
 * pixel holds, already in the RDP z format (18-bit z -> 3-bit exponent, 11-bit mantissa, 2-bit dz of 0) and
 * little-endian: the readback is half the bytes of 24-bit depth and the CPU only copies. The 24-bit value is the
 * one an RGBA8 target held before: d = 1.0 rounds to 2^24 in float, whose high byte (256) the target clamped to
 * 255, giving 0xFF0000 (cleared depth reads as 0xF000). */
static const char *kDepthReadFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D uDepth;\n"
    "uniform int uOx;\n"
    "uniform int uScale;\n"
    "uniform int uW;\n"
    "out vec4 fragColor;\n"
    "const uint kShift[8] = uint[8](6u, 5u, 4u, 3u, 2u, 1u, 0u, 0u);\n"
    "const uint kAdd[8] = uint[8](0x00000u, 0x20000u, 0x30000u, 0x38000u, 0x3C000u, 0x3E000u, 0x3F000u, 0x3F800u);\n"
    "uint zAt(int x, int y) {\n"
    "    float d = texelFetch(uDepth, ivec2(uOx + x * uScale, y * uScale + uScale - 1), 0).r;\n"
    "    uint v = uint(floor(d * 16777215.0 + 0.5));\n"
    "    uint z18 = ((min(v >> 16, 255u) << 16) | (v & 0xFFFFu)) >> 6;\n"
    "    uint e = 0u;\n"
    "    while (e < 7u && z18 >= (e < 6u ? kAdd[e + 1u] : 0x3F800u)) e++;\n"
    "    return (e << 13) | ((((z18 - kAdd[e]) >> kShift[e]) & 0x7FFu) << 2);\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    uint z0 = zAt(p.x * 2, p.y);\n"
    "    uint z1 = p.x * 2 + 1 < uW ? zAt(p.x * 2 + 1, p.y) : 0u;\n"
    "    fragColor = vec4(float(z0 & 255u), float(z0 >> 8), float(z1 & 255u), float(z1 >> 8)) / 255.0;\n"
    "}\n";
static const char *kCopyFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uSrc;\n"
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = texture(uTex, uSrc.xy + vUv * uSrc.zw); }\n";
/* The present pass (issue #33). kResampleFs weights every source texel by the area it covers of the destination
 * pixel's footprint - a box uWidth source texels wide, never narrower than one texel, so an upscale is exactly
 * bilinear and a downsample is an area filter. kFxaaFs is FXAA 3.11's console edge blend, kSharpenFs is AMD's CAS
 * without scaling. Whichever pass writes the canvas adds ordered 8x8 Bayer dither of half a least significant bit
 * (uDither; 0 leaves the colour untouched), against the banding that quantising the filtered values - which carry
 * more precision than the 8-bit image they came from - back to 8 bits would leave. */
#define POST_HEAD                                                                                                  \
    "#version 300 es\n"                                                                                            \
    "precision highp float;\n"                                                                                     \
    "uniform sampler2D uTex;\n"                                                                                    \
    "uniform float uDither;\n"                                                                                     \
    "in vec2 vUv;\n"                                                                                               \
    "out vec4 fragColor;\n"                                                                                        \
    "const int kBayer[64] = int[64](\n"                                                                            \
    " 0,32, 8,40, 2,34,10,42, 48,16,56,24,50,18,58,26, 12,44, 4,36,14,46, 6,38, 60,28,52,20,62,30,54,22,\n"        \
    " 3,35,11,43, 1,33, 9,41, 51,19,59,27,49,17,57,25, 15,47, 7,39,13,45, 5,37, 63,31,55,23,61,29,53,21);\n"       \
    "vec3 dither(vec3 c) {\n"                                                                                      \
    "    ivec2 p = ivec2(gl_FragCoord.xy) & 7;\n"                                                                  \
    "    return c + uDither * ((float(kBayer[p.y * 8 + p.x]) + 0.5) * (1.0 / 64.0) - 0.5);\n"                      \
    "}\n"
static const char *kResampleFs = POST_HEAD
    "uniform ivec2 uSize;\n" /* source image in texels */
    "uniform vec2 uWidth;\n" /* footprint of one destination pixel, in source texels */
    "uniform ivec2 uTaps;\n"
    "void main() {\n"
    "    vec2 lo = vUv * vec2(uSize) - uWidth * 0.5, hi = lo + uWidth;\n"
    "    ivec2 o = ivec2(floor(lo));\n"
    "    vec4 sum = vec4(0.0);\n"
    "    float total = 0.0;\n"
    "    for (int j = 0; j < uTaps.y; j++) {\n"
    "        float y = float(o.y + j);\n"
    "        float wy = min(y + 1.0, hi.y) - max(y, lo.y);\n"
    "        if (wy <= 0.0) continue;\n"
    "        for (int i = 0; i < uTaps.x; i++) {\n"
    "            float x = float(o.x + i);\n"
    "            float w = wy * (min(x + 1.0, hi.x) - max(x, lo.x));\n"
    "            if (w <= 0.0) continue;\n"
    "            sum += w * texelFetch(uTex, clamp(ivec2(o.x + i, o.y + j), ivec2(0), uSize - 1), 0);\n"
    "            total += w;\n"
    "        }\n"
    "    }\n"
    "    sum /= total;\n"
    "    fragColor = vec4(dither(sum.rgb), sum.a);\n"
    "}\n";
static const char *kFxaaFs = POST_HEAD
    "uniform vec2 uTexel;\n"
    "const vec3 kLuma = vec3(0.299, 0.587, 0.114);\n"
    "void main() {\n"
    "    vec4 m = texture(uTex, vUv);\n"
    "    float lM = dot(m.rgb, kLuma);\n"
    "    float lNW = dot(texture(uTex, vUv + vec2(-1.0, -1.0) * uTexel).rgb, kLuma);\n"
    "    float lNE = dot(texture(uTex, vUv + vec2( 1.0, -1.0) * uTexel).rgb, kLuma);\n"
    "    float lSW = dot(texture(uTex, vUv + vec2(-1.0,  1.0) * uTexel).rgb, kLuma);\n"
    "    float lSE = dot(texture(uTex, vUv + vec2( 1.0,  1.0) * uTexel).rgb, kLuma);\n"
    "    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));\n"
    "    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    /* FXAA's edge test: FXAA_EDGE_THRESHOLD_MIN 1/16 absolute, FXAA_EDGE_THRESHOLD 1/8 of the local maximum */
    "    if (lMax - lMin < max(0.0625, lMax * 0.125)) {\n"
    "        fragColor = vec4(dither(m.rgb), m.a);\n"
    "        return;\n"
    "    }\n"
    "    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
    "    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 * 0.125), 0.0078125);\n"
    "    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * uTexel;\n"
    "    vec3 a = 0.5 * (texture(uTex, vUv + dir * (1.0 / 3.0 - 0.5)).rgb +\n"
    "                    texture(uTex, vUv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "    vec3 b = a * 0.5 + 0.25 * (texture(uTex, vUv - dir * 0.5).rgb + texture(uTex, vUv + dir * 0.5).rgb);\n"
    "    float lB = dot(b, kLuma);\n"
    "    fragColor = vec4(dither(lB < lMin || lB > lMax ? a : b), m.a);\n"
    "}\n";
static const char *kSharpenFs = POST_HEAD
    "uniform vec2 uTexel;\n"
    "uniform float uAmount;\n" /* CAS peak, negative */
    "void main() {\n"
    "    vec4 e = texture(uTex, vUv);\n"
    "    vec3 a = texture(uTex, vUv + vec2(-1.0, -1.0) * uTexel).rgb;\n"
    "    vec3 b = texture(uTex, vUv + vec2( 0.0, -1.0) * uTexel).rgb;\n"
    "    vec3 c = texture(uTex, vUv + vec2( 1.0, -1.0) * uTexel).rgb;\n"
    "    vec3 d = texture(uTex, vUv + vec2(-1.0,  0.0) * uTexel).rgb;\n"
    "    vec3 f = texture(uTex, vUv + vec2( 1.0,  0.0) * uTexel).rgb;\n"
    "    vec3 g = texture(uTex, vUv + vec2(-1.0,  1.0) * uTexel).rgb;\n"
    "    vec3 h = texture(uTex, vUv + vec2( 0.0,  1.0) * uTexel).rgb;\n"
    "    vec3 i = texture(uTex, vUv + vec2( 1.0,  1.0) * uTexel).rgb;\n"
    "    vec3 mn = min(min(min(d, e.rgb), min(f, b)), h);\n"
    "    vec3 mx = max(max(max(d, e.rgb), max(f, b)), h);\n"
    "    mn += min(min(min(mn, a), min(c, g)), i);\n"
    "    mx += max(max(max(mx, a), max(c, g)), i);\n"
    "    vec3 w = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1.0 / 255.0)), 0.0, 1.0)) * uAmount;\n"
    "    vec3 col = clamp((b * w + d * w + f * w + h * w + e.rgb) / (1.0 + 4.0 * w), 0.0, 1.0);\n"
    "    fragColor = vec4(dither(col), e.a);\n"
    "}\n";

void gfxFbQsRanges(QsSink *out) {
    QS_KEEP(out, sFbs);
    QS_KEEP(out, sFbCount);
    QS_KEEP(out, gGfxDepthTex);
    QS_KEEP(out, gGfxDepthDirty);
    QS_KEEP(out, gGfxDepthW);
    QS_KEEP(out, gGfxDepthH);
    QS_KEEP(out, gGfxScale);
    QS_KEEP(out, sDepthFbo);
    QS_KEEP(out, sDepthMsRb);
    QS_KEEP(out, sDepthResolveFbo);
    QS_KEEP(out, sStageTex);
    QS_KEEP(out, sDepthReadProg);
    QS_KEEP(out, gGfxQuadVao);
    QS_KEEP(out, sDepthReadTarget);
    QS_KEEP(out, sDepthReadFbo);
    QS_KEEP(out, sCopyProg);
    QS_KEEP(out, sDownTex);
    QS_KEEP(out, sDownFbo);
    QS_KEEP(out, sDownW);
    QS_KEEP(out, sDownH);
    QS_KEEP(out, sScratch);
    QS_KEEP(out, sDepthPhysW);
    QS_KEEP(out, sDepthPhysH);
    QS_KEEP(out, sDepthOx);
    QS_KEEP(out, sDepthScale);
    QS_KEEP(out, sWinAspect);
    QS_KEEP(out, sMaxDim);
    QS_KEEP(out, sScaleOverride);
    QS_KEEP(out, sLoggedScale);
    QS_KEEP(out, sLoggedViW);
    QS_KEEP(out, sMsaaRequest);
    QS_KEEP(out, sSamples);
    QS_KEEP(out, sPost);
    QS_KEEP(out, sPostProg);
    QS_KEEP(out, sPostBuf);
    QS_KEEP(out, sPresented);
    QS_KEEP(out, sInterpImg);
    QS_KEEP(out, sDepthPrefetch);
}

/* RDRAM was replaced: every colour image re-uploads from it, nothing on the GPU is newer */
void gfxFbQsRestored(void) {
    int i;
    for (i = 0; i < sFbCount; i++) {
        sFbs[i].cpuDirty = 1;
        sFbs[i].gpuDirty = 0;
    }
    gGfxDepthDirty = 0;
    sDepthPrefetch.valid = 0;
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

/* present pass options (issue #33): player settings, kept by a quickload like sMsaaRequest, each in effect from
 * the next present on. Enabling one compiles its program here, not in the present that first needs it. */
static const PostProg *postProgram(int kind);

static void postPrepare(void) {
    if (!sPost.ready) {
        return; /* before gfxFbInit: the settings are stored, gfxFbInit compiles what they need */
    }
    if (!sPost.downsample && !sPost.fxaa && !sPost.sharpen && !sPost.dither) {
        return; /* every option off: present blits, and no program is needed */
    }
    postProgram(kPostResample); /* every pass starts with the resample */
    if (sPost.fxaa) {
        postProgram(kPostFxaa);
    }
    if (sPost.sharpen > 0) {
        postProgram(kPostSharpen);
    }
}

int portGfxSetDownsample(int mode) {
    sPost.downsample = mode != 0;
    postPrepare();
    return sPost.downsample;
}

int portGfxSetSharpen(int amount) {
    sPost.sharpen = amount < 0 ? 0 : amount > 10 ? 10 : amount;
    postPrepare();
    return sPost.sharpen;
}

int portGfxSetFxaa(int on) {
    sPost.fxaa = on != 0;
    postPrepare();
    return sPost.fxaa;
}

int portGfxSetDither(int on) {
    sPost.dither = on != 0;
    postPrepare();
    return sPost.dither;
}

void gfxFbInit(void) {
    GLint maxTex = 0, maxRb = 0;
    glGenTextures(1, &sStageTex);
    sDepthReadProg = gfxCompileProgram(gGfxQuadVs, kDepthReadFs);
    sCopyProg = gfxCompileProgram(gGfxQuadVs, kCopyFs);
    glGenVertexArrays(1, &gGfxQuadVao);
    glGenTextures(1, &sDepthReadTarget);
    glGenFramebuffers(1, &sDepthReadFbo);
    glGenTextures(1, &sDownTex);
    glGenFramebuffers(1, &sDownFbo);
    sScratch = malloc(640 * 480 * 4); /* uploads and readbacks are N64-sized at any scale */
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRb);
    sMaxDim = maxTex < maxRb ? maxTex : maxRb;
    sSamples = samplesFor(sMsaaRequest);
    sPost.ready = 1;
    postPrepare(); /* the options the player (or --downsample / --fxaa / --sharpen) set before this */
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
    glBindVertexArray(gGfxQuadVao);
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
    if (fb == &sInterpImg.fb) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
        glViewport(0, 0, fb->physW, fb->physH);
        return;
    }
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
    gGfxDepthSerial++;
}

/* the in-between image shaped like `real` (size, scale, Hor+ margin, MSAA), reallocated when that changes */
GfxFb *gfxFbInterpFor(const GfxFb *real) {
    GfxFb *fb = &sInterpImg.fb;
    if (fb->fbo == 0) {
        glGenFramebuffers(1, &fb->fbo);
        glGenFramebuffers(1, &fb->resolveFbo);
        glGenTextures(1, &fb->color);
        glGenRenderbuffers(1, &sInterpImg.depth);
    }
    if (fb->physW != real->physW || fb->physH != real->physH || fb->scale != real->scale || fb->ox != real->ox ||
        fb->width != real->width || sInterpImg.samples != sSamples) {
        fb->width = real->width;
        fb->height = real->height;
        fb->physW = real->physW;
        fb->physH = real->physH;
        fb->scale = real->scale;
        fb->ox = real->ox;
        fb->sideTask = 0;
        sInterpImg.samples = sSamples;
        glBindTexture(GL_TEXTURE_2D, fb->color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fb->physW, fb->physH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, fb->resolveFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->color, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
        glBindRenderbuffer(GL_RENDERBUFFER, sInterpImg.depth);
        if (sSamples) {
            if (fb->msColor == 0) {
                glGenRenderbuffers(1, &fb->msColor);
            }
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, sSamples, GL_DEPTH_COMPONENT24, fb->physW, fb->physH);
            glBindRenderbuffer(GL_RENDERBUFFER, fb->msColor);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, sSamples, GL_RGBA8, fb->physW, fb->physH);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fb->msColor);
        } else {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, fb->physW, fb->physH);
            if (fb->msColor != 0) {
                glDeleteRenderbuffers(1, &fb->msColor);
                fb->msColor = 0;
            }
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->color, 0);
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sInterpImg.depth);
        checkComplete(fb->fbo, fb->physW, fb->physH);
        checkComplete(fb->resolveFbo, fb->physW, fb->physH);
    }
    fb->addr = real->addr;
    return fb;
}

void gfxFbInterpClearDepth(void) {
    if (sInterpImg.fb.fbo == 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sInterpImg.fb.fbo);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
}

/* one program of the present pass, compiled and its uniforms looked up on first use */
static const PostProg *postProgram(int kind) {
    PostProg *p = &sPostProg[kind];
    if (p->prog == 0) {
        p->prog = gfxCompileProgram(gGfxQuadVs, kind == kPostResample ? kResampleFs
                                             : kind == kPostFxaa   ? kFxaaFs
                                                                   : kSharpenFs);
        p->uTex = glGetUniformLocation(p->prog, "uTex");
        p->uDither = glGetUniformLocation(p->prog, "uDither");
        p->uSize = glGetUniformLocation(p->prog, "uSize");
        p->uWidth = glGetUniformLocation(p->prog, "uWidth");
        p->uTaps = glGetUniformLocation(p->prog, "uTaps");
        p->uTexel = glGetUniformLocation(p->prog, "uTexel");
        p->uAmount = glGetUniformLocation(p->prog, "uAmount");
    }
    return p;
}

/* canvas-sized colour target one pass hands to the next */
static GLuint postTarget(int i, u32 w, u32 h) {
    if (sPostBuf[i].fbo == 0) {
        glGenFramebuffers(1, &sPostBuf[i].fbo);
        glGenTextures(1, &sPostBuf[i].tex);
    }
    if (sPostBuf[i].w != w || sPostBuf[i].h != h) {
        sPostBuf[i].w = w;
        sPostBuf[i].h = h;
        glBindTexture(GL_TEXTURE_2D, sPostBuf[i].tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, sPostBuf[i].fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sPostBuf[i].tex, 0);
        checkComplete(sPostBuf[i].fbo, w, h);
    }
    return sPostBuf[i].fbo;
}

/* `tex` (srcW x srcH) into the canvas rectangle through the enabled passes: the resample always, then FXAA and
 * sharpening, each reading what the one before wrote. The last one writes the canvas and carries the dither. */
static void presentPass(GLuint tex, u32 srcW, u32 srcH, int vx, int vy, int vw, int vh) {
    int kinds[kPostProgCount], n = 0, i, buf = 0;
    kinds[n++] = kPostResample;
    if (sPost.fxaa) {
        kinds[n++] = kPostFxaa;
    }
    if (sPost.sharpen > 0) {
        kinds[n++] = kPostSharpen;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(gGfxQuadVao);
    for (i = 0; i < n; i++) {
        const PostProg *p = postProgram(kinds[i]);
        int last = i == n - 1;
        glBindFramebuffer(GL_FRAMEBUFFER, last ? 0 : postTarget(buf, (u32)vw, (u32)vh));
        glViewport(last ? vx : 0, last ? vy : 0, vw, vh);
        glUseProgram(p->prog);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUniform1i(p->uTex, 0);
        glUniform1f(p->uDither, last && sPost.dither ? 1.0f / 255.0f : 0.0f);
        if (kinds[i] == kPostResample) {
            /* a box of one destination pixel in source texels (at least one: an upscale stays bilinear) */
            float wx = 1.0f, wy = 1.0f;
            if (sPost.downsample) {
                wx = (float)srcW / vw;
                wy = (float)srcH / vh;
                wx = wx < 1.0f ? 1.0f : wx;
                wy = wy < 1.0f ? 1.0f : wy;
            }
            glUniform2i(p->uSize, (GLint)srcW, (GLint)srcH);
            glUniform2f(p->uWidth, wx, wy);
            glUniform2i(p->uTaps, (GLint)wx + 2, (GLint)wy + 2); /* ceil(width) + 1 texels can overlap it */
        } else {
            glUniform2f(p->uTexel, 1.0f / vw, 1.0f / vh);
            if (kinds[i] == kPostSharpen) {
                glUniform1f(p->uAmount, -0.125f - 0.075f * (sPost.sharpen / 10.0f)); /* CAS peak, -1/8 .. -1/5 */
            }
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (!last) {
            tex = sPostBuf[buf].tex;
            buf ^= 1;
        }
    }
}

/* letterboxed present of an image shaped like `fb` (NULL: black) to the canvas: the shader passes, or the blit of
 * `readFbo` they replaced when every present option is off */
static void presentImage(const GfxFb *fb, GLuint readFbo, GLuint tex, int winW, int winH) {
    int vw, vh, vx, vy;
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
    if (vw <= 0 || vh <= 0) {
        return; /* a canvas of no size (minimised, display: none): nothing to present into */
    }
    if (!sPost.downsample && !sPost.fxaa && !sPost.sharpen && !sPost.dither) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        glBlitFramebuffer(0, 0, fb->physW, fb->physH, vx, vy, vx + vw, vy + vh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        return;
    }
    presentPass(tex, fb->physW, fb->physH, vx, vy, vw, vh);
}

void gfxFbPresent(u32 addr, int black, int winW, int winH) {
    GfxFb *fb = black ? NULL : gfxFbFind(addr);
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
    presentImage(fb, fb != NULL ? fb->resolveFbo : 0, fb != NULL ? fb->color : 0, winW, winH);
}

/* the replay just drawn into the in-between image, resolved into `slot` */
void gfxFbInterpStore(int slot) {
    GfxFb *fb = &sInterpImg.fb;
    if (slot < 0 || slot >= INTERP_SLOTS || fb->fbo == 0) {
        return;
    }
    if (sInterpImg.slotFbo[slot] == 0) {
        glGenFramebuffers(1, &sInterpImg.slotFbo[slot]);
        glGenTextures(1, &sInterpImg.slotTex[slot]);
    }
    if (sInterpImg.slotW[slot] != fb->physW || sInterpImg.slotH[slot] != fb->physH) {
        sInterpImg.slotW[slot] = fb->physW;
        sInterpImg.slotH[slot] = fb->physH;
        glBindTexture(GL_TEXTURE_2D, sInterpImg.slotTex[slot]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fb->physW, fb->physH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, sInterpImg.slotFbo[slot]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sInterpImg.slotTex[slot], 0);
        checkComplete(sInterpImg.slotFbo[slot], fb->physW, fb->physH);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sInterpImg.slotFbo[slot]);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, fb->physW, fb->physH, 0, 0, fb->physW, fb->physH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

/* 0 when the slot holds no image of the in-between image's current shape */
int gfxFbPresentInterp(int slot, int winW, int winH) {
    const GfxFb *fb = &sInterpImg.fb;
    if (slot < 0 || slot >= INTERP_SLOTS || sInterpImg.slotW[slot] != fb->physW || sInterpImg.slotH[slot] != fb->physH ||
        fb->physW == 0) {
        return 0;
    }
    presentImage(fb, sInterpImg.slotFbo[slot], sInterpImg.slotTex[slot], winW, winH);
    return 1;
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

/* readback width in texels: kDepthReadFs packs two N64 pixels per RGBA8 texel */
static u32 depthPackedW(void) {
    return (gGfxDepthW + 1) / 2;
}

/* depth image -> packed RDP z (kDepthReadFs) in sDepthReadFbo (bound as GL_READ_FRAMEBUFFER on return) */
static void depthToReadTarget(void) {
    static u32 targetW, targetH;
    static GLint uDepth = -1, uOx, uScale, uW;
    u32 w = depthPackedW(), h = gGfxDepthH;
    if (sSamples) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sDepthFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sDepthResolveFbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, sDepthPhysW, sDepthPhysH, 0, 0, sDepthPhysW, sDepthPhysH, GL_DEPTH_BUFFER_BIT,
                          GL_NEAREST);
    }
    if (targetW != w || targetH != h) {
        targetW = w;
        targetH = h;
        glBindTexture(GL_TEXTURE_2D, sDepthReadTarget);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, sDepthReadFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sDepthReadTarget, 0);
    }
    if (uDepth < 0) {
        uDepth = glGetUniformLocation(sDepthReadProg, "uDepth");
        uOx = glGetUniformLocation(sDepthReadProg, "uOx");
        uScale = glGetUniformLocation(sDepthReadProg, "uScale");
        uW = glGetUniformLocation(sDepthReadProg, "uW");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sDepthReadFbo);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(sDepthReadProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gGfxDepthTex);
    glUniform1i(uDepth, 0);
    glUniform1i(uOx, (GLint)sDepthOx);
    glUniform1i(uScale, (GLint)sDepthScale);
    glUniform1i(uW, (GLint)gGfxDepthW);
    glBindVertexArray(gGfxQuadVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sDepthReadFbo);
}

/* not in GLES3's header; emscripten's WebGL2 library has it */
void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void *data);

void gfxFbDepthPrefetch(void) {
    u32 w = gGfxDepthW, h = gGfxDepthH;
    if (!gGfxDepthDirty || gGfxDepthTex == 0 || w * h * 4 > 640 * 480 * 4) {
        return;
    }
    if (sDepthPrefetch.pbo == 0) {
        glGenBuffers(1, &sDepthPrefetch.pbo);
    }
    depthToReadTarget();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, sDepthPrefetch.pbo);
    if (sDepthPrefetch.w != w || sDepthPrefetch.h != h) {
        glBufferData(GL_PIXEL_PACK_BUFFER, depthPackedW() * h * 4, NULL, GL_STREAM_READ);
    }
    glReadPixels(0, 0, depthPackedW(), h, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    sDepthPrefetch.w = w;
    sDepthPrefetch.h = h;
    sDepthPrefetch.serial = gGfxDepthSerial;
    sDepthPrefetch.valid = 1;
    sDepthPrefetch.collected = 0;
}

static int depthPrefetchCurrent(void) {
    return sDepthPrefetch.valid && sDepthPrefetch.serial == gGfxDepthSerial && sDepthPrefetch.w == gGfxDepthW &&
           sDepthPrefetch.h == gGfxDepthH;
}

static u8 *depthHostBuffer(void) {
    if (sDepthPrefetch.data == NULL) {
        sDepthPrefetch.data = malloc(640 * 480 * 4);
        if (sDepthPrefetch.data == NULL) {
            portAbort("PORT: depth readback out of memory");
        }
    }
    return sDepthPrefetch.data;
}

/* copies the prefetched depth while the GPU queue holds nothing newer: a later read would wait for the GPU process
 * to get through everything queued after it */
void gfxFbDepthCollect(void) {
    if (!depthPrefetchCurrent() || sDepthPrefetch.collected) {
        return;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, sDepthPrefetch.pbo);
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, depthPackedW() * sDepthPrefetch.h * 4, depthHostBuffer());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    sDepthPrefetch.collected = 1;
}

/* the depth image as packed RDP z in host memory: the prefetch, or a readback now that serves the rest of the
 * frame */
static const u8 *depthPixels(void) {
    if (depthPrefetchCurrent()) {
        gfxFbDepthCollect();
    } else {
        depthToReadTarget();
        glReadPixels(0, 0, depthPackedW(), gGfxDepthH, GL_RGBA, GL_UNSIGNED_BYTE, depthHostBuffer());
        sDepthPrefetch.w = gGfxDepthW;
        sDepthPrefetch.h = gGfxDepthH;
        sDepthPrefetch.serial = gGfxDepthSerial;
        sDepthPrefetch.valid = 1;
        sDepthPrefetch.collected = 1;
    }
    return sDepthPrefetch.data;
}

/* RDRAM [addr, addr + len) of the depth image: the game reads single pixels (zbufferReadDepth), so only those are
 * copied; the rest of RDRAM stays as it was */
static void syncDepth(u32 addr, u32 len) {
    u32 w = gGfxDepthW, h = gGfxDepthH, pitch = depthPackedW() * 2, i;
    u16 *dst = (u16 *)gGfx.zimg;
    u32 lo = addr > gGfx.zimg ? (addr - gGfx.zimg) / 2 : 0;
    u32 hi = (addr + len - gGfx.zimg + 1) / 2;
    const u8 *px = depthPixels();
    if (hi > w * h) {
        hi = w * h; /* RDRAM past the image belongs to someone else */
    }
    for (i = lo; i < hi; i++) {
        memcpy(&dst[i], &px[((h - 1 - i / w) * pitch + i % w) * 2], 2);
    }
}

void gfxFbSyncRdram(u32 addr, u32 len) {
    int i;
    u32 zend = gGfx.zimg + gGfxDepthW * gGfxDepthH * 2;
    if (gGfxDepthDirty && gGfx.zimg != 0 && addr < zend && addr + len > gGfx.zimg) {
        syncDepth(addr, len);
        if (addr <= gGfx.zimg && addr + len >= zend) {
            gGfxDepthDirty = 0; /* all of it is current; a partial read leaves the rest stale */
        }
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
