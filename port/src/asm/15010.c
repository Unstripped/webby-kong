/*
 * C replacement for decomp/asm/global/15010.s (handwritten): COP0 Status-register helpers.
 * The port has no interrupts, so both are no-ops returning 0. Neither entry is referenced by game C.
 */
#include "port_internal.h"

/* func_80610310: returns the COP0 Status register. */
u32 func_80610310(void) {
    return 0;
}

/* func_8061032C: clears Status.IE and returns the previous IE bit. */
u32 func_8061032C(void) {
    return 0;
}
