/* RT64 texture packs (web/src/game/texpacks/): replacement lookup by RT64 hash and the texture dump. Browser only. */
#ifndef PORT_GFX_TEXPACK_H
#define PORT_GFX_TEXPACK_H
#include "gfx/gfx_gl.h"

enum { TEXPACK_UNLOADED, TEXPACK_REQUESTED, TEXPACK_READY, TEXPACK_FAILED };

typedef struct {
    GLuint tex;
    u32 w, h;
    u32 sampling; /* setSampling's packed parameters last set on tex, 0 unknown */
    u8 state, shiftHalf, mipmapped;
} GfxPackTex;

/* the pack entry replacing tile `tile` as decoded into img, or -1; queues img for the dump while one runs */
s32 gfxTexpackLookup(const GfxState *st, int tile, const GfxTexImage *img);
/* a READY entry's texture, else NULL; an UNLOADED entry becomes REQUESTED and is asked for from the page */
GfxPackTex *gfxTexpackTexture(s32 entry);
/* any pack entries installed */
int gfxTexpackActive(void);
int gfxTexpackQsRanges(QsRange *out);
#endif
