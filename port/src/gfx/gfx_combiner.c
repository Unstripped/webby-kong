/* RDP colour combiner -> GLSL ES 3.00: (a - b) * c + d per channel group, one or two cycles. */
#include "gfx/gfx_gl.h"
#include <math.h>
#include <stdio.h>

#define MAX_PROGRAMS 512

typedef struct {
    GfxProgramKey key;
    GfxProgram prog;
} ProgramEntry;

static ProgramEntry sPrograms[MAX_PROGRAMS];
static int sProgramCount;
/* compiled vertex shaders by source: every combiner program shares one */
static struct {
    const char *src;
    GLuint shader;
} sVertexShaders[4];
static int sVertexShaderCount;

void gfxCombinerQsRanges(QsSink *out) {
    QS_KEEP(out, sPrograms);
    QS_KEEP(out, sProgramCount);
    QS_KEEP(out, sVertexShaders);
    QS_KEEP(out, sVertexShaderCount);
}

static const char *kVertex =
    "#version 300 es\n"
    "in vec4 aPos;\n"
    "in vec4 aColor;\n"
    "in vec2 aSt;\n"
    "out vec4 vShade;\n"
    "out vec2 vSt;\n"
    "void main() { gl_Position = aPos; vShade = aColor; vSt = aSt; }\n";

/* status is not queried here: each query is a synchronous round trip to the GPU process (a build stalls the frame
 * it happens in), so only a failed link fetches the compile logs */
static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static GLuint vertexShader(const char *src) {
    int i;
    for (i = 0; i < sVertexShaderCount; i++) {
        if (sVertexShaders[i].src == src) {
            return sVertexShaders[i].shader;
        }
    }
    if (sVertexShaderCount == (int)(sizeof sVertexShaders / sizeof sVertexShaders[0])) {
        portAbort("PORT: more than %d vertex shaders", sVertexShaderCount);
    }
    sVertexShaders[sVertexShaderCount].src = src;
    sVertexShaders[sVertexShaderCount].shader = compile(GL_VERTEX_SHADER, src);
    return sVertexShaders[sVertexShaderCount++].shader;
}

GLuint gfxCompileProgram(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    GLuint v = vertexShader(vs), f = compile(GL_FRAGMENT_SHADER, fs);
    GLint ok = 0;
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aColor");
    glBindAttribLocation(p, 2, "aSt");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char vlog[1024], flog[1024], plog[1024];
        glGetShaderInfoLog(v, sizeof vlog, NULL, vlog);
        glGetShaderInfoLog(f, sizeof flog, NULL, flog);
        glGetProgramInfoLog(p, sizeof plog, NULL, plog);
        portAbort("PORT: shader link failed: %s\nvertex: %s\nfragment: %s\n%s", plog, vlog, flog, fs);
    }
    glDetachShader(p, v);
    glDetachShader(p, f);
    glDeleteShader(f);
    return p;
}

/* ---- mux decode ---- */
static const char *colorIn(int slot, int v, u8 *tex) {
    if (v == 1) tex[0] = 1;
    if (v == 2) tex[1] = 1;
    if (slot == 2) {
        switch (v) {
        case 8: tex[0] = 1; break;
        case 9: tex[1] = 1; break;
        }
    }
    switch (v) {
    case 0: return "comb.rgb";
    case 1: return "t0.rgb";
    case 2: return "t1.rgb";
    case 3: return "uPrim.rgb";
    case 4: return "vShade.rgb";
    case 5: return "uEnv.rgb";
    }
    switch (slot) {
    case 0: /* a */
        return v == 6 ? "vec3(1.0)" : v == 7 ? "vec3(noise())" : "vec3(0.0)";
    case 1: /* b: CENTER, K4 = 0 */
        return "vec3(0.0)";
    case 2: /* c */
        switch (v) {
        case 7: return "vec3(comb.a)";
        case 8: return "vec3(t0.a)";
        case 9: return "vec3(t1.a)";
        case 10: return "vec3(uPrim.a)";
        case 11: return "vec3(vShade.a)";
        case 12: return "vec3(uEnv.a)";
        case 13: return "vec3(lodFrac)";
        case 14: return "vec3(uPrimLod)";
        default: return "vec3(0.0)"; /* SCALE, K5, 0 */
        }
    default: /* d */
        return v == 6 ? "vec3(1.0)" : "vec3(0.0)";
    }
}

