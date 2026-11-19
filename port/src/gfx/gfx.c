/* F3DEX2 + RDP display-list interpreter (Fast3D-style HLE): CPU vertex transform and lighting,
 * triangles batched per RDP state and handed to the backend. Runs on the host context. */
#include "gfx/gfx.h"
#include <math.h>
#include <stdarg.h>

#define MAX_VTX 64
#define MAX_MTX_STACK 32
#define MAX_DL_DEPTH 18
#define MAX_LIGHTS 8
#define BATCH_MAX 3 * 1024

GfxState gGfx;
float gGfxTargetWide = 1.0f;

typedef struct {
    float x, y, z, w;      /* clip space */
    float r, g, b, a;      /* 0..1 */
    float s, t;            /* texels (render tile scale applied) */
    u8 clip;               /* outcode */
    float sx, sy, sz;      /* screen position (N64 pixels) for G_BRANCH_Z / MODIFYVTX */
} RspVtx;

typedef struct {
    u8 col[3];
    s8 dir[3];
    float dirObj[3];
} RspLight;

static struct {
    u32 seg[16];
    float mv[MAX_MTX_STACK][4][4];
    int mvDepth;
    float proj[4][4];
    float mvp[4][4];
    int mvpDirty;
    RspVtx vtx[MAX_VTX];
    RspLight lights[MAX_LIGHTS + 1];
    int numLights;
    int lightsDirty;
    s8 lookat[2][3];
    float lookatObj[2][3];
    s16 fogMul, fogOff;
    float texScaleS, texScaleT;
    s16 vpScale[4], vpTrans[4];
    u32 rdpHalf1, rdpHalf2;
    u32 timg, timgFmt, timgSiz, timgWidth;
    u32 persp;
} R;

static GfxVtx sBatch[BATCH_MAX];
static u32 sCurW0, sCurW1; /* command being interpreted, for diagnostics */
static u32 sCurCmd, sDlStack[MAX_DL_DEPTH + 2];
static int sDlDepth;

static const char *dlStackString(void) {
    static char buf[512];
    int i, n = 0;
    buf[0] = 0;
    for (i = 0; i <= sDlDepth && n < (int)sizeof buf - 40; i++) {
        const u32 *c = (const u32 *)sDlStack[i];
        int ok = c && sDlStack[i] >= 0x408;
        n += snprintf(buf + n, sizeof buf - n, "%s0x%x[%08x %08x after %08x %08x]", i ? " > " : "", sDlStack[i],
                      ok ? c[0] : 0, ok ? c[1] : 0, ok ? c[-2] : 0, ok ? c[-1] : 0);
    }
    return buf;
}
static u32 sBatchCount;

/* ---------------------------------------------------------------- utilities */

void gfxWarnOnce(const char *key, const char *fmt, ...) {
    static char seen[64][32];
    static int nseen;
    int i;
    va_list ap;
    for (i = 0; i < nseen; i++) {
        if (strncmp(seen[i], key, 31) == 0) {
            return;
        }
    }
    if (nseen < 64) {
        strncpy(seen[nseen++], key, 31);
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

u8 *gfxHost(u32 a) {
    u32 host = R.seg[(a >> 24) & 0xF] + (a & 0x00FFFFFF);
    host = portHostAddr(host);
    if (host >= PORT_GAME_LIMIT) {
        portAbort("PORT: gfx address 0x%08x resolves outside game memory (0x%08x) in command %08x %08x at 0x%x, "
                  "list stack %s", a, host, sCurW0, sCurW1, sCurCmd, dlStackString());
    }
    if (host < 0x400) {
        /* the RSP would read RDRAM's first page; reads are harmless here, display lists there are skipped */
        char key[16];
        snprintf(key, sizeof key, "null%02x", (unsigned)(sCurW0 >> 24));
        gfxWarnOnce(key, "PORT: gfx null-page address 0x%08x in command %08x %08x", a, sCurW0, sCurW1);
    }
    return (u8 *)host;
}

static void flush(void) {
    if (sBatchCount != 0) {
        gfxBackendDrawTriangles(&gGfx, sBatch, sBatchCount);
        sBatchCount = 0;
    }
}

static void mtxMul(float out[4][4], float a[4][4], float b[4][4]) {
    float r[4][4];
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(out, r, sizeof r);
}

/* Mtx: 16 native s32 words; integer halves in words 0..7, fractions in 8..15 */
static void mtxLoad(float out[4][4], const u8 *p) {
    const u32 *w = (const u32 *)p;
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j += 2) {
            u32 iw = w[i * 2 + j / 2];
            u32 fw = w[8 + i * 2 + j / 2];
            s32 a = (s32)((iw & 0xFFFF0000u) | (fw >> 16));
            s32 b = (s32)((iw << 16) | (fw & 0xFFFFu));
            out[i][j] = a / 65536.0f;
            out[i][j + 1] = b / 65536.0f;
        }
    }
}

