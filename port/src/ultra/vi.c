#include "port_internal.h"

void *gPortViCurrentFb;
void *gPortViPendingFb;
int gPortViBlack = 1;
int gPortViSwapped;
u32 gPortViWidth = 320;

static OSMesgQueue *sViMq;
static OSMesg sViMsg;
static u32 sViRetrace = 1;
static u32 sViCount;
static OSViMode *sViMode;

void osCreateViManager(OSPri pri) {
    (void)pri;
}

void osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount) {
    sViMq = mq;
    sViMsg = msg;
    sViRetrace = retraceCount ? retraceCount : 1;
}

void osViSetMode(OSViMode *mode) {
    sViMode = mode;
    if (mode != NULL && mode->comRegs.width != 0) {
        gPortViWidth = mode->comRegs.width;
    }
}

u32 osViGetCurrentMode(void) {
    return sViMode != NULL ? sViMode->type : 0;
}

void osViBlack(u8 active) {
    gPortViBlack = active;
    gPortViSwapped = 1;
}

void osViSwapBuffer(void *fb) {
    gPortViPendingFb = fb;
}

void *osViGetCurrentFramebuffer(void) {
    return gPortViCurrentFb;
}

void *osViGetNextFramebuffer(void) {
    return gPortViPendingFb != NULL ? gPortViPendingFb : gPortViCurrentFb;
}

void osViSetSpecialFeatures(u32 func) {
    (void)func;
}

void osViSetXScale(f32 v) { (void)v; }
void osViSetYScale(f32 v) { (void)v; }

/* One vertical retrace: latch the pending framebuffer, wake VI waiters, post the VI event. */
void portViTick(void) {
    if (gPortViPendingFb != NULL) {
        gPortViCurrentFb = gPortViPendingFb;
        gPortViPendingFb = NULL;
        gPortViSwapped = 1;
    }
    portWakeViWaiters();
    sViCount++;
    if (sViMq != NULL && sViCount % sViRetrace == 0) {
        osSendMesg(sViMq, sViMsg, OS_MESG_NOBLOCK);
    }
}
