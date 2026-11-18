#include "port_internal.h"
#include <emscripten/stack.h>

extern unsigned char __heap_base;

u32 portHostAddr(u32 a) {
    return a >= 0x80000000u ? (a & 0x1FFFFFFFu) : a;
}

void portMemCheck(void) {
    u32 heap = (u32)&__heap_base;
    u32 stack = (u32)emscripten_stack_get_base();
    if (heap >= PORT_GAME_LIMIT || stack >= PORT_GAME_LIMIT) {
        portAbort("PORT: game-visible memory above 16MB (heap_base=0x%x stack_base=0x%x)", heap, stack);
    }
    printf("PORT: memory ok heap_base=0x%x stack_base=0x%x\n", heap, stack);
}
