/* 4 KB TMEM emulation and per-tile decode to RGBA8. Texels and TLUTs are big-endian in RDRAM.
 * TMEM keeps the hardware's odd-line swizzle: loads swap the 32-bit halves of every 64-bit word on odd lines
 * (LoadTile: odd rows; LoadBlock: odd lines of its dxt counter) and sampling swaps back on odd tile rows. Model
 * textures loaded with dxt = 0 are stored pre-swizzled in RDRAM and only decode correctly this way. 32b tiles are
 * stored linearly at twice the line stride here (no RG/BA split), so their swap unit is two texels (8 bytes). */
#include "gfx/gfx.h"

u8 gGfxTmem[4096];

/* port-only, never game-visible: lives above 16 MB (see memory model) */
static u8 *sDecoded;

int gfxTmemQsRanges(QsRange *out) {
    out[0] = QS_RANGE(sDecoded);
    return 1;
}

static u32 texelBytesX2(u32 siz) {
    /* bytes per texel times two (4b = 1) */
    return siz == G_IM_SIZ_4b ? 1 : siz == G_IM_SIZ_8b ? 2 : siz == G_IM_SIZ_16b ? 4 : 8;
}

/* bytes swapped with their neighbour on odd lines */
static u32 swapUnit(u32 siz) {
    return siz == G_IM_SIZ_32b ? 8 : 4;
}

/* fixed-size copies: these compile to plain loads and stores, a variable `unit` would call memmove per group */
static void swapHalves(u8 *p, u32 unit) {
    if (unit == 4) {
        u32 a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        memcpy(p, &b, 4);
        memcpy(p + 4, &a, 4);
    } else {
        u64 a, b;
        memcpy(&a, p, 8);
        memcpy(&b, p + 8, 8);
        memcpy(p, &b, 8);
        memcpy(p + 8, &a, 8);
    }
}

/* copies n bytes from RDRAM to TMEM at dst, rounded up to whole swap groups and clipped to TMEM; returns the
 * byte count written */
static u32 tmemCopy(u32 dst, u32 src, u32 n, u32 group) {
    n = (n + group - 1) / group * group;
    if (dst >= sizeof gGfxTmem) {
        return 0;
    }
    if (dst + n > sizeof gGfxTmem) {
        n = sizeof gGfxTmem - dst;
    }
    memcpy(&gGfxTmem[dst], (const void *)src, n);
    return n;
}

void gfxTmemLoadBlock(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs,
                      u32 dxt) {
    const GfxTile *t = &st->tiles[tile];
    u32 bpt2 = texelBytesX2(timgSiz);
    u32 unit = swapUnit(timgSiz), group = unit * 2;
    u32 bytes = ((lrs - uls + 1) * bpt2 + 1) / 2;
    u32 src = timg + ((ult * timgWidth + uls) * bpt2) / 2;
    u32 dst = (u32)t->tmem * 8;
    u32 i;
    bytes = tmemCopy(dst, src, bytes, group);
    /* the dxt counter advances once per 64-bit RDRAM word; its integer part is the TMEM line. dxt 0 (model
     * textures stored pre-swizzled) never leaves line 0. */
    for (i = 0; dxt != 0 && i + group <= bytes; i += group) {
        if ((((i / 8) * dxt) >> 11) & 1) {
            swapHalves(&gGfxTmem[dst + i], unit);
        }
    }
}

void gfxTmemLoadTile(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs,
                     u32 lrt) {
    const GfxTile *t = &st->tiles[tile];
    u32 bpt2 = texelBytesX2(timgSiz);
    u32 unit = swapUnit(timgSiz), group = unit * 2;
    u32 x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2;
    u32 rowBytes = ((x1 - x0 + 1) * bpt2 + 1) / 2;
    u32 stride = (u32)t->line * 8;
    u32 dst = (u32)t->tmem * 8;
    u32 y, i;
    if (timgSiz == G_IM_SIZ_32b) {
        stride *= 2; /* 32b tiles split across both TMEM halves; stored linearly here */
    }
    for (y = y0; y <= y1; y++) {
        u32 d = dst + (y - y0) * stride;
        u32 n = tmemCopy(d, timg + ((y * timgWidth + x0) * bpt2) / 2, rowBytes, group);
        if (n == 0) {
            break;
        }
        if ((y - y0) & 1) {
            for (i = 0; i + group <= n; i += group) {
                swapHalves(&gGfxTmem[d + i], unit);
            }
        }
    }
}

