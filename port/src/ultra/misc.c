/* Shims that must not see the libultra headers (their game-side prototypes differ from libultra's). */
#include "port/port.h"
#include <string.h>

u32 osAiGetLength(void);

void portBzero(void *p, int n) {
    memset(p, 0, (size_t)n);
}

void portBcopy(const void *src, void *dst, int n) {
    memmove(dst, src, (size_t)n);
}

int portBcmp(const void *a, const void *b, int n) {
    return memcmp(a, b, (size_t)n);
}

/* The game declares osGetIntMask(s32) and uses it as set-and-return; masks mean nothing here. */
s32 osGetIntMask(s32 mask) {
    (void)mask;
    return 0;
}

#define PHYS_SP_STATUS 0x04040010u
#define PHYS_AI_LEN 0x04500004u
#define SP_STATUS_HALT_BIT 0x0001u

u32 portIoRead(u32 reg) {
    reg &= 0x1FFFFFFFu;
    if (reg == PHYS_AI_LEN) {
        return osAiGetLength();
    }
    if (reg == PHYS_SP_STATUS) {
        return SP_STATUS_HALT_BIT;
    }
    return 0; /* DPC and everything else */
}

void portIoWrite(u32 reg, u32 value) {
    (void)reg;
    (void)value;
}

void portSwap16(void *p, u32 count) {
    u16 *w = p;
    u32 i;
    for (i = 0; i < count; i++) {
        w[i] = PORT_BE16(w[i]);
    }
}

void portSwap32(void *p, u32 count) {
    u32 *w = p;
    u32 i;
    for (i = 0; i < count; i++) {
        w[i] = PORT_BE32(w[i]);
    }
}
