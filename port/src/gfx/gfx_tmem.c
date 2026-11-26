/* 4 KB TMEM emulation and per-tile decode to RGBA8. Texels and TLUTs are big-endian in RDRAM.
 * Loads leave TMEM byte-identical to RT64's, whose replacement packs hash raw TMEM (gfx_rt64hash.c):
 * - every load swaps the 32-bit halves of each 64-bit word on odd lines (LoadTile/LoadTlut: odd rows; LoadBlock: odd
 *   lines of its dxt counter) and sampling swaps back on odd tile rows. Model textures loaded with dxt = 0 are stored
 *   pre-swizzled in RDRAM and only decode correctly this way.
 * - RGBA32 tiles split each texel: R,G in the lower 2 KB, B,A at the same offset in the upper 2 KB.
 * - a TLUT entry fills 8 bytes (its 2 bytes four times), as on hardware: entry i of a palette at 0x800 + i * 8. */
#include "gfx/gfx.h"

u8 gGfxTmem[4096];

/* The TMEM every load filled, newest last, and for a 16-bit block or tile load also where its texels came from:
 * a tile filled by exactly one such load can be drawn from the RDRAM image's GPU copy (gfx_fbsnap.c) instead of
 * its texels. Every load is recorded, whatever its format, so the search stops at the newest one that wrote the
 * TMEM the tile reads - an older record of the same shape describes texels that are no longer there. An RGBA32
 * load fills the same offset in both halves of TMEM, so it has two extents; a TLUT load fills the upper half and
 * can never match. The extents are linear byte ranges: they ignore the wraparound at the end of TMEM that
 * sampling does (tmemMask) and the stride a multi-row TLUT load writes with, so such a load can cover less than
 * it really wrote. For that to matter the tile would have to share its TMEM base, width and height with an older
 * 16-bit load of a copy, which also still has to stand for unchanged RDRAM bytes (gfx_fbsnap.c). */
#define TMEM_LOADS 8
static struct {
    u32 tmem, tmemEnd;   /* TMEM bytes the load filled */
    u32 tmem2, tmemEnd2; /* RGBA32: the other half */
    u32 src;             /* RDRAM address of the first texel */
    u32 width, height;   /* texels loaded */
    u32 stride;          /* source texels per row */
    u8 match;            /* the record can stand for a tile (a 16-bit block or tile load) */
} sLoads[TMEM_LOADS];
static u32 sLoadSeq;

static void recordLoad(const GfxTile *t, u32 tmemBytes, int rgba32, int match, u32 src, u32 width, u32 height,
                       u32 stride) {
    u32 i = sLoadSeq++ % TMEM_LOADS;
    u32 base = (u32)t->tmem * 8;
    sLoads[i].tmem = rgba32 ? (base & 0x7FF) : base;
    sLoads[i].tmemEnd = sLoads[i].tmem + tmemBytes;
    sLoads[i].tmem2 = rgba32 ? (base & 0x7FF) + 0x800 : 0;
    sLoads[i].tmemEnd2 = rgba32 ? sLoads[i].tmem2 + tmemBytes : 0;
    sLoads[i].src = src;
    sLoads[i].width = width;
    sLoads[i].height = height;
    sLoads[i].stride = stride;
    sLoads[i].match = (u8)match;
}

/* the newest load that filled tile `ti` whole, from 16-bit texels */
int gfxTmemTileSource(const GfxState *st, int ti, GfxTileSource *out) {
    const GfxTile *t = &st->tiles[ti];
    u32 w = gfxTileWidth(t), h = gfxTileHeight(t), base = (u32)t->tmem * 8, n;
    u32 row = t->line != 0 ? (u32)t->line * 8 : w * 2;
    u32 end = base + row * h; /* the TMEM the tile reads */
    if (w == 0 || h == 0 || t->siz != G_IM_SIZ_16b || t->fmt != G_IM_FMT_RGBA) {
        return 0;
    }
    for (n = sLoadSeq; n > 0 && n + TMEM_LOADS > sLoadSeq; n--) {
        const u32 i = (n - 1) % TMEM_LOADS;
        if (sLoads[i].match && sLoads[i].tmem == base && sLoads[i].width == w && sLoads[i].height == h) {
            out->addr = sLoads[i].src;
            out->width = w;
            out->height = h;
            out->stride = sLoads[i].stride;
            return 1;
        }
        if ((sLoads[i].tmem < end && sLoads[i].tmemEnd > base) ||
            (sLoads[i].tmemEnd2 > sLoads[i].tmem2 && sLoads[i].tmem2 < end && sLoads[i].tmemEnd2 > base)) {
            return 0; /* a newer load replaced the texels the tile reads */
        }
    }
    return 0;
}

/* port-only, never game-visible: lives above 16 MB (see memory model) */
static u8 *sDecoded;

