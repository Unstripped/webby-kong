/* RT64's texture replacement hash (TMEMHasher), so RT64 texture packs match this renderer's TMEM. Depends only on
 * <stdint.h>: tests/tools/test_rt64hash.py builds it standalone with a host compiler. */
#ifndef PORT_GFX_RT64HASH_H
#define PORT_GFX_RT64HASH_H
#include <stdint.h>

typedef struct {
    uint8_t fmt, siz, palette, cms, cmt, masks, maskt;
    uint16_t line, tmem, uls, ult, lrs, lrt; /* uls..lrt 10.2, as GfxTile */
} Rt64Tile;

/* the size RT64 samples a draw's tile at; 0 when line == 0 (RT64: invalid tile, no hash) */
int rt64SampleSize(const Rt64Tile *t, uint16_t *w, uint16_t *h);
/* the tile can sample more bytes than TMEM holds: RT64 hashes all of TMEM (rt64RawTmemHash) */
int rt64RequiresRawTmem(const Rt64Tile *t, uint16_t w, uint16_t h, uint32_t tlut);
/* tlut: otherH & (3 << G_MDSFT_TEXTLUT); version: hash version 1..5 */
uint64_t rt64TmemHash(const uint8_t tmem[4096], const Rt64Tile *t, uint16_t w, uint16_t h, uint32_t tlut,
                      uint32_t version);
uint64_t rt64RawTmemHash(const uint8_t tmem[4096]);
#endif
