/*
 * C transcription of decomp/asm/global/110F00.s (handwritten).
 */
#include "port_internal.h"
#include <math.h>

/* func_8070C200: round.w.s (FCSR default: round to nearest, ties to even), result taken as s16. */
s16 func_8070C200(f32 x) {
    return (s16)(s32)lrintf(x);
}