static const char *alphaIn(int slot, int v, u8 *tex) {
    if (v == 1) tex[0] = 1;
    if (v == 2) tex[1] = 1;
    if (slot == 2) {
        switch (v) {
        case 0: return "lodFrac";
        case 6: return "uPrimLod";
        }
    } else if (v == 0) {
        return "comb.a";
    }
    switch (v) {
    case 1: return "t0.a";
    case 2: return "t1.a";
    case 3: return "uPrim.a";
    case 4: return "vShade.a";
    case 5: return "uEnv.a";
    case 6: return "1.0";
    default: return "0.0";
    }
}

static void emitCycle(char *out, size_t cap, int cycle, u32 hi, u32 lo, u8 *tex) {
    int a, b, c, d, aa, ab, ac, ad;
    char buf[1024];
    if (cycle == 0) {
        a = (hi >> 20) & 0xF; c = (hi >> 15) & 0x1F; aa = (hi >> 12) & 7; ac = (hi >> 9) & 7;
        b = (lo >> 28) & 0xF; d = (lo >> 15) & 7; ab = (lo >> 12) & 7; ad = (lo >> 9) & 7;
    } else {
        a = (hi >> 5) & 0xF; c = hi & 0x1F; aa = (lo >> 21) & 7; ac = (lo >> 18) & 7;
        b = (lo >> 24) & 0xF; d = (lo >> 6) & 7; ab = (lo >> 3) & 7; ad = lo & 7;
    }
    snprintf(buf, sizeof buf,
             "    comb = vec4((%s - %s) * %s + %s, (%s - %s) * %s + %s);\n",
             colorIn(0, a, tex), colorIn(1, b, tex), colorIn(2, c, tex), colorIn(3, d, tex),
             alphaIn(0, aa, tex), alphaIn(1, ab, tex), alphaIn(2, ac, tex), alphaIn(3, ad, tex));
    strncat(out, buf, cap - strlen(out) - 1);
}

static void nanFill(float *c, int n) {
    while (n-- > 0) {
        *c++ = NAN;
    }
}