static void updateMvp(void) {
    if (R.mvpDirty) {
        mtxMul(R.mvp, R.mv[R.mvDepth], R.proj);
        R.mvpDirty = 0;
    }
}

static void normalize3(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

/* light/lookat directions into object space: transpose(modelview) * dir */
static void toObject(const s8 in[3], float out[3]) {
    float (*m)[4] = R.mv[R.mvDepth];
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
    }
    normalize3(out);
}

static void updateLights(void) {
    int i;
    if (!R.lightsDirty) {
        return;
    }
    for (i = 0; i < R.numLights; i++) {
        toObject(R.lights[i].dir, R.lights[i].dirObj);
    }
    toObject(R.lookat[0], R.lookatObj[0]);
    toObject(R.lookat[1], R.lookatObj[1]);
    R.lightsDirty = 0;
}

/* ---------------------------------------------------------------- vertices */

static void loadVertices(u32 addr, int n, int dst) {
    const Vtx *src = (const Vtx *)gfxHost(addr);
    int i;
    float vx = R.vpScale[0] / 4.0f, vy = R.vpScale[1] / 4.0f, tx = R.vpTrans[0] / 4.0f, ty = R.vpTrans[1] / 4.0f;
    /* x outcodes test the displayed span, not the viewport: a wide target shows the side areas and the game's
     * overscan border around the viewport too, so trivial rejects and G_CULLDL must keep what lands there.
     * Screen x relative to the viewport centre, as x * vx / w. */
    float fbHalf = (gGfx.cimgWidth ? gGfx.cimgWidth : 320) * 0.5f;
    float xLo = fbHalf - fbHalf * gGfxTargetWide - tx, xHi = fbHalf + fbHalf * gGfxTargetWide - tx;
    if (dst < 0 || dst + n > MAX_VTX) {
        gfxWarnOnce("vtxrange", "PORT: gfx G_VTX range %d+%d", dst, n);
        return;
    }
    if (dst + n > 32) {
        gfxWarnOnce("vtx32", "PORT: gfx G_VTX beyond 32 entries (%d+%d)", dst, n);
    }
    updateMvp();
    if (gGfx.geometry & G_LIGHTING) {
        updateLights();
    }
    for (i = 0; i < n; i++) {
        const Vtx_t *v = &src[i].v;
        RspVtx *d = &R.vtx[dst + i];
        float x = v->ob[0], y = v->ob[1], z = v->ob[2];
        float (*m)[4] = R.mvp;
        float s = v->tc[0], t = v->tc[1];
        d->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        d->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        d->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        d->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        if (gGfx.geometry & G_LIGHTING) {
            const Vtx_tn *vn = &src[i].n;
            float nrm[3] = { vn->n[0], vn->n[1], vn->n[2] };
            float r, g, b;
            int l;
            normalize3(nrm);
            r = R.lights[R.numLights].col[0];
            g = R.lights[R.numLights].col[1];
            b = R.lights[R.numLights].col[2];
            for (l = 0; l < R.numLights; l++) {
                float dp = nrm[0] * R.lights[l].dirObj[0] + nrm[1] * R.lights[l].dirObj[1] + nrm[2] * R.lights[l].dirObj[2];
                if (dp > 0) {
                    r += dp * R.lights[l].col[0];
                    g += dp * R.lights[l].col[1];
                    b += dp * R.lights[l].col[2];
                }
            }
            d->r = (r > 255 ? 255 : r) / 255.0f;
            d->g = (g > 255 ? 255 : g) / 255.0f;
            d->b = (b > 255 ? 255 : b) / 255.0f;
            if (gGfx.geometry & G_TEXTURE_GEN) {
                float dx = nrm[0] * R.lookatObj[0][0] + nrm[1] * R.lookatObj[0][1] + nrm[2] * R.lookatObj[0][2];
                float dy = nrm[0] * R.lookatObj[1][0] + nrm[1] * R.lookatObj[1][1] + nrm[2] * R.lookatObj[1][2];
                if (gGfx.geometry & G_TEXTURE_GEN_LINEAR) {
                    dx = acosf(-dx) / (float)M_PI * 2.0f - 1.0f;
                    dy = acosf(-dy) / (float)M_PI * 2.0f - 1.0f;
                }
                /* (dot + 1) / 4 of the texture scale, in s10.5 units */
                s = (dx + 1.0f) / 4.0f * (R.texScaleS * 65536.0f);
                t = (dy + 1.0f) / 4.0f * (R.texScaleT * 65536.0f);
                d->s = s / 32.0f;
                d->t = t / 32.0f;
                goto texdone;
            }
        } else {
            d->r = v->cn[0] / 255.0f;
            d->g = v->cn[1] / 255.0f;
            d->b = v->cn[2] / 255.0f;
        }
        d->s = s * R.texScaleS / 32.0f;
        d->t = t * R.texScaleT / 32.0f;
    texdone:
        d->a = v->cn[3] / 255.0f;
        if (gGfx.geometry & G_FOG) {
            float wi = d->w != 0 ? 1.0f / d->w : 1.0f;
            float f = d->z * wi * R.fogMul + R.fogOff;
            if (d->w < 0) {
                f = -f;
            }
            if (f < 0) f = 0;
            if (f > 255) f = 255;
            d->a = f / 255.0f;
        }
        d->clip = 0;
        if (d->x * vx < d->w * xLo) d->clip |= 1;
        if (d->x * vx > d->w * xHi) d->clip |= 2;
        if (d->y < -d->w) d->clip |= 4;
        if (d->y > d->w) d->clip |= 8;
        if (d->z < -d->w) d->clip |= 16;
        if (d->z > d->w) d->clip |= 32;
        if (d->w != 0) {
            d->sx = d->x / d->w * vx + tx;
            d->sy = -d->y / d->w * vy + ty;
            d->sz = (d->z / d->w * (R.vpScale[2] / 4.0f) + R.vpTrans[2] / 4.0f) * 32.0f;
        } else {
            d->sx = d->sy = d->sz = 0;
        }
    }
}

