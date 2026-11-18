#include "port_internal.h"

typedef struct {
    OSMesgQueue *mq;
    OSMesg msg;
} EventEntry;

static EventEntry sEvents[OS_NUM_EVENTS];

void osSetEventMesg(OSEvent ev, OSMesgQueue *mq, OSMesg msg) {
    if (ev < OS_NUM_EVENTS) {
        sEvents[ev].mq = mq;
        sEvents[ev].msg = msg;
    }
}

void portPostEvent(int ev) {
    if (ev < OS_NUM_EVENTS && sEvents[ev].mq != NULL) {
        osSendMesg(sEvents[ev].mq, sEvents[ev].msg, OS_MESG_NOBLOCK);
    }
}

/* Interrupt masking has no meaning with cooperative threads (osGetIntMask: see misc.c). */
OSIntMask osSetIntMask(OSIntMask m) { (void)m; return 0; }
u32 __osDisableInt(void) { return 0; }
void __osRestoreInt(u32 m) { (void)m; }
u32 __osGetFpcCsr(void) { return 0; }
u32 __osSetFpcCsr(u32 v) { (void)v; return 0; }