void gfxTmemLoadTlut(const GfxState *st, u32 timg, int tile, u32 count) {
    const GfxTile *t = &st->tiles[tile];
    u32 dst = (u32)t->tmem * 8;
    u32 bytes = (count + 1) * 2;
    if (dst >= sizeof gGfxTmem) {
        return;
    }
    if (dst + bytes > sizeof gGfxTmem) {
        bytes = sizeof gGfxTmem - dst;
    }
    memcpy(&gGfxTmem[dst], (const void *)timg, bytes);
}

/* texels decoded along one tile axis. A masked axis repeats every 2^mask texels whatever the tile size: games
 * scroll by moving ul (sampling subtracts it), which can shrink lr - ul or turn it negative (Aztec's sand). Only a
 * clamped axis smaller than its mask period ends at the tile size. 0: no texels (unmasked, lr above ul). */
static u32 tileDim(u16 ul, u16 lr, u8 mask, u8 cm) {
    s32 span = ((s32)lr - (s32)ul) >> 2;
    u32 size = span >= 0 ? (u32)span + 1 : 0;
    if (mask == 0) {
        return size;
    }
    if (mask > 10) {
        mask = 10; /* the RDP's wrap counter is 10 bits */
    }
    if ((cm & G_TX_CLAMP) && size != 0 && size < (1u << mask)) {
        return size;
    }
    return 1u << mask;
}

u32 gfxTileWidth(const GfxTile *t) {
    return tileDim(t->uls, t->lrs, t->masks, t->cms);
}

u32 gfxTileHeight(const GfxTile *t) {
    return tileDim(t->ult, t->lrt, t->maskt, t->cmt);
}

/* FNV-1a style over 64-bit words (tail bytewise): the key covers up to all of TMEM on every textured draw. Four
 * lanes with distinct seeds take 32 bytes per step, so their multiply chains overlap. */
static u64 hashBytes(u64 h, const u8 *p, u32 n) {
    const u64 prime = 1099511628211ull;
    u32 i = 0;
    if (n >= 32) {
        u64 a = h, b = h ^ 0x9E3779B97F4A7C15ull, c = h ^ 0xC2B2AE3D27D4EB4Full, d = h ^ 0x165667B19E3779F9ull;
        for (; i + 32 <= n; i += 32) {
            u64 w[4];
            memcpy(w, p + i, 32);
            a = (a ^ w[0]) * prime;
            b = (b ^ w[1]) * prime;
            c = (c ^ w[2]) * prime;
            d = (d ^ w[3]) * prime;
            a ^= a >> 32;
            b ^= b >> 32;
            c ^= c >> 32;
            d ^= d >> 32;
        }
        h = (((((a * prime) ^ b) * prime ^ c) * prime) ^ d) * prime;
        h ^= h >> 32;
    }
    for (; i + 8 <= n; i += 8) {
        u64 w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * prime;
        h ^= h >> 32;
    }
    for (; i < n; i++) {
        h = (h ^ p[i]) * prime;
    }
    return h;
}

static void rgba16(u16 c, u8 *o) {
    o[0] = ((c >> 11) & 0x1F) * 255 / 31;
    o[1] = ((c >> 6) & 0x1F) * 255 / 31;
    o[2] = ((c >> 1) & 0x1F) * 255 / 31;
    o[3] = (c & 1) ? 255 : 0;
}

static void tlutColor(u32 tt, u32 index, u8 *o) {
    const u8 *e = &gGfxTmem[0x800 + (index & 0xFF) * 2];
    u16 c = (u16)((e[0] << 8) | e[1]);
    if (tt == G_TT_IA16) {
        o[0] = o[1] = o[2] = e[0];
        o[3] = e[1];
    } else {
        rgba16(c, o);
    }
}

/* tile `ti` as decoded by gfxTmemDecode: size, TMEM layout and the cache key of its texels and parameters */
typedef struct {
    u32 w, h, base, stride, tt;
} TileLayout;

