/* RDP colour combiner -> GLSL ES 3.00: (a - b) * c + d per channel group, one or two cycles. */
#include "gfx/gfx_gl.h"
#include <stdio.h>

#define MAX_PROGRAMS 512

typedef struct {
    GfxProgramKey key;
    GfxProgram prog;
} ProgramEntry;

static ProgramEntry sPrograms[MAX_PROGRAMS];
static int sProgramCount;

int gfxCombinerQsRanges(QsRange *out) {
    out[0] = QS_RANGE(sPrograms);
    out[1] = QS_RANGE(sProgramCount);
    return 2;
}

static const char *kVertex =
    "#version 300 es\n"
    "in vec4 aPos;\n"
    "in vec4 aColor;\n"
    "in vec2 aSt;\n"
    "out vec4 vShade;\n"
    "out vec2 vSt;\n"
    "void main() { gl_Position = aPos; vShade = aColor; vSt = aSt; }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        portAbort("PORT: shader compile failed: %s\n%s", log, src);
    }
    return s;
}

GLuint gfxCompileProgram(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    GLint ok = 0;
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aColor");
    glBindAttribLocation(p, 2, "aSt");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        portAbort("PORT: shader link failed: %s", log);
    }
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
        case 14: return "vec3(uPrimLod)";
        default: return "vec3(0.0)"; /* SCALE, LOD_FRAC, K5, 0 */
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
        case 0: return "0.0"; /* LOD_FRACTION */
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

static GfxProgram build(const GfxProgramKey *k) {
    static char fs[8192];
    GfxProgram p;
    u8 tex[2] = { 0, 0 };
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
           "out vec4 fragColor;\n"
           "float noise() { return fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453); }\n"
           "void main() {\n"
           "    vec4 t0 = texture(uTex0, (vSt * uTile0.xy - uTile0.zw) / uSize0);\n"
           "    vec4 t1 = texture(uTex1, (vSt * uTile1.xy - uTile1.zw) / uSize1);\n"
           "    vec4 comb = vec4(0.0);\n");
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
    if (k->alphaMode == 1) {
        strcat(fs, "    if (comb.a < uAlphaRef) discard;\n");
    } else if (k->alphaMode == 2) {
        strcat(fs, "    if (comb.a < 0.5) discard;\n");
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
    p.usesTex0 = tex[0];
    p.usesTex1 = tex[1];
    glUseProgram(p.prog);
    glUniform1i(p.uTex0, 0);
    glUniform1i(p.uTex1, 1);
    return p;
}

const GfxProgram *gfxProgramGet(const GfxProgramKey *key) {
    int i;
    for (i = 0; i < sProgramCount; i++) {
        if (memcmp(&sPrograms[i].key, key, sizeof *key) == 0) {
            return &sPrograms[i].prog;
        }
    }
    if (sProgramCount == MAX_PROGRAMS) {
        portAbort("PORT: combiner program cache full");
    }
    sPrograms[sProgramCount].key = *key;
    sPrograms[sProgramCount].prog = build(key);
    return &sPrograms[sProgramCount++].prog;
}
