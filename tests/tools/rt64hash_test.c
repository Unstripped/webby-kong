/* Prints RT64 hashes of a fixed TMEM pattern for tests/tools/test_rt64hash.py: one line per case,
 * "name w h raw hash" (hash in lowercase hex; the raw-TMEM hash when RT64 would use it). */
#include <stdint.h>
#include <stdio.h>
#include "gfx/gfx_rt64hash.h"

typedef struct {
    const char *name;
    Rt64Tile tile;
    uint32_t tlut, version;
} Case;

/* fmt, siz, palette, cms, cmt, masks, maskt, line, tmem, uls, ult, lrs, lrt */
#define TILE(fmt, siz, line, tmem, pal, cms, cmt, masks, maskt, uls, ult, lrs, lrt) \
    { fmt, siz, pal, cms, cmt, masks, maskt, line, tmem, uls, ult, lrs, lrt }

static const Case kCases[] = {
    { "rgba16_32x32_v5", TILE(0, 2, 8, 0x000, 0, 0, 0, 5, 5, 0, 0, 31 << 2, 31 << 2), 0, 5 },
    { "rgba16_32x32_v1", TILE(0, 2, 8, 0x000, 0, 0, 0, 5, 5, 0, 0, 31 << 2, 31 << 2), 0, 1 },
    { "ci4_16x16_rgba_v5", TILE(2, 0, 1, 0x000, 3, 0, 0, 4, 4, 0, 0, 15 << 2, 15 << 2), 0x8000, 5 },
    { "ci4_16x16_rgba_v4", TILE(2, 0, 1, 0x000, 3, 0, 0, 4, 4, 0, 0, 15 << 2, 15 << 2), 0x8000, 4 },
    { "ci8_32x16_ia_clamp_v5", TILE(2, 1, 4, 0x040, 0, 2, 2, 5, 4, 0, 0, 31 << 2, 15 << 2), 0xC000, 5 },
    { "rgba32_16x16_v5", TILE(0, 3, 4, 0x000, 0, 0, 0, 4, 4, 0, 0, 15 << 2, 15 << 2), 0, 5 },
    { "ia8_32x8_rows_v5", TILE(3, 1, 8, 0x010, 0, 0, 0, 5, 3, 0, 0, 31 << 2, 7 << 2), 0, 5 },
    { "ia8_32x8_rows_v3", TILE(3, 1, 8, 0x010, 0, 0, 0, 5, 3, 0, 0, 31 << 2, 7 << 2), 0, 3 },
    { "i4_negspan_clamp_v5", TILE(4, 0, 2, 0x100, 0, 2, 2, 0, 0, 10 << 2, 0, 2 << 2, 7 << 2), 0, 5 },
    { "rgba16_wrap_v5", TILE(0, 2, 16, 0x1F0, 0, 0, 0, 6, 5, 0, 0, 63 << 2, 31 << 2), 0, 5 },
    { "ci4_2x1_partial_v5", TILE(2, 0, 1, 0x020, 5, 0, 2, 1, 0, 0, 0, 1 << 2, 0), 0x8000, 5 },
    { "ci4_2x1_partial_v4", TILE(2, 0, 1, 0x020, 5, 0, 2, 1, 0, 0, 0, 1 << 2, 0), 0x8000, 4 },
    { "ia8_12x4_v5", TILE(3, 1, 2, 0x030, 0, 2, 2, 0, 0, 0, 0, 11 << 2, 3 << 2), 0, 5 },
    { "ia8_12x4_v3", TILE(3, 1, 2, 0x030, 0, 2, 2, 0, 0, 0, 0, 11 << 2, 3 << 2), 0, 3 },
    { "ia8_12x4_v1", TILE(3, 1, 2, 0x030, 0, 2, 2, 0, 0, 0, 0, 11 << 2, 3 << 2), 0, 1 },
    { "rgba16_raw_v5", TILE(0, 2, 64, 0x000, 0, 2, 2, 0, 0, 0, 0, 255 << 2, 63 << 2), 0, 5 },
};

int main(void) {
    static uint8_t tmem[4096];
    unsigned i;
    for (i = 0; i < sizeof tmem; i++) {
        tmem[i] = (uint8_t)((i * 37 + 11) ^ (i >> 5));
    }
    for (i = 0; i < sizeof kCases / sizeof kCases[0]; i++) {
        const Case *c = &kCases[i];
        uint16_t w = 0, h = 0;
        int raw;
        uint64_t hash;
        if (!rt64SampleSize(&c->tile, &w, &h)) {
            printf("%s invalid\n", c->name);
            continue;
        }
        raw = rt64RequiresRawTmem(&c->tile, w, h, c->tlut);
        hash = raw ? rt64RawTmemHash(tmem) : rt64TmemHash(tmem, &c->tile, w, h, c->tlut, c->version);
        printf("%s %u %u %d %016llx\n", c->name, (unsigned)w, (unsigned)h, raw, (unsigned long long)hash);
    }
    return 0;
}