static int tileLayout(const GfxState *st, int ti, TileLayout *l, u64 *key) {
    const GfxTile *t = &st->tiles[ti];
    u32 span;
    u64 k = 1469598103934665603ull;
    l->w = gfxTileWidth(t);
    l->h = gfxTileHeight(t);
    l->base = (u32)t->tmem * 8;
    l->stride = (u32)t->line * 8;
    l->tt = st->otherH & (3u << G_MDSFT_TEXTLUT);
    if (l->w == 0 || l->h == 0 || l->w > 1024 || l->h > 1024) {
        return 0;
    }
    if (t->siz == G_IM_SIZ_32b) {
        l->stride *= 2;
    }
    if (l->stride == 0) {
        l->stride = (l->w * texelBytesX2(t->siz) + 1) / 2;
    }
    /* sampling wraps at the end of TMEM (TMEM_AT below), so the key covers the wrapped span */
    span = l->stride * l->h;
    if (span > sizeof gGfxTmem) {
        span = sizeof gGfxTmem;
    }
    if (l->base + span > sizeof gGfxTmem) {
        k = hashBytes(k, &gGfxTmem[l->base], sizeof gGfxTmem - l->base);
        k = hashBytes(k, gGfxTmem, l->base + span - sizeof gGfxTmem);
    } else {
        k = hashBytes(k, &gGfxTmem[l->base], span);
    }
    {
        u32 params[6] = { t->fmt, t->siz, l->w, l->h, l->stride, t->palette | (l->tt << 8) };
        k = hashBytes(k, (const u8 *)params, sizeof params);
    }
    if (t->fmt == G_IM_FMT_CI) {
        k = hashBytes(k, &gGfxTmem[0x800], 0x200);
    }
    *key = k;
    return 1;
}

/* size and key only (rgba NULL): backends look the key up before paying for gfxTmemDecode */
int gfxTmemKey(const GfxState *st, int ti, GfxTexImage *out) {
    TileLayout l;
    if (!tileLayout(st, ti, &l, &out->key)) {
        return 0;
    }
    out->width = l.w;
    out->height = l.h;
    out->rgba = NULL;
    return 1;
}

int gfxTmemDecode(const GfxState *st, int ti, GfxTexImage *out) {
    const GfxTile *t = &st->tiles[ti];
    TileLayout l;
    u32 w, h, base, stride, tt, x, y;
    if (!tileLayout(st, ti, &l, &out->key)) {
        return 0;
    }
    w = l.w;
    h = l.h;
    base = l.base;
    stride = l.stride;
    tt = l.tt;
    out->width = w;
    out->height = h;
    if (sDecoded == NULL) {
        sDecoded = malloc(1024 * 1024 * 4);
    }
    out->rgba = sDecoded;
    for (y = 0; y < h; y++) {
        u32 rowAddr = base + y * stride;
        u32 xr = (y & 1) ? swapUnit(t->siz) : 0; /* sampling undoes the odd-line swap */
        u8 *o = &sDecoded[(y * w) * 4];
#define TMEM_AT(off) (&gGfxTmem[(rowAddr + ((off) ^ xr)) & 0xFFF])
        for (x = 0; x < w; x++, o += 4) {
            u8 b;
            switch (t->siz) {
            case G_IM_SIZ_4b:
                b = *TMEM_AT(x / 2);
                b = (x & 1) ? (b & 0xF) : (b >> 4);
                switch (t->fmt) {
                case G_IM_FMT_CI:
                    if (tt != G_TT_NONE) {
                        tlutColor(tt, (t->palette << 4) | b, o);
                        break;
                    }
                    /* fallthrough */
                case G_IM_FMT_I:
                    o[0] = o[1] = o[2] = o[3] = b * 17;
                    break;
                case G_IM_FMT_IA:
                    o[0] = o[1] = o[2] = (b >> 1) * 255 / 7;
                    o[3] = (b & 1) ? 255 : 0;
                    break;
                default:
                    o[0] = o[1] = o[2] = o[3] = b * 17;
                    break;
                }
                break;
            case G_IM_SIZ_8b:
                b = *TMEM_AT(x);
                switch (t->fmt) {
                case G_IM_FMT_CI:
                    if (tt != G_TT_NONE) {
                        tlutColor(tt, b, o);
                        break;
                    }
                    /* fallthrough */
                case G_IM_FMT_I:
                    o[0] = o[1] = o[2] = o[3] = b;
                    break;
                case G_IM_FMT_IA:
                    o[0] = o[1] = o[2] = (b >> 4) * 17;
                    o[3] = (b & 0xF) * 17;
                    break;
                default:
                    o[0] = o[1] = o[2] = o[3] = b;
                    break;
                }
                break;
            case G_IM_SIZ_16b: {
                const u8 *p = TMEM_AT(x * 2);
                if (t->fmt == G_IM_FMT_IA) {
                    o[0] = o[1] = o[2] = p[0];
                    o[3] = p[1];
                } else {
                    rgba16((u16)((p[0] << 8) | p[1]), o);
                }
                break;
            }
            default: {
                const u8 *p = TMEM_AT(x * 4);
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
                o[3] = p[3];
                break;
            }
            }
        }
#undef TMEM_AT
    }
    return 1;
}