void gfxTmemQsRanges(QsSink *out) {
    /* sLoads/sLoadSeq describe gGfxTmem, which a restore replaces, so they are restored with it */
    QS_KEEP(out, sDecoded);
}

static u32 texelBytesX2(u32 siz) {
    /* bytes per texel times two (4b = 1) */
    return siz == G_IM_SIZ_4b ? 1 : siz == G_IM_SIZ_8b ? 2 : siz == G_IM_SIZ_16b ? 4 : 8;
}

enum { LOAD_NORMAL, LOAD_RGBA32, LOAD_TLUT };

/* Ported from RT64's loadToTMEMCommon (MIT, Copyright (c) 2024 RT64 Contributors, see port/third_party/rt64/LICENSE;
 * src/hle/rt64_rdp.cpp at d9817c68ffd7328cd0af383af124971489366e18). Packs need RT64's TMEM bytes, quirks included,
 * not hardware accuracy. Copies are fixed-size: a per-byte loop costs load throughput. */
static void loadToTmem(u32 src, u32 srcStride, u32 tmemStart, u32 tmemStride, u32 wordsPerRow, u32 rowCount, u32 dxt,
                       int mode) {
    const u32 tmemMask = mode == LOAD_RGBA32 ? 0x7FF : 0xFFF;
    const u32 tmemAdvance = mode == LOAD_RGBA32 ? 4 : 8;
    const u32 srcAdvance = mode == LOAD_TLUT ? 2 : 8;
    u32 xr = 0, counter = 0;
    u32 srcRow = src, tmemRow = tmemStart & tmemMask;
    /* RT64 skips rows a wrapping load overwrites anyway (its tmemAddressRow line kept as is) */
    if (tmemStride > 0 && wordsPerRow * tmemAdvance <= tmemStride) {
        s32 rowsToSkip = (s32)rowCount - (s32)((tmemMask + tmemStride) / tmemStride);
        if (rowsToSkip > 0) {
            tmemRow += (tmemRow + tmemStride * (u32)rowsToSkip) & tmemMask;
            srcRow += srcStride * (u32)rowsToSkip;
            xr = ((u32)rowsToSkip & 1) << 2;
            rowCount -= (u32)rowsToSkip;
        }
    }
    for (; rowCount > 0; rowCount--) {
        const u8 *s = (const u8 *)srcRow;
        u32 tm = tmemRow & tmemMask; /* RT64 leaves the skip's row address unmasked: its first word lands past TMEM */
        u32 n;
        for (n = wordsPerRow; n > 0; n--) {
            u8 *d = &gGfxTmem[tm ^ xr];
            if (mode == LOAD_NORMAL) {
                if (xr == 0) {
                    memcpy(d, s, 8);
                } else {
                    /* tm is 8-aligned: the XOR exchanges the word's halves */
                    memcpy(&gGfxTmem[tm], s + 4, 4);
                    memcpy(&gGfxTmem[tm + 4], s, 4);
                }
            } else if (mode == LOAD_RGBA32) {
                /* tm is 4-aligned: R,G of both texels low, B,A high */
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[4];
                d[3] = s[5];
                d[0x800] = s[2];
                d[0x801] = s[3];
                d[0x802] = s[6];
                d[0x803] = s[7];
            } else {
                /* the entry four times: the XOR leaves the 2-byte pattern unchanged */
                u8 e[8] = { s[0], s[1], s[0], s[1], s[0], s[1], s[0], s[1] };
                memcpy(&gGfxTmem[tm], e, 8);
            }
            if (dxt != 0) {
                counter += dxt;
                while (counter >= 0x800) {
                    tm = (tm + tmemStride) & tmemMask;
                    counter -= 0x800;
                    xr ^= 4;
                }
            }
            s += srcAdvance;
            tm = (tm + tmemAdvance) & tmemMask;
        }
        tmemRow = (tmemRow + tmemStride) & tmemMask;
        srcRow += srcStride;
        xr ^= 4;
    }
}

static int isRgba32(const GfxTile *t) {
    return t->siz == G_IM_SIZ_32b && t->fmt == G_IM_FMT_RGBA;
}

void gfxTmemLoadBlock(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs,
                      u32 dxt) {
    const GfxTile *t = &st->tiles[tile];
    u32 srcStride = (timgWidth << timgSiz) >> 1;
    u32 src, words, total, row;
    if (lrs < uls) {
        return;
    }
    src = timg + ((uls << timgSiz) >> 1) + srcStride * ult;
    words = ((lrs - uls) >> (4 - t->siz)) + 1;
    loadToTmem(src, srcStride, (u32)t->tmem << 3, (u32)t->line << 3, words, 1, dxt,
               isRgba32(t) ? LOAD_RGBA32 : LOAD_NORMAL);
    /* a block is one run of 64-bit words; the tile's line splits it into rows, the last one possibly short */
    total = lrs - uls + 1;
    row = (u32)t->line * 4;
    if (t->siz != G_IM_SIZ_16b || row == 0 || row > total) {
        row = total;
    }
    recordLoad(t, words * 8, isRgba32(t), t->siz == G_IM_SIZ_16b && total % row == 0, src, row, total / row, row);
}