/* clip coords from the N64 viewport into GL NDC of the current color image */
static void emitVertex(const RspVtx *v) {
    GfxVtx *o;
    float fbW = (float)(gGfx.cimgWidth ? gGfx.cimgWidth : 320);
    float fbH = fbW * 3.0f / 4.0f;
    float sx = R.vpScale[0] / 4.0f, sy = R.vpScale[1] / 4.0f;
    float tx = R.vpTrans[0] / 4.0f, ty = R.vpTrans[1] / 4.0f;
    if (sBatchCount >= BATCH_MAX) {
        flush();
    }
    o = &sBatch[sBatchCount++];
    /* screen_x = x/w*sx + tx ; ndc = screen_x/(W/2) - 1, squeezed into the centre of a wide target */
    o->x = (v->x * (sx / (fbW / 2)) + v->w * (tx / (fbW / 2) - 1.0f)) / gGfxTargetWide;
    o->y = v->y * (sy / (fbH / 2)) + v->w * (1.0f - ty / (fbH / 2));
    o->z = v->z;
    o->w = v->w;
    o->r = v->r;
    o->g = v->g;
    o->b = v->b;
    o->a = v->a;
    o->s = v->s;
    o->t = v->t;
}

static void triangle(int a, int b, int c) {
    RspVtx *v0, *v1, *v2;
    if (a >= MAX_VTX || b >= MAX_VTX || c >= MAX_VTX) {
        gfxWarnOnce("triidx", "PORT: gfx triangle index out of range");
        return;
    }
    v0 = &R.vtx[a];
    v1 = &R.vtx[b];
    v2 = &R.vtx[c];
    if (v0->clip & v1->clip & v2->clip) {
        return;
    }
    if (gGfx.geometry & G_CULL_BOTH) {
        /* winding in clip space projected (w-signs handled by the cross product of x/w, y/w) */
        float dx1 = v1->x / v1->w - v0->x / v0->w, dy1 = v1->y / v1->w - v0->y / v0->w;
        float dx2 = v2->x / v2->w - v0->x / v0->w, dy2 = v2->y / v2->w - v0->y / v0->w;
        float cross = dx1 * dy2 - dy1 * dx2;
        if ((v0->w < 0) ^ (v1->w < 0) ^ (v2->w < 0)) {
            cross = -cross;
        }
        if ((gGfx.geometry & G_CULL_BOTH) == G_CULL_BOTH) {
            return;
        }
        if ((gGfx.geometry & G_CULL_FRONT) && cross >= 0) {
            return;
        }
        if ((gGfx.geometry & G_CULL_BACK) && cross <= 0) {
            return;
        }
    }
    emitVertex(v0);
    emitVertex(v1);
    emitVertex(v2);
}

