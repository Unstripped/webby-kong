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

static void swapHalves(u8 *p, u32 unit) {
    u8 tmp[8];
    memcpy(tmp, p, unit);
    memmove(p, p + unit, unit);
    memcpy(p + unit, tmp, unit);
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
    /* the dxt counter advances once per 64-bit RDRAM word; its integer part is the TMEM line */
    for (i = 0; i + group <= bytes; i += group) {
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

u32 gfxTileWidth(const GfxTile *t) {
    u32 w = ((t->lrs - t->uls) >> 2) + 1;
    if (t->masks && (1u << t->masks) < w) {
        w = 1u << t->masks;
    }
    return w;
}

u32 gfxTileHeight(const GfxTile *t) {
    u32 h = ((t->lrt - t->ult) >> 2) + 1;
    if (t->maskt && (1u << t->maskt) < h) {
        h = 1u << t->maskt;
    }
    return h;
}

static u64 fnv(u64 h, const u8 *p, u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
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

int gfxTmemDecode(const GfxState *st, int ti, GfxTexImage *out) {
    const GfxTile *t = &st->tiles[ti];
    u32 w = gfxTileWidth(t), h = gfxTileHeight(t);
    u32 base = (u32)t->tmem * 8;
    u32 stride = (u32)t->line * 8;
    u32 tt = st->otherH & (3u << G_MDSFT_TEXTLUT);
    u32 x, y;
    u64 key = 1469598103934665603ull;
    u32 span;
    if (w == 0 || h == 0 || w > 1024 || h > 1024) {
        return 0;
    }
    if (t->siz == G_IM_SIZ_32b) {
        stride *= 2;
    }
    if (stride == 0) {
        stride = (w * texelBytesX2(t->siz) + 1) / 2;
    }
    /* sampling wraps at the end of TMEM (TMEM_AT below), so the key covers the wrapped span */
    span = stride * h;
    if (span > sizeof gGfxTmem) {
        span = sizeof gGfxTmem;
    }
    if (base + span > sizeof gGfxTmem) {
        key = fnv(key, &gGfxTmem[base], sizeof gGfxTmem - base);
        key = fnv(key, gGfxTmem, base + span - sizeof gGfxTmem);
    } else {
        key = fnv(key, &gGfxTmem[base], span);
    }
    {
        u32 params[6] = { t->fmt, t->siz, w, h, stride, t->palette | (tt << 8) };
        key = fnv(key, (const u8 *)params, sizeof params);
    }
    if (t->fmt == G_IM_FMT_CI) {
        key = fnv(key, &gGfxTmem[0x800], 0x200);
    }
    out->width = w;
    out->height = h;
    out->key = key;
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
