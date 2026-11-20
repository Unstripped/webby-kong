/* Fills the objects whose bytes come from the ROM (build/gen/rom_fill.c) before game code or overlay snapshots read
 * them; the wasm carries placeholders only. Each segment's ROM range is inflated the way the game loads it (text
 * gzip member, then data gzip member) and every piece is copied from that stream, swapped to native order at its
 * directive width. */
#include "port_internal.h"
#include <string.h>

extern const u32 gPortSegRom[18][2];
void gunzip(u8 **inBufPtr, u8 **outBufPtr, void *huftStorage); /* src/boot/30E0.c */

void portRomFill(void) {
    void *huft = malloc(0x10000);
    u32 total = 0;
    u32 i, j;

    if (huft == NULL) {
        portAbort("PORT: rom fill out of memory");
    }
    for (i = 0; i < gPortRomFillSegCount; i++) {
        const PortRomFillSeg *s = &gPortRomFillSegs[i];
        u32 romStart = gPortSegRom[s->seg][0];
        u32 romLen = gPortSegRom[s->seg][1] - romStart;
        u8 *in = malloc(romLen);
        u8 *out = malloc(s->streamSize + 0x10000);
        u8 *src, *dst;
        u32 got;

        if (in == NULL || out == NULL) {
            portAbort("PORT: rom fill out of memory");
        }
        portRomRead(in, romStart, romLen);
        src = in;
        dst = out;
        gunzip(&src, &dst, huft);
        gunzip(&src, &dst, huft);
        got = (u32)(dst - out);
        if (got != s->streamSize) {
            portAbort("PORT: rom fill seg %u inflated 0x%x bytes, expected 0x%x", s->seg, got, s->streamSize);
        }
        for (j = 0; j < s->count; j++) {
            const PortRomFill *p = &s->fills[j];
            u8 *d = p->dst;
            u32 k;

            memcpy(d, out + p->off, p->size);
            if (p->width > 1) {
                for (k = 0; k + p->width <= p->size; k += p->width) {
                    u32 a = k, b = k + p->width - 1;
                    for (; a < b; a++, b--) {
                        u8 t = d[a];
                        d[a] = d[b];
                        d[b] = t;
                    }
                }
            }
        }
        total += s->count;
        free(in);
        free(out);
    }
    free(huft);
    printf("PORT: rom fill ok %u pieces\n", (unsigned)total);
}