/* ---------------------------------------------------------------- state changes */

static void setOtherMode(u32 *mode, u32 w0, u32 w1) {
    u32 len = (w0 & 0xFF) + 1;
    u32 shift = 32 - ((w0 >> 8) & 0xFF) - len;
    u32 mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
    flush();
    *mode = (*mode & ~mask) | (w1 & mask);
}

static void moveMem(u32 w0, u32 w1) {
    u32 idx = w0 & 0xFF;
    u32 ofs = ((w0 >> 8) & 0xFF) * 8;
    u8 *p = gfxHost(w1);
    switch (idx) {
    case G_MV_VIEWPORT: {
        const s16 *vp = (const s16 *)p;
        int i;
        flush();
        for (i = 0; i < 4; i++) {
            R.vpScale[i] = vp[i];
            R.vpTrans[i] = vp[4 + i];
        }
        break;
    }
    case G_MV_LIGHT:
        if (ofs < 0x30) {
            int k = ofs / 0x18;
            memcpy(R.lookat[k], p + 8, 3);
        } else {
            int k = (ofs - 0x30) / 0x18;
            if (k <= MAX_LIGHTS) {
                memcpy(R.lights[k].col, p, 3);
                memcpy(R.lights[k].dir, p + 8, 3);
            }
        }
        R.lightsDirty = 1;
        break;
    case G_MV_MATRIX:
        mtxLoad(R.mvp, p);
        break;
    default:
        gfxWarnOnce("movemem", "PORT: gfx G_MOVEMEM index %u ignored", idx);
        break;
    }
}

