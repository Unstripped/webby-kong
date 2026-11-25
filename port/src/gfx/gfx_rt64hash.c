/* Ported from RT64 (MIT, Copyright (c) 2024 RT64 Contributors, see port/third_party/rt64/LICENSE) at commit
 * d9817c68ffd7328cd0af383af124971489366e18: TMEMHasher (src/common/rt64_tmem_hasher.h), the per-draw sample size
 * (src/hle/rt64_state.cpp) and the raw-TMEM hash (src/hle/rt64_rdp_tmem.cpp uploadTMEM). Texture packs are keyed by
 * these hashes, so every quirk is kept, each version's included. */
#include "gfx_rt64hash.h"
#define XXH_INLINE_ALL
#include "../../third_party/xxhash/xxhash.h"

#define RT64_TMEM_BYTES 4096u
#define RT64_SIZ_16B 2u
#define RT64_SIZ_32B 3u
#define RT64_SIZ_4B 0u
#define RT64_FMT_RGBA 0u
#define RT64_TX_CLAMP 2u

static int isRgba32(const Rt64Tile *t) {
    return t->siz == RT64_SIZ_32B && t->fmt == RT64_FMT_RGBA;
}

static int32_t minI(int32_t a, int32_t b) {
    return a < b ? a : b;
}

static int32_t maxI(int32_t a, int32_t b) {
    return a > b ? a : b;
}

int rt64SampleSize(const Rt64Tile *t, uint16_t *w, uint16_t *h) {
    int clampS, clampT;
    int32_t tileW, tileH, maskW, maskH;
    if (t->line == 0) {
        return 0;
    }
    clampS = t->masks == 0 || (t->cms & RT64_TX_CLAMP);
    clampT = t->maskt == 0 || (t->cmt & RT64_TX_CLAMP);
    tileW = clampS ? maxI(((int32_t)t->lrs - (int32_t)t->uls + 4) / 4, 1) : 0xFFFF;
    tileH = clampT ? maxI(((int32_t)t->lrt - (int32_t)t->ult + 4) / 4, 1) : 0xFFFF;
    maskW = t->masks > 0 ? (int32_t)1 << t->masks : 0xFFFF;
    maskH = t->maskt > 0 ? (int32_t)1 << t->maskt : 0xFFFF;
    *w = (uint16_t)minI(tileW, maskW);
    *h = (uint16_t)minI(tileH, maskH);
    return 1;
}

int rt64RequiresRawTmem(const Rt64Tile *t, uint16_t w, uint16_t h, uint32_t tlut) {
    uint32_t tmemSize = isRgba32(t) || tlut > 0 ? RT64_TMEM_BYTES >> 1 : RT64_TMEM_BYTES;
    uint32_t lastRowBytes = (uint32_t)w << (t->siz < RT64_SIZ_16B ? t->siz : RT64_SIZ_16B) >> 1;
    uint32_t bytesToHash = (uint32_t)(((int32_t)t->line << 3) * ((int32_t)h - 1)) + lastRowBytes;
    return bytesToHash > tmemSize;
}

typedef struct {
    XXH3_state_t xxh3;
    uint64_t tlutBitset[4];
    const uint8_t *tmem;
    uint32_t version, siz, usesTlut, tmemSize;
} HashCtx;

static void hashUpdate(HashCtx *c, const uint8_t *p, uint32_t n) {
    uint32_t i;
    XXH3_64bits_update(&c->xxh3, p, n);
    /* version 5 records which palette entries the texels use, to hash only those */
    if (c->version >= 5 && c->usesTlut) {
        if (c->siz == RT64_SIZ_4B) {
            for (i = 0; i < n; i++) {
                c->tlutBitset[0] |= 1ull << (p[i] & 0xFu);
                c->tlutBitset[0] |= 1ull << ((p[i] >> 4) & 0xFu);
            }
        } else {
            for (i = 0; i < n; i++) {
                c->tlutBitset[p[i] >> 6] |= 1ull << (p[i] & 0x3Fu);
            }
        }
    }
}

static void hashTmem(HashCtx *c, uint32_t base, uint32_t orAddr, uint32_t n, int oddRow) {
    /* too many bytes for one step: wrap around TMEM and hash the rest */
    if (base + n > c->tmemSize) {
        uint32_t first = c->tmemSize - base;
        hashUpdate(c, &c->tmem[base | orAddr], first);
        n = n - first < base ? n - first : base;
        base = 0;
    }
    /* version 4 hashes the odd-row word swap when hashing rows individually */
    if (oddRow && c->version >= 4) {
        uint32_t words = n / 8;
        if (words > 0) {
            hashUpdate(c, &c->tmem[base | orAddr], words * 8);
            base += words * 8;
            n -= words * 8;
        }
        if (n > 4) {
            hashUpdate(c, &c->tmem[base | orAddr], n - 4);
            hashUpdate(c, &c->tmem[(base + 4) | orAddr], 4);
        } else if (n > 0) {
            hashUpdate(c, &c->tmem[(base + 4) | orAddr], n);
        }
    } else {
        hashUpdate(c, &c->tmem[base | orAddr], n);
    }
}