static GfxProgram build(const GfxProgramKey *k) {
    static char fs[8192];
    GfxProgram p;
    u8 tex[2] = { 0, 0 };
    char line[128];
    memset(&p, 0, sizeof p);
    fs[0] = 0;
    strcat(fs,
           "#version 300 es\n"
           "precision highp float;\n"
           "in vec4 vShade;\n"
           "in vec2 vSt;\n"
           "uniform sampler2D uTex0;\n"
           "uniform sampler2D uTex1;\n"
           "uniform vec4 uPrim, uEnv, uFog, uBlend;\n"
           "uniform float uPrimLod, uAlphaRef, uPrimDepth;\n"
           "uniform vec4 uTile0, uTile1;\n"
           "uniform vec2 uSize0, uSize1;\n"
           "uniform float uLodScale, uMaxLevel;\n"
           "uniform vec4 uMipTile[8];\n"
           "uniform vec2 uMipSize[8];\n"
           "out vec4 fragColor;\n"
           "float noise() { return fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453); }\n"
           "void main() {\n"
           "    float lodFrac = 0.0;\n");
    if (k->lod) {
        /* RDP texture LOD (no detail/sharpen): tile0 texels per N64 pixel picks mip level l = floor(log2(lod)),
         * TEXEL0/TEXEL1 are levels l and l + 1, LOD_FRAC = lod / 2^l - 1; magnified: level 0, frac 0;
         * past the last level: that level, frac 1. uTex0 holds the levels as GL mips; each level samples with its
         * own tile's shift and ul offset (a scrolled chain moves every level by the same texel count). */
        strcat(fs,
               "    vec2 uv = vSt * uTile0.xy - uTile0.zw;\n"
               "    vec2 dx = abs(dFdx(uv)), dy = abs(dFdy(uv));\n"
               "    float lod = max(max(dx.x, dx.y), max(dy.x, dy.y)) * uLodScale;\n"
               "    float lvl = 0.0;\n"
               "    if (lod >= 1.0) { lvl = floor(log2(lod)); lodFrac = lod / exp2(lvl) - 1.0; }\n"
               "    if (lvl >= uMaxLevel) { lvl = uMaxLevel; lodFrac = 1.0; }\n"
               "    float lvl1 = min(lvl + 1.0, uMaxLevel);\n"
               "    int i0 = int(lvl), i1 = int(lvl1);\n"
               "    vec4 t0 = textureLod(uTex0, (vSt * uMipTile[i0].xy - uMipTile[i0].zw) / uMipSize[i0], lvl);\n"
               "    vec4 t1 = textureLod(uTex0, (vSt * uMipTile[i1].xy - uMipTile[i1].zw) / uMipSize[i1], lvl1);\n");
    } else {
        strcat(fs,
               "    vec4 t0 = texture(uTex0, (vSt * uTile0.xy - uTile0.zw) / uSize0);\n"
               "    vec4 t1 = texture(uTex1, (vSt * uTile1.xy - uTile1.zw) / uSize1);\n");
    }
    strcat(fs, "    vec4 comb = vec4(0.0);\n");
    if (k->cycle == 2) {
        strcat(fs, "    comb = t0;\n");
        tex[0] = 1;
    } else if (k->cycle == 3) {
        strcat(fs, "    comb = vShade;\n");
    } else {
        emitCycle(fs, sizeof fs, 0, k->combineHi, k->combineLo, tex);
        if (k->cycle == 1) {
            strcat(fs, "    comb = clamp(comb, 0.0, 1.0);\n");
            emitCycle(fs, sizeof fs, 1, k->combineHi, k->combineLo, tex);
        }
    }
    strcat(fs, "    comb = clamp(comb, 0.0, 1.0);\n");
    if (k->fogBlend) {
        strcat(fs, "    comb.rgb = mix(comb.rgb, uFog.rgb, vShade.a);\n");
    }
    if (k->alphaMode != 0) {
        const char *ref = k->alphaMode == 1 ? "uAlphaRef" : "0.5";
        if (k->a2c && k->alphaMode == 2) {
            /* coverage mode (G_AC_DITHER, CVG_X_ALPHA): the RDP scales coverage by alpha, which is what
             * GL_SAMPLE_ALPHA_TO_COVERAGE does with the combined alpha, so it passes through untouched */
        } else if (k->a2c) {
            /* GL_SAMPLE_ALPHA_TO_COVERAGE turns this alpha into a sample mask. Every pixel the discard kept
             * (alpha at or above the reference) keeps full coverage; only the pixels it killed can gain samples,
             * across the one screen pixel the alpha ramp needs to reach the reference. A flat alpha then still
             * gives the hard cutout, and a reference of 0 or 1 cannot halve a surface. Coverage 0 writes no
             * sample, so no discard is needed. */
            snprintf(line, sizeof line,
                     "    comb.a = comb.a >= %s ? 1.0 :"
                     " clamp((comb.a - %s) / max(fwidth(comb.a), 1e-4) + 1.0, 0.0, 1.0);\n",
                     ref, ref);
            strcat(fs, line);
        } else {
            snprintf(line, sizeof line, "    if (comb.a < %s) discard;\n", ref);
            strcat(fs, line);
        }
    }
    strcat(fs, "    fragColor = comb;\n");
    if (k->primDepth) {
        strcat(fs, "    gl_FragDepth = uPrimDepth;\n");
    }
    strcat(fs, "}\n");
    p.prog = gfxCompileProgram(kVertex, fs);
    p.uTex0 = glGetUniformLocation(p.prog, "uTex0");
    p.uTex1 = glGetUniformLocation(p.prog, "uTex1");
    p.uPrim = glGetUniformLocation(p.prog, "uPrim");
    p.uEnv = glGetUniformLocation(p.prog, "uEnv");
    p.uFog = glGetUniformLocation(p.prog, "uFog");
    p.uBlend = glGetUniformLocation(p.prog, "uBlend");
    p.uPrimLod = glGetUniformLocation(p.prog, "uPrimLod");
    p.uTile0 = glGetUniformLocation(p.prog, "uTile0");
    p.uTile1 = glGetUniformLocation(p.prog, "uTile1");
    p.uSize0 = glGetUniformLocation(p.prog, "uSize0");
    p.uSize1 = glGetUniformLocation(p.prog, "uSize1");
    p.uAlphaRef = glGetUniformLocation(p.prog, "uAlphaRef");
    p.uPrimDepth = glGetUniformLocation(p.prog, "uPrimDepth");
    p.uLodScale = glGetUniformLocation(p.prog, "uLodScale");
    p.uMaxLevel = glGetUniformLocation(p.prog, "uMaxLevel");
    p.uMipTile = glGetUniformLocation(p.prog, "uMipTile");
    p.uMipSize = glGetUniformLocation(p.prog, "uMipSize");
    p.usesTex0 = tex[0];
    p.usesTex1 = tex[1];
    /* uniform shadows start unknown: NaN compares unequal to every value */
    nanFill(p.cPrim, 4);
    nanFill(p.cEnv, 4);
    nanFill(p.cFog, 4);
    nanFill(p.cBlend, 4);
    nanFill(p.cTile0, 4);
    nanFill(p.cTile1, 4);
    nanFill(p.cSize0, 2);
    nanFill(p.cSize1, 2);
    nanFill(&p.cPrimLod, 1);
    nanFill(&p.cAlphaRef, 1);
    nanFill(&p.cPrimDepth, 1);
    nanFill(&p.cLodScale, 1);
    nanFill(&p.cMaxLevel, 1);
    glUseProgram(p.prog);
    glUniform1i(p.uTex0, 0);
    glUniform1i(p.uTex1, 1);
    gGfxProgramBuilt = 1;
    return p;
}