static void moveWord(u32 w0, u32 w1) {
    u32 index = (w0 >> 16) & 0xFF;
    u32 offset = w0 & 0xFFFF;
    switch (index) {
    case G_MW_SEGMENT:
        R.seg[(offset / 4) & 0xF] = portHostAddr(w1) & 0x00FFFFFF;
        break;
    case G_MW_NUMLIGHT:
        R.numLights = (int)(w1 / 24);
        if (R.numLights > MAX_LIGHTS) {
            R.numLights = MAX_LIGHTS;
        }
        R.lightsDirty = 1;
        break;
    case G_MW_FOG:
        R.fogMul = (s16)(w1 >> 16);
        R.fogOff = (s16)(w1 & 0xFFFF);
        break;
    case G_MW_LIGHTCOL: {
        int k = offset / 0x18;
        if (k <= MAX_LIGHTS) {
            R.lights[k].col[0] = w1 >> 24;
            R.lights[k].col[1] = w1 >> 16;
            R.lights[k].col[2] = w1 >> 8;
        }
        break;
    }
    case G_MW_PERSPNORM:
        R.persp = w1;
        break;
    case G_MW_MATRIX: {
        /* one element pair of the combined matrix: offset selects int/frac word */
        int word = (offset / 4) & 15;
        int frac = (offset & 0x20) != 0;
        int row = (word & 7) / 2, col = ((word & 7) % 2) * 2;
        float a, b;
        (void)frac;
        a = (s16)(w1 >> 16);
        b = (s16)(w1 & 0xFFFF);
        if (offset & 0x20) {
            R.mvp[row][col] = (float)(s32)R.mvp[row][col] + (u16)(w1 >> 16) / 65536.0f;
            R.mvp[row][col + 1] = (float)(s32)R.mvp[row][col + 1] + (u16)(w1 & 0xFFFF) / 65536.0f;
        } else {
            R.mvp[row][col] = a;
            R.mvp[row][col + 1] = b;
        }
        break;
    }
    case G_MW_CLIP:
        break;
    default:
        gfxWarnOnce("moveword", "PORT: gfx G_MOVEWORD index %u ignored", index);
        break;
    }
}

static void matrix(u32 w0, u32 w1) {
    u32 p = (w0 & 0xFF) ^ G_MTX_PUSH;
    float m[4][4];
    mtxLoad(m, gfxHost(w1));
    flush();
    if (p & G_MTX_PROJECTION) {
        if (p & G_MTX_LOAD) {
            memcpy(R.proj, m, sizeof m);
        } else {
            mtxMul(R.proj, m, R.proj);
        }
    } else {
        if ((p & G_MTX_PUSH) && R.mvDepth < MAX_MTX_STACK - 1) {
            memcpy(R.mv[R.mvDepth + 1], R.mv[R.mvDepth], sizeof m);
            R.mvDepth++;
        }
        if (p & G_MTX_LOAD) {
            memcpy(R.mv[R.mvDepth], m, sizeof m);
        } else {
            mtxMul(R.mv[R.mvDepth], m, R.mv[R.mvDepth]);
        }
        R.lightsDirty = 1;
    }
    R.mvpDirty = 1;
}

static void setTile(u32 w0, u32 w1) {
    GfxTile *t = &gGfx.tiles[(w1 >> 24) & 7];
    flush();
    t->fmt = (w0 >> 21) & 7;
    t->siz = (w0 >> 19) & 3;
    t->line = (w0 >> 9) & 0x1FF;
    t->tmem = w0 & 0x1FF;
    t->palette = (w1 >> 20) & 0xF;
    t->cmt = (w1 >> 18) & 3;
    t->maskt = (w1 >> 14) & 0xF;
    t->shiftt = (w1 >> 10) & 0xF;
    t->cms = (w1 >> 8) & 3;
    t->masks = (w1 >> 4) & 0xF;
    t->shifts = w1 & 0xF;
}

static void setTileSize(u32 w0, u32 w1) {
    GfxTile *t = &gGfx.tiles[(w1 >> 24) & 7];
    flush();
    t->uls = (w0 >> 12) & 0xFFF;
    t->ult = w0 & 0xFFF;
    t->lrs = (w1 >> 12) & 0xFFF;
    t->lrt = w1 & 0xFFF;
}