static void hashLe(HashCtx *c, uint32_t v, uint32_t bytes) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    XXH3_64bits_update(&c->xxh3, b, bytes);
}

uint64_t rt64TmemHash(const uint8_t tmem[4096], const Rt64Tile *t, uint16_t w, uint16_t h, uint32_t tlut,
                      uint32_t version) {
    HashCtx c;
    int rgba32 = isRgba32(t);
    int halfTmem = rgba32;
    uint32_t drawBytesPerRow, drawBytesTotal, tmemMask, tmemAddress, tmemBytesPerRow, i;
    XXH3_64bits_reset(&c.xxh3);
    c.tlutBitset[0] = c.tlutBitset[1] = c.tlutBitset[2] = c.tlutBitset[3] = 0;
    c.tmem = tmem;
    c.version = version;
    c.siz = t->siz;
    c.usesTlut = tlut > 0;
    /* version 3 masks TLUT draws to the lower half of TMEM */
    if (version >= 3 && c.usesTlut) {
        halfTmem = 1;
    }
    c.tmemSize = halfTmem ? RT64_TMEM_BYTES >> 1 : RT64_TMEM_BYTES;
    drawBytesPerRow = (uint32_t)w << (rgba32 ? RT64_SIZ_16B : t->siz) >> 1;
    if (drawBytesPerRow < 1) {
        drawBytesPerRow = 1;
    }
    drawBytesTotal = (uint32_t)(((int32_t)t->line << 3) * ((int32_t)h - 1)) + drawBytesPerRow;
    tmemMask = halfTmem ? 2047u : 4095u;
    tmemAddress = ((uint32_t)t->tmem << 3) & tmemMask;
    tmemBytesPerRow = (uint32_t)t->line << 3;

    /* version 2 hashes row by row when rows don't use their whole TMEM line */
    if (version >= 2 && tmemBytesPerRow > drawBytesPerRow) {
        for (i = 0; i < h; i++) {
            hashTmem(&c, (tmemAddress + i * tmemBytesPerRow) & tmemMask, 0, drawBytesPerRow, i & 1);
        }
        if (rgba32) {
            for (i = 0; i < h; i++) {
                hashTmem(&c, (tmemAddress + i * tmemBytesPerRow) & tmemMask, c.tmemSize, drawBytesPerRow, i & 1);
            }
        }
    } else {
        hashTmem(&c, tmemAddress, 0, drawBytesTotal, 0);
        if (rgba32) {
            hashTmem(&c, tmemAddress, c.tmemSize, drawBytesTotal, 0);
        }
    }

    if (c.usesTlut) {
        int ci4 = t->siz == RT64_SIZ_4B;
        uint32_t paletteAddress = (RT64_TMEM_BYTES >> 1) + (ci4 ? (uint32_t)t->palette << 7 : 0);
        if (version >= 5) {
            if (ci4 && c.tlutBitset[0] == 0xFFFFu) {
                XXH3_64bits_update(&c.xxh3, &tmem[paletteAddress], 0x80);
            } else {
                uint32_t count = ci4 ? 1 : 4;
                for (i = 0; i < count; i++) {
                    if (c.tlutBitset[i] == UINT64_MAX) {
                        XXH3_64bits_update(&c.xxh3, &tmem[paletteAddress + i * 0x200], 0x200);
                    } else {
                        while (c.tlutBitset[i] > 0) {
                            uint32_t bit = (uint32_t)__builtin_ctzll(c.tlutBitset[i]);
                            XXH3_64bits_update(&c.xxh3, &tmem[paletteAddress + (i * 0x40 + bit) * 8], 8);
                            c.tlutBitset[i] &= ~(1ull << bit);
                        }
                    }
                }
            }
        } else {
            XXH3_64bits_update(&c.xxh3, &tmem[paletteAddress], ci4 ? 0x80 : 0x800);
        }
    }

    /* parameters that change the decoded image, as RT64 hashes them (little-endian in memory) */
    hashLe(&c, w, 2);
    hashLe(&c, h, 2);
    hashLe(&c, tlut, 4);
    hashLe(&c, t->line, 2);
    hashLe(&c, t->siz, 1);
    hashLe(&c, t->fmt, 1);
    return XXH3_64bits_digest(&c.xxh3);
}

uint64_t rt64RawTmemHash(const uint8_t tmem[4096]) {
    XXH3_state_t s;
    uint8_t tail[4] = { 0, 0, (uint8_t)(RT64_TMEM_BYTES & 0xFF), (uint8_t)(RT64_TMEM_BYTES >> 8) };
    XXH3_64bits_reset(&s);
    XXH3_64bits_update(&s, tmem, RT64_TMEM_BYTES);
    XXH3_64bits_update(&s, tail, sizeof tail); /* uint16_t byteOffset 0, uint16_t byteCount 4096 */
    return XXH3_64bits_digest(&s);
}