void gfxTmemLoadTile(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs,
                     u32 lrt) {
    const GfxTile *t = &st->tiles[tile];
    u32 srcStride = (timgWidth << timgSiz) >> 1;
    u32 x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2;
    u32 src, words, rowBytes;
    if (x1 < x0 || y1 < y0) {
        return;
    }
    src = timg + ((x0 << timgSiz) >> 1) + srcStride * y0;
    words = ((x1 - x0) >> (4 - t->siz)) + 1;
    loadToTmem(src, srcStride, (u32)t->tmem << 3, (u32)t->line << 3, words, y1 - y0 + 1, 0,
               isRgba32(t) ? LOAD_RGBA32 : LOAD_NORMAL);
    rowBytes = t->line != 0 ? (u32)t->line * 8 : words * 8;
    recordLoad(t, rowBytes * (y1 - y0 + 1), isRgba32(t), t->siz == G_IM_SIZ_16b, src, x1 - x0 + 1, y1 - y0 + 1,
               srcStride >> 1);
}

void gfxTmemLoadTlut(const GfxState *st, u32 timg, u32 timgSiz, u32 timgWidth, int tile, u32 uls, u32 ult, u32 lrs,
                     u32 lrt) {
    const GfxTile *t = &st->tiles[tile];
    u32 srcStride = (timgWidth << timgSiz) >> 1;
    u32 x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2;
    if (x1 < x0 || y1 < y0) {
        return;
    }
    loadToTmem(timg + ((x0 << timgSiz) >> 1) + srcStride * y0, srcStride, (u32)t->tmem << 3, (u32)t->line << 5,
               x1 - x0 + 1, y1 - y0 + 1, 0, LOAD_TLUT);
    /* every entry fills 8 bytes of the upper half; never a source a tile can be drawn from */
    recordLoad(t, (x1 - x0 + 1) * (y1 - y0 + 1) * 8, 0, 0, 0, 0, 0, 0);
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
    const u8 *e = &gGfxTmem[0x800 + (index & 0xFF) * 8];
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

/* hashes n bytes of TMEM from start & mask, wrapping at mask + 1, each address OR'd with orAddr */
static u64 hashSpan(u64 k, u32 start, u32 n, u32 mask, u32 orAddr) {
    u32 s = start & mask, first = mask + 1 - s;
    if (n <= first) {
        return hashBytes(k, &gGfxTmem[s | orAddr], n);
    }
    k = hashBytes(k, &gGfxTmem[s | orAddr], first);
    return hashBytes(k, &gGfxTmem[orAddr], n - first);
}

static int tileLayout(const GfxState *st, int ti, TileLayout *l, u64 *key) {
    const GfxTile *t = &st->tiles[ti];
    int rgba32 = isRgba32(t);
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
    if (l->stride == 0) {
        l->stride = rgba32 ? l->w * 2 : (l->w * texelBytesX2(t->siz) + 1) / 2; /* RGBA32: bytes per half */
    }
    /* sampling wraps at the end of TMEM (TMEM_AT below), so the key covers the wrapped span */
    span = l->stride * l->h;
    if (rgba32) {
        if (span > 0x800) {
            span = 0x800;
        }
        k = hashSpan(k, l->base, span, 0x7FF, 0);
        k = hashSpan(k, l->base, span, 0x7FF, 0x800);
    } else {
        if (span > sizeof gGfxTmem) {
            span = sizeof gGfxTmem;
        }
        k = hashSpan(k, l->base, span, 0xFFF, 0);
    }
    {
        u32 params[6] = { t->fmt, t->siz, l->w, l->h, l->stride, t->palette | (l->tt << 8) };
        k = hashBytes(k, (const u8 *)params, sizeof params);
    }
    if (t->fmt == G_IM_FMT_CI) {
        k = hashBytes(k, &gGfxTmem[0x800], 0x800); /* all 256 quadricated entries */
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
        u32 xr = (y & 1) ? 4 : 0; /* sampling undoes the odd-line swap */
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
                if (t->fmt == G_IM_FMT_RGBA) {
                    /* R,G in the lower half, B,A at the same offset in the upper half */
                    u32 a = (rowAddr + ((x * 2) ^ xr)) & 0x7FF;
                    o[0] = gGfxTmem[a];
                    o[1] = gGfxTmem[a + 1];
                    o[2] = gGfxTmem[0x800 | a];
                    o[3] = gGfxTmem[(0x800 | a) + 1];
                } else {
                    const u8 *p = TMEM_AT(x * 4);
                    o[0] = p[0];
                    o[1] = p[1];
                    o[2] = p[2];
                    o[3] = p[3];
                }
                break;
            }
            }
        }
#undef TMEM_AT
    }
    return 1;
}
