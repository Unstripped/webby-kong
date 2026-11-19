/* Replaces bootMainThread (src/boot/main.c): segment table, VI mode, then hand over to globalInit. */
#include "port_internal.h"
#include "segment.h"

extern const u32 gPortSegRom[18][2];

/* src/boot/main.c's globals that game code reads (main.c itself is replaced by this file). */
u32 sAudioFreq;
SegmentTableEntry gSegmentTable[18];
/* the decomp links D_8000DDCC (5A50, 6AD90, 7130: the sequence/sound bank ROM base) by value: entry 11's romStart */
__asm__(".globl D_8000DDCC\n.set D_8000DDCC, gSegmentTable+0x108\n");
void globalInit(OSMesgQueue *queue, s32 haltReason);

/* IPL3/libultra boot globals (0x80000300 on the N64). */
s32 osTvType = OS_TV_NTSC;
s32 osRomType = 0;
void *osRomBase = (void *)0xB0000000u;
s32 osResetType = 0;
s32 osCicId = 6105;
s32 osVersion = 0;
u32 osMemSize = 0x800000;
s32 osAppNMIBuffer[16];

static OSThread sBootThread;
static OSMesgQueue sBootQueue;
static OSMesg sBootQueueBuf[10];

void osInitialize(void) {}

static void portBoot(void *arg) {
    OSMesg msg;
    int i;
    (void)arg;
    osInitialize();
    osCreateViManager(OS_PRIORITY_VIMGR);
    osViSetMode(&osViModeTable[OS_VI_NTSC_LAN1]);
    osViBlack(1);
    sAudioFreq = osAiSetFrequency(22050);
    for (i = 0; i < 18; i++) {
        gSegmentTable[i].romStart = gPortSegRom[i][0];
        gSegmentTable[i].romEnd = gPortSegRom[i][1];
    }
    osCreateMesgQueue(&sBootQueue, sBootQueueBuf, 10);
    globalInit(&sBootQueue, 0);
    for (;;) {
        osRecvMesg(&sBootQueue, &msg, OS_MESG_BLOCK);
    }
}

void portBootStart(void) {
    osCreateThread(&sBootThread, 1, portBoot, NULL, NULL, 0);
    osStartThread(&sBootThread);
}