static void setColorImage(u32 w0, u32 w1) {
    flush();
    gGfx.cimg = (u32)gfxHost(w1);
    gGfx.cimgWidth = (w0 & 0xFFF) + 1;
    gGfx.cimgSiz = (w0 >> 19) & 3;
    gfxBackendSetColorImage(gGfx.cimg, gGfx.cimgWidth, gGfx.cimgSiz);
}

static void texRect(u32 w0, u32 w1, u32 h1, u32 h2, int flip) {
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    int tile = (w1 >> 24) & 7;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    float s = (s16)(h1 >> 16) / 32.0f, t = (s16)(h1 & 0xFFFF) / 32.0f;
    float dsdx = (s16)(h2 >> 16) / 1024.0f, dtdy = (s16)(h2 & 0xFFFF) / 1024.0f;
    u32 cyc = gGfx.otherH & (3u << G_MDSFT_CYCLETYPE);
    flush();
    if (cyc == G_CYC_COPY) {
        dsdx /= 4.0f;
        lrx += 1.0f;
        lry += 1.0f;
    } else if (cyc == G_CYC_FILL) {
        lrx += 1.0f;
        lry += 1.0f;
    }
    gfxBackendTexRect(&gGfx, tile, ulx, uly, lrx, lry, s, t, dsdx, dtdy, flip);
}

static void fillRect(u32 w0, u32 w1) {
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    u32 cyc = gGfx.otherH & (3u << G_MDSFT_CYCLETYPE);
    flush();
    if (cyc == G_CYC_FILL || cyc == G_CYC_COPY) {
        lrx += 1.0f;
        lry += 1.0f;
    }
    gfxBackendFillRect(&gGfx, ulx, uly, lrx, lry);
}

static int cullDl(int v0, int v1) {
    u8 all = 0xFF;
    int i;
    if (v0 < 0 || v1 >= MAX_VTX || v0 > v1) {
        return 0;
    }
    for (i = v0; i <= v1; i++) {
        all &= R.vtx[i].clip;
    }
    return all != 0;
}