int gGfxProgramBuilt;

static int sLastProgram = -1; /* gfxProgramGet's one-entry hint; every hit still compares the whole key */

/* the alpha-to-coverage setting (and MSAA, which gates it) changed: drop the programs built for the old one, so a
 * session that toggles it does not keep both variants of every alpha-tested combiner against MAX_PROGRAMS */
void gfxCombinerDropA2c(void) {
    int i = 0;
    while (i < sProgramCount) {
        if (sPrograms[i].key.a2c) {
            glDeleteProgram(sPrograms[i].prog.prog);
            sPrograms[i] = sPrograms[--sProgramCount];
        } else {
            i++;
        }
    }
    sLastProgram = -1;
}

GfxProgram *gfxProgramGet(const GfxProgramKey *key) {
    int i;
    if (sLastProgram >= 0 && sLastProgram < sProgramCount &&
        memcmp(&sPrograms[sLastProgram].key, key, sizeof *key) == 0) {
        return &sPrograms[sLastProgram].prog;
    }
    for (i = 0; i < sProgramCount; i++) {
        if (memcmp(&sPrograms[i].key, key, sizeof *key) == 0) {
            sLastProgram = i;
            return &sPrograms[i].prog;
        }
    }
    if (sProgramCount == MAX_PROGRAMS) {
        portAbort("PORT: combiner program cache full");
    }
    sPrograms[sProgramCount].key = *key;
    sPrograms[sProgramCount].prog = build(key);
    sLastProgram = sProgramCount;
    return &sPrograms[sProgramCount++].prog;
}
