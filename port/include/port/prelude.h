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

#endif