static void runDl(u32 addr, int depth) {
    const u32 *cmd = (const u32 *)gfxHost(addr);
    if (depth > MAX_DL_DEPTH) {
        portAbort("PORT: gfx display list nesting deeper than %d", MAX_DL_DEPTH);
    }
    sDlStack[depth] = (u32)cmd;
    sDlDepth = depth;
    for (;;) {
        u32 w0 = cmd[0], w1 = cmd[1];
        u8 op = w0 >> 24;
        sCurW0 = w0;
        sCurW1 = w1;
        sCurCmd = (u32)cmd;
        sDlStack[depth] = (u32)cmd;
        sDlDepth = depth;
        cmd += 2;
        switch (op) {
        case G_NOOP:
        case G_SPNOOP:
        case G_RDPLOADSYNC:
        case G_RDPPIPESYNC:
        case G_RDPTILESYNC:
        case G_RDPFULLSYNC:
        case G_SETKEYGB:
        case G_SETKEYR:
        case G_SETCONVERT:
        case G_DMA_IO:
        case G_SPECIAL_1:
        case G_SPECIAL_2:
        case G_SPECIAL_3:
            break;
        case G_VTX: {
            int n = (w0 >> 12) & 0xFF;
            int end = (w0 >> 1) & 0x7F;
            loadVertices(w1, n, end - n);
            break;
        }
        case G_MODIFYVTX: {
            int where = (w0 >> 16) & 0xFF;
            int vi = (w0 & 0xFFFF) / 2;
            RspVtx *v;
            if (vi >= MAX_VTX) {
                break;
            }
            v = &R.vtx[vi];
            switch (where) {
            case G_MWO_POINT_RGBA:
                v->r = (w1 >> 24) / 255.0f;
                v->g = ((w1 >> 16) & 0xFF) / 255.0f;
                v->b = ((w1 >> 8) & 0xFF) / 255.0f;
                v->a = (w1 & 0xFF) / 255.0f;
                break;
            case G_MWO_POINT_ST:
                v->s = (s16)(w1 >> 16) * R.texScaleS / 32.0f;
                v->t = (s16)(w1 & 0xFFFF) * R.texScaleT / 32.0f;
                break;
            default:
                gfxWarnOnce("modvtx", "PORT: gfx G_MODIFYVTX where 0x%x ignored", where);
                break;
            }
            break;
        }
        case G_CULLDL:
            if (cullDl((w0 & 0xFFFF) / 2, (w1 & 0xFFFF) / 2)) {
                return;
            }
            break;
        case G_BRANCH_Z: {
            int vi = (w0 & 0xFFF) / 2;
            if (vi < MAX_VTX && R.vtx[vi].sz <= (float)(s32)w1) {
                cmd = (const u32 *)gfxHost(R.rdpHalf1);
            }
            break;
        }
        case G_TRI1:
            triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
            break;
        case G_TRI2:
            triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
            triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
            break;
        case G_QUAD:
            triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
            triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
            break;
        case G_TEXTURE:
            flush();
            gGfx.texTile = (w0 >> 8) & 7;
            gGfx.texLevels = (w0 >> 11) & 7;
            gGfx.texOn = ((w0 >> 1) & 0x7F) != 0;
            R.texScaleS = (w1 >> 16) / 65536.0f;
            R.texScaleT = (w1 & 0xFFFF) / 65536.0f;
            if ((w1 >> 16) == 0xFFFF) R.texScaleS = 1.0f;
            if ((w1 & 0xFFFF) == 0xFFFF) R.texScaleT = 1.0f;
            break;
        case G_POPMTX: {
            u32 n = w1 / 64;
            flush();
            R.mvDepth = (int)n > R.mvDepth ? 0 : R.mvDepth - (int)n;
            R.mvpDirty = 1;
            R.lightsDirty = 1;
            break;
        }
        case G_GEOMETRYMODE:
            flush();
            gGfx.geometry = (gGfx.geometry & (w0 | 0xFF000000u)) | w1;
            break;
        case G_MTX:
            matrix(w0, w1);
            break;
        case G_MOVEWORD:
            moveWord(w0, w1);
            break;
        case G_MOVEMEM:
            moveMem(w0, w1);
            break;
        case G_DL:
            if ((u32)gfxHost(w1) < 0x400) {
                break; /* would execute exception-vector bytes */
            }
            if (((w0 >> 16) & 0xFF) == G_DL_PUSH) {
                runDl(w1, depth + 1);
            } else {
                cmd = (const u32 *)gfxHost(w1);
            }
            break;
        case G_ENDDL:
            return;
        case G_RDPHALF_1:
            R.rdpHalf1 = w1;
            break;
        case G_RDPHALF_2:
            R.rdpHalf2 = w1;
            break;
        case G_SETOTHERMODE_H:
            setOtherMode(&gGfx.otherH, w0, w1);
            break;
        case G_SETOTHERMODE_L:
            setOtherMode(&gGfx.otherL, w0, w1);
            break;
        case G_RDPSETOTHERMODE:
            flush();
            gGfx.otherH = w0 & 0x00FFFFFF;
            gGfx.otherL = w1;
            break;
        case G_TEXRECT:
        case G_TEXRECTFLIP: {
            u32 h1 = cmd[1], h2 = cmd[3];
            cmd += 4;
            texRect(w0, w1, h1, h2, op == G_TEXRECTFLIP);
            break;
        }
        case G_FILLRECT:
            fillRect(w0, w1);
            break;
        case G_SETSCISSOR:
            flush();
            gGfx.scissor[0] = ((w0 >> 12) & 0xFFF) / 4;
            gGfx.scissor[1] = (w0 & 0xFFF) / 4;
            gGfx.scissor[2] = ((w1 >> 12) & 0xFFF) / 4;
            gGfx.scissor[3] = (w1 & 0xFFF) / 4;
            break;
        case G_SETPRIMDEPTH:
            flush();
            gGfx.primDepth = (u16)(w1 >> 16);
            break;
        case G_SETTILE:
            setTile(w0, w1);
            break;
        case G_SETTILESIZE:
            setTileSize(w0, w1);
            break;
        case G_LOADBLOCK:
            flush();
            gfxTmemLoadBlock(&gGfx, R.timg, R.timgSiz, R.timgWidth, (w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF,
                             (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            break;
        case G_LOADTILE:
            flush();
            gfxTmemLoadTile(&gGfx, R.timg, R.timgSiz, R.timgWidth, (w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF,
                            (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            break;
        case G_LOADTLUT:
            flush();
            gfxTmemLoadTlut(&gGfx, R.timg, (w1 >> 24) & 7, (w1 >> 14) & 0x3FF);
            break;
        case G_SETCOMBINE:
            flush();
            gGfx.combineHi = w0 & 0x00FFFFFF;
            gGfx.combineLo = w1;
            break;
        case G_SETENVCOLOR:
            flush();
            gGfx.env[0] = w1 >> 24; gGfx.env[1] = w1 >> 16; gGfx.env[2] = w1 >> 8; gGfx.env[3] = w1;
            break;
        case G_SETPRIMCOLOR:
            flush();
            gGfx.prim[0] = w1 >> 24; gGfx.prim[1] = w1 >> 16; gGfx.prim[2] = w1 >> 8; gGfx.prim[3] = w1;
            gGfx.primLodFrac = w0 & 0xFF;
            break;
        case G_SETBLENDCOLOR:
            flush();
            gGfx.blend[0] = w1 >> 24; gGfx.blend[1] = w1 >> 16; gGfx.blend[2] = w1 >> 8; gGfx.blend[3] = w1;
            break;
        case G_SETFOGCOLOR:
            flush();
            gGfx.fog[0] = w1 >> 24; gGfx.fog[1] = w1 >> 16; gGfx.fog[2] = w1 >> 8; gGfx.fog[3] = w1;
            break;
        case G_SETFILLCOLOR:
            flush();
            gGfx.fillColor = w1;
            break;
        case G_SETTIMG:
            R.timg = (u32)gfxHost(w1);
            R.timgFmt = (w0 >> 21) & 7;
            R.timgSiz = (w0 >> 19) & 3;
            R.timgWidth = (w0 & 0xFFF) + 1;
            break;
        case G_SETZIMG:
            flush();
            gGfx.zimg = (u32)gfxHost(w1);
            break;
        case G_SETCIMG:
            setColorImage(w0, w1);
            break;
        case G_LOAD_UCODE:
            gfxWarnOnce("loaducode", "PORT: gfx G_LOAD_UCODE ignored");
            break;
        default: {
            char key[16];
            snprintf(key, sizeof key, "op%02x", op);
            gfxWarnOnce(key, "PORT: gfx unknown opcode 0x%02x (w0=%08x w1=%08x) at 0x%x, list stack %s", op, w0, w1,
                        sCurCmd, dlStackString());
            break;
        }
        }
    }
}

void portGfxInit(void) {
    gfxBackendInit();
}

void portGfxRunTask(OSTask *task) {
    int i;
    memset(&R, 0, sizeof R);
    for (i = 0; i < 4; i++) {
        R.mv[0][i][i] = 1.0f;
        R.proj[i][i] = 1.0f;
    }
    R.mvpDirty = 1;
    R.texScaleS = R.texScaleT = 1.0f;
    gGfx.texOn = 0;
    gfxBackendBeginTask();
    runDl((u32)task->t.data_ptr, 0);
    flush();
    gfxBackendEndTask();
}

void portGfxPresent(void *fb, int black) {
    gfxBackendPresent((u32)fb, black);
}

void portGfxCpuWrote(const void *p, u32 len) {
    gfxBackendCpuWrote(portHostAddr((u32)p), len);
}

void portGfxSyncRdram(const void *p, u32 len) {
    gfxBackendSyncRdram(portHostAddr((u32)p), len);
}
