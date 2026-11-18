/* RSP/RDP: tasks are queued by the game and executed on the host context between fiber rounds. */
#include "port_internal.h"

#define MAX_TASKS 8

static OSTask *sQueue[MAX_TASKS];
static int sQueueCount;
/* RSP microcode text/data the game points its tasks at (global .text 0x80741310.., rspboot); the HLE
 * never reads them, they only need distinct addresses. */
u64 func_80741310[2];
u8 D_80741F40[16];
u8 D_807432D0[16];


void osSpTaskLoad(OSTask *t) {
    (void)t;
}

void osSpTaskStartGo(OSTask *t) {
    if (sQueueCount == MAX_TASKS) {
        portAbort("PORT: RSP task queue overflow");
    }
    sQueue[sQueueCount++] = t;
}

void osSpTaskYield(void) {}

OSYieldResult osSpTaskYielded(OSTask *t) {
    (void)t;
    return 0;
}

/* The scheduler freezes the RDP after queueing a framebuffer swap and thaws it on the retrace that shows
 * the new image; on hardware that holds the next gfx task until then, which is what paces the game. */
static int sDpFrozen;

void __osSpSetStatus(u32 v) { (void)v; }

void osDpSetStatus(u32 v) {
    if (v & 0x0008) { /* DPC_SET_FREEZE */
        sDpFrozen = 1;
    }
    if (v & 0x0004) { /* DPC_CLR_FREEZE */
        sDpFrozen = 0;
    }
}

u32 osDpGetStatus(void) {
    return sDpFrozen ? 0x0002 : 0; /* DPC_STATUS_FREEZE */
}

/* Run one queued task; returns 0 when none can run. A gfx task at the head waits while the RDP is
 * frozen (the RSP is busy with it, so nothing queued behind it runs either). */
int portSpRunPendingTasks(void) {
    OSTask *t;
    int i;
    if (sQueueCount == 0) {
        return 0;
    }
    t = sQueue[0];
    if (t->t.type == M_GFXTASK && sDpFrozen) {
        return 0;
    }
    for (i = 1; i < sQueueCount; i++) {
        sQueue[i - 1] = sQueue[i];
    }
    sQueueCount--;
    switch (t->t.type) {
    case M_GFXTASK:
        gPortStatGfxTasks++;
        portGfxRunTask(t);
        portPostEvent(OS_EVENT_SP);
        portPostEvent(OS_EVENT_DP);
        break;
    case M_AUDTASK:
        gPortStatAudTasks++;
        portAudioRunTask(t);
        portPostEvent(OS_EVENT_SP);
        break;
    default:
        portLog("PORT: unknown RSP task type %u", (unsigned)t->t.type);
        portPostEvent(OS_EVENT_SP);
        break;
    }
    return 1;
}
