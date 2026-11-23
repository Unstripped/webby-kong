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
    s16 ob[3];             /* object-space position, for the debug probe */
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
/* RDP state the pending batch draws with. State commands only mark sStateDirty; the next triangle ends the batch
 * if the state really differs (display lists re-set unchanged state between most draws), so consecutive draws
 * with the same state go out as one. Commands that draw otherwise or change TMEM or the target still flush. */
static GfxState sBatchState;
static int sStateDirty;

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
        gfxBackendDrawTriangles(&sBatchState, sBatch, sBatchCount);
        sBatchCount = 0;
    }
}

/* before a state command writes gGfx */
static void stateChanging(void) {
    sStateDirty = 1;
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
        memcpy(d->ob, v->ob, sizeof d->ob);
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

/* ---------------------------------------------------------------- debug pixel probe */

#define PROBE_MAX_HITS 16
#define PROBE_LOADS 8

static struct {
    float x, y;
    int state; /* 0 idle, 1 armed for the next task, 2 recording, 3 done */
    int hits, len, truncated;
    u32 loads[PROBE_LOADS][6]; /* ring of recent texture loads: kind (1 block, 2 tile, 3 tlut), timg, siz, width, w0, w1 */
    int loadNext;
    char buf[24576];
} sProbe;

void gfxProbeArm(float x, float y) {
    sProbe.x = x;
    sProbe.y = y;
    sProbe.state = 1;
    sProbe.hits = 0;
    sProbe.len = 0;
    sProbe.truncated = 0;
    sProbe.buf[0] = 0;
}

#define PROBE_TRUNCATED "(truncated: probe buffer full)\n"

const char *gfxProbeResult(void) {
    if (sProbe.state != 3) {
        return "";
    }
    if (sProbe.truncated && sProbe.len + sizeof PROBE_TRUNCATED <= sizeof sProbe.buf) {
        memcpy(sProbe.buf + sProbe.len, PROBE_TRUNCATED, sizeof PROBE_TRUNCATED);
        sProbe.len += sizeof PROBE_TRUNCATED - 1;
        sProbe.truncated = 0;
    }
    return sProbe.hits ? sProbe.buf : "no triangle covers this point\n";
}

static void probeLoad(u32 kind, u32 w0, u32 w1) {
    u32 *l = sProbe.loads[sProbe.loadNext++ % PROBE_LOADS];
    l[0] = kind;
    l[1] = R.timg;
    l[2] = R.timgSiz;
    l[3] = R.timgWidth;
    l[4] = w0;
    l[5] = w1;
}

static float probeEdge(const RspVtx *a, const RspVtx *b) {
    return (b->sx - a->sx) * (sProbe.y - a->sy) - (b->sy - a->sy) * (sProbe.x - a->sx);
}

/* appends whole lines only: one that doesn't fit (with room kept for the truncation note) is dropped and stops the
 * recording */
static void probePrintf(const char *fmt, ...) {
    size_t avail = sizeof sProbe.buf - sizeof PROBE_TRUNCATED - sProbe.len;
    va_list ap;
    int n;
    if (sProbe.truncated) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(sProbe.buf + sProbe.len, avail, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= avail) {
        sProbe.buf[sProbe.len] = 0;
        sProbe.truncated = 1;
        return;
    }
    sProbe.len += n;
}

#define PP probePrintf

static void probeTriangle(const RspVtx *v0, const RspVtx *v1, const RspVtx *v2) {
    const RspVtx *v[3] = { v0, v1, v2 };
    float e0, e1, e2;
    int i, k;
    if (sProbe.state != 2 || sProbe.truncated || sProbe.hits >= PROBE_MAX_HITS || v0->w <= 0 || v1->w <= 0 ||
        v2->w <= 0) {
        return;
    }
    e0 = probeEdge(v0, v1);
    e1 = probeEdge(v1, v2);
    e2 = probeEdge(v2, v0);
    if (!((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))) {
        return;
    }
    sProbe.hits++;
    PP("#%d dl=%s\n  othermode H=%08x L=%08x combine=%08x:%08x geometry=%08x texOn=%d tile=%d levels=%d texScale=%.4f,%.4f\n"
       "  prim=%d,%d,%d,%d lodfrac=%d env=%d,%d,%d,%d\n",
       sProbe.hits, dlStackString(), (unsigned)gGfx.otherH, (unsigned)gGfx.otherL, (unsigned)gGfx.combineHi,
       (unsigned)gGfx.combineLo, (unsigned)gGfx.geometry, gGfx.texOn, gGfx.texTile, gGfx.texLevels, R.texScaleS,
       R.texScaleT, gGfx.prim[0], gGfx.prim[1], gGfx.prim[2], gGfx.prim[3], gGfx.primLodFrac, gGfx.env[0], gGfx.env[1],
       gGfx.env[2], gGfx.env[3]);
    for (i = 0; i < 3; i++) {
        PP("  v%d ob=(%d,%d,%d) st=(%.2f,%.2f) screen=(%.1f,%.1f,%.0f) w=%.1f rgba=(%.2f,%.2f,%.2f,%.2f)\n", i, v[i]->ob[0],
           v[i]->ob[1], v[i]->ob[2], v[i]->s, v[i]->t, v[i]->sx, v[i]->sy, v[i]->sz, v[i]->w, v[i]->r, v[i]->g, v[i]->b,
           v[i]->a);
    }
    for (i = 0; i < 8; i++) {
        const GfxTile *t = &gGfx.tiles[i];
        if (t->line == 0 && t->tmem == 0 && t->lrs == 0 && t->lrt == 0) {
            continue;
        }
        PP("  tile%d fmt=%d siz=%d line=%d tmem=0x%x pal=%d s(cm=%d mask=%d shift=%d) t(cm=%d mask=%d shift=%d) "
           "size=%.2f,%.2f-%.2f,%.2f decoded=%ux%u\n",
           i, t->fmt, t->siz, t->line, t->tmem, t->palette, t->cms, t->masks, t->shifts, t->cmt, t->maskt, t->shiftt,
           t->uls / 4.0, t->ult / 4.0, t->lrs / 4.0, t->lrt / 4.0, (unsigned)gfxTileWidth(t), (unsigned)gfxTileHeight(t));
    }
    for (k = 1; k <= PROBE_LOADS; k++) {
        const u32 *l = sProbe.loads[(sProbe.loadNext - k + PROBE_LOADS * 4) % PROBE_LOADS];
        if (l[0] != 0) {
            PP("  load-%d %s timg=0x%x siz=%u width=%u cmd=%08x:%08x\n", k,
               l[0] == 1 ? "block" : l[0] == 2 ? "tile" : "tlut", (unsigned)l[1], (unsigned)l[2], (unsigned)l[3],
               (unsigned)l[4], (unsigned)l[5]);
        }
    }
}

#undef PP

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
    probeTriangle(v0, v1, v2);
    if (sStateDirty) {
        sStateDirty = 0;
        if (sBatchCount != 0 && memcmp(&sBatchState, &gGfx, sizeof gGfx) != 0) {
            flush();
        }
    }
    if (sBatchCount == 0) {
        memcpy(&sBatchState, &gGfx, sizeof gGfx);
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
    stateChanging();
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

/* ---------------------------------------------------------------- frame interpolation
 * The game draws 30 frames/s. After a task that draws the VI image, its display list runs again for each in-between
 * image (one at 60 fps, three at 120, seven at 240), every matrix blended from the previous frame's value toward
 * this frame's by the image's fraction (1/2; 1/4, 2/4, 3/4; ...); main.c presents them on the refreshes between
 * the two swaps (the game frames themselves keep their VIs). A matrix pairs with the previous frame's matrix of the
 * same load kind and vertex data in the same draw position, or a clearly nearer one of those (interpFindPrev);
 * unmatched matrices and ones that jump (cuts, teleports) keep this frame's value. The replays repeat the first
 * pass's G_BRANCH_Z / G_CULLDL outcomes, so they walk exactly the same commands and their n-th matrix is the first
 * pass's n-th. */
typedef struct {
    u32 kind; /* G_MTX parameter bits */
    u32 sig;  /* host address of the first vertices loaded after this matrix, 0 if none */
    u32 occ;  /* earlier records of this frame with the same kind and sig */
    float m[4][4];
} MtxRec;

typedef struct {
    MtxRec *rec;
    u32 count, cap;
} MtxFrame;

enum { INTERP_OFF, INTERP_RECORD, INTERP_REPLAY };

static struct {
    MtxFrame frame[2];   /* [cur]: this task's records, [cur ^ 1]: the last VI-image task's */
    int cur;
    int prevValid;
    int pass;            /* INTERP_* */
    int drewVi;          /* the recording task set a VI-sized colour image */
    u32 pendingSig;      /* records from here on wait for the vertices that give their sig */
    float (*from)[4][4]; /* per first-pass matrix: the previous frame's partner (this frame's value if none) */
    float (*to)[4][4];   /* and this frame's value */
    u32 fromCap, toCap, pairCount;
    float fraction;      /* of the way from `from` to `to`, for the replay being drawn */
    u32 *table;          /* open-addressing index of the previous frame by (kind, sig): record index + 1, 0 empty */
    u32 *curTable;       /* the same for this frame: its latest record of each (kind, sig) */
    u32 tableCap, curTableCap, tableMask;
    u8 *branch;          /* recorded G_BRANCH_Z / G_CULLDL outcomes */
    u32 branchCount, branchCap, branchNext;
    u32 mtxNext;
    /* replays draw at the latest before the next VI's game code runs (portGfxRunDeferred), so nothing has touched
     * the display list's inputs since the first pass; earlier ones are drawn on the refresh that shows them */
    int images;          /* images per game frame (2 at 60 fps), 0 when no in-between image is pending */
    u32 rendered;        /* bit m: in-between image m (1 .. images - 1) is drawn */
    u32 drawn;           /* bit m: and that replay drew the VI image (its slot holds it) */
    u32 viImage;         /* the VI image the pair's first pass drew: presenting it ends the in-between images */
    u32 recordViImage;   /* the one the recording pass draws */
    int shown;           /* the last in-between image presented, 0 none */
    int ticks;           /* refreshes (a VI each when unlocked) since the first pass */
    int ticksPerImage;
    u32 replayDl;
    GfxState start;      /* RDP state and TMEM the task started with */
    u8 tmemStart[sizeof gGfxTmem];
} sInterp;

int gfxInterpQsRanges(QsRange *out) {
    out[0] = QS_RANGE(sInterp);
    return 1;
}

/* quickload: the previous frame's matrices belong to another timeline */
void gfxInterpReset(void) {
    sInterp.prevValid = 0;
    sInterp.images = 0;
}

static void *growArray(void *p, u32 *cap, u32 need, size_t elem) {
    u32 n;
    if (need <= *cap) {
        return p;
    }
    n = *cap ? *cap : 256;
    while (n < need) {
        n *= 2;
    }
    p = realloc(p, n * elem);
    if (p == NULL) {
        portAbort("PORT: gfx interpolation out of memory");
    }
    *cap = n;
    return p;
}

static void interpRecordMatrix(u32 kind, float m[4][4]) {
    MtxFrame *f = &sInterp.frame[sInterp.cur];
    MtxRec *r;
    f->rec = growArray(f->rec, &f->cap, f->count + 1, sizeof *f->rec);
    r = &f->rec[f->count++];
    r->kind = kind;
    r->sig = 0;
    memcpy(r->m, m, sizeof r->m);
}

/* projection matrices (the camera: perspective, then the view multiplied on) keep sig 0 and pair by draw order:
 * the vertices that follow them often sit in a buffer the CPU alternates every frame, which left the camera
 * unpaired, so in-between images moved objects but not the view */
static void interpRecordVertices(u32 host) {
    MtxFrame *f = &sInterp.frame[sInterp.cur];
    for (; sInterp.pendingSig < f->count; sInterp.pendingSig++) {
        if (!(f->rec[sInterp.pendingSig].kind & G_MTX_PROJECTION)) {
            f->rec[sInterp.pendingSig].sig = host;
        }
    }
}

/* G_BRANCH_Z / G_CULLDL: the first pass records `taken`, the replay returns the recorded outcome */
static int interpBranch(int taken) {
    if (sInterp.pass == INTERP_RECORD) {
        sInterp.branch = growArray(sInterp.branch, &sInterp.branchCap, sInterp.branchCount + 1, 1);
        sInterp.branch[sInterp.branchCount++] = (u8)taken;
    } else if (sInterp.pass == INTERP_REPLAY && sInterp.branchNext < sInterp.branchCount) {
        taken = sInterp.branch[sInterp.branchNext++];
    }
    return taken;
}

static u32 interpHash(u32 kind, u32 sig) {
    u32 h = kind * 0x9E3779B1u ^ sig * 0x85EBCA77u;
    return h ^ (h >> 15);
}

/* fills occ for the current frame (curTable maps (kind, sig) to its latest record), then indexes the previous
 * frame by (kind, sig): every record of a group lands in one probe run */
static void interpIndex(void) {
    MtxFrame *cur = &sInterp.frame[sInterp.cur], *prev = &sInterp.frame[sInterp.cur ^ 1];
    u32 need = 1, mask, i;
    while (need < 2 * (cur->count > prev->count ? cur->count : prev->count) + 2) {
        need *= 2;
    }
    sInterp.table = growArray(sInterp.table, &sInterp.tableCap, need, sizeof *sInterp.table);
    sInterp.curTable = growArray(sInterp.curTable, &sInterp.curTableCap, need, sizeof *sInterp.curTable);
    mask = sInterp.tableMask = need - 1;
    memset(sInterp.curTable, 0, need * sizeof *sInterp.curTable);
    for (i = 0; i < cur->count; i++) {
        MtxRec *r = &cur->rec[i];
        u32 h = interpHash(r->kind, r->sig) & mask;
        r->occ = 0;
        while (sInterp.curTable[h] != 0) {
            MtxRec *o = &cur->rec[sInterp.curTable[h] - 1];
            if (o->kind == r->kind && o->sig == r->sig) {
                r->occ = o->occ + 1;
                break;
            }
            h = (h + 1) & mask;
        }
        sInterp.curTable[h] = i + 1;
    }
    memset(sInterp.table, 0, need * sizeof *sInterp.table);
    for (i = 0; i < prev->count; i++) {
        MtxRec *r = &prev->rec[i];
        u32 h = interpHash(r->kind, r->sig) & mask;
        while (sInterp.table[h] != 0) {
            h = (h + 1) & mask;
        }
        sInterp.table[h] = i + 1;
    }
}

/* this frame has a record of (kind, sig) */
static int interpInCur(u32 kind, u32 sig) {
    MtxFrame *cur = &sInterp.frame[sInterp.cur];
    u32 mask = sInterp.tableMask, h;
    for (h = interpHash(kind, sig) & mask; sInterp.curTable[h] != 0; h = (h + 1) & mask) {
        const MtxRec *o = &cur->rec[sInterp.curTable[h] - 1];
        if (o->kind == kind && o->sig == sig) {
            return 1;
        }
    }
    return 0;
}

static float mtxDistSq(const float a[4][4], const float b[4][4]) {
    float d = 0;
    int k;
    for (k = 0; k < 16; k++) {
        float e = (&a[0][0])[k] - (&b[0][0])[k];
        d += e * e;
    }
    return d;
}

/* the previous frame's record of the same kind and vertices: the one in the same draw position (occ), unless
 * another lies at under half its distance. Instances sharing a mesh (trees) change draw order between frames, so
 * the n-th one can be a different object far away; parts of one model can share a translation (bones, rigid
 * pieces), so nearest alone mixes them up. */
static const MtxRec *interpFindPrev(const MtxRec *r) {
    MtxFrame *prev = &sInterp.frame[sInterp.cur ^ 1];
    const MtxRec *best = NULL, *same = NULL;
    float bestD = 0, sameD = 0;
    u32 mask = sInterp.tableMask, h;
    for (h = interpHash(r->kind, r->sig) & mask; sInterp.table[h] != 0; h = (h + 1) & mask) {
        const MtxRec *o = &prev->rec[sInterp.table[h] - 1];
        float d;
        if (o->kind != r->kind || o->sig != r->sig) {
            continue;
        }
        d = mtxDistSq(o->m, r->m);
        if (o->occ == r->occ) {
            same = o;
            sameD = d;
        }
        if (best == NULL || d < bestD) {
            best = o;
            bestD = d;
        }
    }
    if (best == NULL) {
        /* vertices the CPU rebuilds into alternating buffers (shadows): sig changes every frame. The nearest
         * previous record of the same kind whose vertices this frame no longer loads, if it is close by */
        u32 i;
        for (i = 0; i < prev->count; i++) {
            const MtxRec *o = &prev->rec[i];
            float d, dx, dy, dz;
            if (o->kind != r->kind) {
                continue;
            }
            dx = o->m[3][0] - r->m[3][0];
            dy = o->m[3][1] - r->m[3][1];
            dz = o->m[3][2] - r->m[3][2];
            if (dx * dx + dy * dy + dz * dz > 100.0f * 100.0f || interpInCur(o->kind, o->sig)) {
                continue;
            }
            d = mtxDistSq(o->m, r->m);
            if (best == NULL || d < bestD) {
                best = o;
                bestD = d;
            }
        }
        return best;
    }
    return same != NULL && sameD <= 4.0f * bestD ? same : best;
}

/* a jump too large to be motion between two frames: a cut, teleport or a different object in this slot. Animated
 * bones turn a lot in a frame (tens of degrees), so only a rotation of roughly 90 degrees or more counts */
static int interpJumps(const float a[4][4], const float b[4][4]) {
    float rot = 0, scale = 0, dt, mag;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            float d = fabsf(a[i][j] - b[i][j]), s = fabsf(b[i][j]);
            rot = d > rot ? d : rot;
            scale = s > scale ? s : scale;
        }
    }
    dt = sqrtf((a[3][0] - b[3][0]) * (a[3][0] - b[3][0]) + (a[3][1] - b[3][1]) * (a[3][1] - b[3][1]) +
               (a[3][2] - b[3][2]) * (a[3][2] - b[3][2]));
    mag = sqrtf(b[3][0] * b[3][0] + b[3][1] * b[3][1] + b[3][2] * b[3][2]);
    return rot > 1.4f * scale || dt > 0.35f * mag + 100.0f;
}

