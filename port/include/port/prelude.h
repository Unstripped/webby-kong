#ifndef PORT_PRELUDE_H
#define PORT_PRELUDE_H
/* Force-included into every game TU (never into port TUs). */
#define PORT_GAME_TU 1
#include "port/port.h"
#include "port_abs.h" /* PORT_ABS_<sym>: addresses the decomp links by value (build/gen) */
#include "port_ovl_fn.h" /* OVL_FN(type, addr): per-overlay thunks for overlay functions in global data (build/gen) */

/* Game-defined libc functions that collide with musl. */
#define strlen dkStrlen
#define vsprintf dkVsprintf

/* libultra handwritten libc (asm/libultra/libc) -> port shims over memset/memmove/memcmp. */
#define bzero portBzero
#define bcopy portBcopy
#define bcmp portBcmp

/* Co-op (5 players): per-player tables the asm .bss sized for 4, relocated into game TUs (src/global/E4090.c).
 * The generated placeholder keeps its asm name, so the layout check is unaffected. */
#define gPlayerPadState gPortPlayerPadState

/* Co-op: a G_NOOP that tells the port's renderer which player's view the following commands draw
 * (port/src/gfx/gfx.c rasterises only the local player's view; PORT_VIEW_SHARED for everyone). */
#define gPortViewMarker(pkt, view)                                                                                     \
    {                                                                                                                  \
        Gfx *_g = (Gfx *)(pkt);                                                                                        \
        _g->words.w0 = ((u32)G_NOOP << 24) | PORT_G_VIEW_TAG;                                                          \
        _g->words.w1 = (u32)(view);                                                                                    \
    }

#endif
