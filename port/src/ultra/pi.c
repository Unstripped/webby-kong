/* Cartridge (PI) access: ROM bytes live in a JS Uint8Array (Module.portRom), never wholesale in wasm memory. */
#include "port_internal.h"
#include <emscripten.h>

EM_JS(u32, port_rom_size, (void), {
    return Module.portRom ? Module.portRom.length : 0;
});

EM_JS(int, port_rom_copy, (void *dst, u32 off, u32 len), {
    var rom = Module.portRom;
    if (!rom || off + len > rom.length || off + len < off) {
        return 0;
    }
    HEAPU8.set(rom.subarray(off, off + len), dst);
    return 1;
});

u32 portRomSize(void) {
    return port_rom_size();
}

void portRomCopy(void *dst, u32 romOff, u32 len) {
    if (len == 0) {
        return;
    }
    if ((u32)dst + len > PORT_GAME_LIMIT || (u32)dst < 0x400) {
        portAbort("PORT: ROM DMA to bad address %p len 0x%x", dst, len);
    }
    if (!port_rom_copy(dst, romOff, len)) {
        portAbort("PORT: ROM DMA out of range off 0x%x len 0x%x", romOff, len);
    }
}

u32 portRomRead32(u32 romOff) {
    u8 b[4];
    portRomCopy(b, romOff, 4);
    return portReadBE32(b);
}

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 cmdMsgCnt) {
    (void)pri;
    (void)cmdQ;
    (void)cmdBuf;
    (void)cmdMsgCnt;
}

s32 osPiStartDma(OSIoMesg *mb, s32 priority, s32 direction, u32 devAddr, void *vAddr, u32 nbytes, OSMesgQueue *mq) {
    (void)priority;
    if (direction != OS_READ) {
        portAbort("PORT: PI write DMA unsupported");
    }
    portRomCopy((void *)portHostAddr((u32)vAddr), devAddr & 0x0FFFFFFF, nbytes);
    if (mb != NULL) {
        mb->hdr.retQueue = mq;
        mb->dramAddr = vAddr;
        mb->devAddr = devAddr;
        mb->size = nbytes;
    }
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

s32 osPiRawStartDma(s32 direction, u32 devAddr, void *dramAddr, u32 size) {
    if (direction != OS_READ) {
        portAbort("PORT: PI raw write DMA unsupported");
    }
    portRomCopy((void *)portHostAddr((u32)dramAddr), devAddr & 0x0FFFFFFF, size);
    return 0;
}

s32 osPiReadIo(u32 devAddr, u32 *data) {
    *data = portRomRead32((devAddr & 0x1FFFFFFF) - 0x10000000);
    return 0;
}

u32 osPiGetStatus(void) {
    return 0;
}

u32 osVirtualToPhysical(void *p) {
    return portHostAddr((u32)p);
}

/* data-cache maintenance has no meaning on the host */
void osWritebackDCache(void *p, s32 n) { (void)p; (void)n; }
void osWritebackDCacheAll(void) {}
void osInvalDCache(void *p, s32 n) { (void)p; (void)n; }
void osInvalICache(void *p, s32 n) { (void)p; (void)n; }