static void interpBuildPairs(void) {
    MtxFrame *cur = &sInterp.frame[sInterp.cur];
    u32 i;
    sInterp.from = growArray(sInterp.from, &sInterp.fromCap, cur->count + 1, sizeof *sInterp.from);
    sInterp.to = growArray(sInterp.to, &sInterp.toCap, cur->count + 1, sizeof *sInterp.to);
    sInterp.pairCount = cur->count;
    for (i = 0; i < cur->count; i++) {
        const MtxRec *r = &cur->rec[i];
        const MtxRec *p = interpFindPrev(r);
        memcpy(sInterp.from[i], p == NULL || interpJumps(p->m, r->m) ? r->m : p->m, sizeof r->m);
        memcpy(sInterp.to[i], r->m, sizeof r->m);
    }
}

static void matrix(u32 w0, u32 w1) {
    u32 p = (w0 & 0xFF) ^ G_MTX_PUSH;
    float m[4][4];
    if (sInterp.pass == INTERP_REPLAY && sInterp.mtxNext < sInterp.pairCount) {
        const float *a = &sInterp.from[sInterp.mtxNext][0][0], *b = &sInterp.to[sInterp.mtxNext][0][0];
        float t = sInterp.fraction;
        int k;
        for (k = 0; k < 16; k++) {
            (&m[0][0])[k] = a[k] + t * (b[k] - a[k]);
        }
        sInterp.mtxNext++;
    } else {
        mtxLoad(m, gfxHost(w1));
        if (sInterp.pass == INTERP_RECORD) {
            interpRecordMatrix(p, m);
        }
    }
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
    stateChanging();
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
    stateChanging();
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
    if (sInterp.pass == INTERP_RECORD && gGfx.cimgWidth == gPortViWidth && gGfx.cimg != gGfx.zimg) {
        sInterp.drewVi = 1;
        sInterp.recordViImage = gGfx.cimg;
    }
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
            if (w1 == PORT_GFX_BLANK_SIDES) {
                flush();
                gfxBackendBlankSides(&gGfx);
            }
            break;
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
            if (sInterp.pass == INTERP_RECORD) {
                interpRecordVertices((u32)gfxHost(w1));
            }
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
            if (interpBranch(cullDl((w0 & 0xFFFF) / 2, (w1 & 0xFFFF) / 2))) {
                return;
            }
            break;
        case G_BRANCH_Z: {
            int vi = (w0 & 0xFFF) / 2;
            if (interpBranch(vi < MAX_VTX && R.vtx[vi].sz <= (float)(s32)w1)) {
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
            stateChanging();
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
            R.mvDepth = (int)n > R.mvDepth ? 0 : R.mvDepth - (int)n;
            R.mvpDirty = 1;
            R.lightsDirty = 1;
            break;
        }
        case G_GEOMETRYMODE:
            stateChanging();
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
            stateChanging();
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
            stateChanging();
            gGfx.scissor[0] = ((w0 >> 12) & 0xFFF) / 4;
            gGfx.scissor[1] = (w0 & 0xFFF) / 4;
            gGfx.scissor[2] = ((w1 >> 12) & 0xFFF) / 4;
            gGfx.scissor[3] = (w1 & 0xFFF) / 4;
            break;
        case G_SETPRIMDEPTH:
            stateChanging();
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
            probeLoad(1, w0, w1);
            gfxTmemLoadBlock(&gGfx, R.timg, R.timgSiz, R.timgWidth, (w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF,
                             (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            break;
        case G_LOADTILE:
            flush();
            probeLoad(2, w0, w1);
            gfxTmemLoadTile(&gGfx, R.timg, R.timgSiz, R.timgWidth, (w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF,
                            (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            break;
        case G_LOADTLUT:
            flush();
            probeLoad(3, w0, w1);
            gfxTmemLoadTlut(&gGfx, R.timg, (w1 >> 24) & 7, (w1 >> 14) & 0x3FF);
            break;
        case G_SETCOMBINE:
            stateChanging();
            gGfx.combineHi = w0 & 0x00FFFFFF;
            gGfx.combineLo = w1;
            break;
        case G_SETENVCOLOR:
            stateChanging();
            gGfx.env[0] = w1 >> 24; gGfx.env[1] = w1 >> 16; gGfx.env[2] = w1 >> 8; gGfx.env[3] = w1;
            break;
        case G_SETPRIMCOLOR:
            stateChanging();
            gGfx.prim[0] = w1 >> 24; gGfx.prim[1] = w1 >> 16; gGfx.prim[2] = w1 >> 8; gGfx.prim[3] = w1;
            gGfx.primLodFrac = w0 & 0xFF;
            break;
        case G_SETBLENDCOLOR:
            stateChanging();
            gGfx.blend[0] = w1 >> 24; gGfx.blend[1] = w1 >> 16; gGfx.blend[2] = w1 >> 8; gGfx.blend[3] = w1;
            break;
        case G_SETFOGCOLOR:
            stateChanging();
            gGfx.fog[0] = w1 >> 24; gGfx.fog[1] = w1 >> 16; gGfx.fog[2] = w1 >> 8; gGfx.fog[3] = w1;
            break;
        case G_SETFILLCOLOR:
            stateChanging();
            gGfx.fillColor = w1;
            break;
        case G_SETTIMG:
            R.timg = (u32)gfxHost(w1);
            R.timgFmt = (w0 >> 21) & 7;
            R.timgSiz = (w0 >> 19) & 3;
            R.timgWidth = (w0 & 0xFFF) + 1;
            break;
        case G_SETZIMG:
            stateChanging();
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

static void resetRsp(void) {
    int i;
    memset(&R, 0, sizeof R);
    for (i = 0; i < 4; i++) {
        R.mv[0][i][i] = 1.0f;
        R.proj[i][i] = 1.0f;
    }
    R.mvpDirty = 1;
    R.texScaleS = R.texScaleT = 1.0f;
    stateChanging();
    gGfx.texOn = 0;
}

/* in-between image m: the pending task's list again from the RDP state and TMEM it started with, every matrix
 * m / images of the way from the previous frame's value; afterwards the state is what it was before */
static void interpRender(int m) {
    static u8 tmemEnd[sizeof gGfxTmem];
    GfxState end;
    end = gGfx;
    memcpy(tmemEnd, gGfxTmem, sizeof tmemEnd);
    gGfx = sInterp.start;
    memcpy(gGfxTmem, sInterp.tmemStart, sizeof tmemEnd);
    resetRsp();
    sInterp.pass = INTERP_REPLAY;
    sInterp.fraction = (float)m / (float)sInterp.images;
    sInterp.mtxNext = 0;
    sInterp.branchNext = 0;
    gfxBackendInterpBegin();
    runDl(sInterp.replayDl, 0);
    flush();
    if (gfxBackendInterpEnd(m - 1)) {
        sInterp.drawn |= 1u << m;
    }
    sInterp.rendered |= 1u << m;
    sInterp.pass = INTERP_OFF;
    gGfx = end;
    memcpy(gGfxTmem, tmemEnd, sizeof tmemEnd);
}

/* game code is about to run (a VI, the next task) and may change the display list's inputs: draw every in-between
 * image still to come now */
void portGfxRunDeferred(void) {
    int m;
    for (m = sInterp.shown + 1; m < sInterp.images; m++) {
        if (!(sInterp.rendered & (1u << m))) {
            interpRender(m);
        }
    }
}

/* images per game frame: the most that divide the refreshes of a game frame (two VIs) evenly, up to the setting's
 * 2 << (level - 1); a display that is not phase-locked counts one refresh per VI (60 fps) */
static int interpImagesFor(int level) {
    int cap = 1 << level, span = 2 * portPaceRefreshesPerVi(), m;
    for (m = cap; m > 2; m--) {
        if (span % m == 0) {
            return m;
        }
    }
    return 2;
}

void portGfxRunTask(OSTask *task) {
    int interp = gfxBackendInterpLevel();
    portGfxRunDeferred(); /* draws before this task's recording replaces its inputs */
    if (interp) {
        sInterp.start = gGfx;
        memcpy(sInterp.tmemStart, gGfxTmem, sizeof sInterp.tmemStart);
        sInterp.pass = INTERP_RECORD;
        sInterp.drewVi = 0;
        sInterp.frame[sInterp.cur].count = 0;
        sInterp.pendingSig = 0;
        sInterp.branchCount = 0;
    }
    resetRsp();
    gfxBackendBeginTask();
    if (sProbe.state == 1) {
        sProbe.state = 2;
    }
    runDl((u32)task->t.data_ptr, 0);
    flush();
    if (sProbe.state == 2) {
        sProbe.state = 3;
    }
    gfxBackendEndTask();
    if (!interp) {
        return;
    }
    sInterp.pass = INTERP_OFF;
    if (!sInterp.drewVi) {
        return; /* offscreen work: the last VI image's matrices stay the previous frame */
    }
    interpIndex(); /* occ is needed when this frame is the previous one, too */
    if (sInterp.prevValid) {
        interpBuildPairs();
        sInterp.replayDl = (u32)task->t.data_ptr;
        sInterp.viImage = sInterp.recordViImage;
        sInterp.images = interpImagesFor(interp);
        sInterp.ticksPerImage = 2 * portPaceRefreshesPerVi() / sInterp.images;
        sInterp.rendered = 0;
        sInterp.drawn = 0;
        sInterp.shown = 0;
        sInterp.ticks = 0;
    }
    sInterp.cur ^= 1;
    sInterp.prevValid = 1;
}

/* the game frame the pending in-between images lead up to is on screen (or the screen is blanked): they are done */
void portGfxPresent(void *fb, int black) {
    if (black || (u32)fb == sInterp.viImage) {
        sInterp.images = 0;
    }
    gfxBackendPresent((u32)fb, black);
}

/* the refreshes of this animation frame (one per VI on a display that is not phase-locked); before its VIs run */
void portGfxInterpTick(int ticks) {
    sInterp.ticks += ticks;
}

/* the in-between image due on this refresh, if it is a new one; 1 when presented */
int portGfxPresentInterp(void) {
    int m;
    if (sInterp.images == 0) {
        return 0;
    }
    m = sInterp.ticks / sInterp.ticksPerImage;
    if (m >= sInterp.images) {
        m = sInterp.images - 1; /* a late game frame: the last in-between image stays up */
    }
    if (m <= sInterp.shown) {
        return 0;
    }
    if (!(sInterp.rendered & (1u << m))) {
        interpRender(m); /* no VI since the first pass, so no game code ran: its inputs are as they were */
    }
    sInterp.shown = m;
    return (sInterp.drawn & (1u << m)) ? gfxBackendPresentInterp(m - 1) : 0;
}

/* 0 = 30 fps; 1, 2, 3 = up to 60, 120, 240 by interpolation; the next in-between images pair this frame with the
 * next one */
int portGfxSetInterp(int level) {
    gfxInterpReset();
    return gfxBackendSetInterp(level);
}

void portGfxCpuWrote(const void *p, u32 len) {
    gfxBackendCpuWrote(portHostAddr((u32)p), len);
}

void portGfxSyncRdram(const void *p, u32 len) {
    gfxBackendSyncRdram(portHostAddr((u32)p), len);
}
