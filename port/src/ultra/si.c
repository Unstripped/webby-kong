/* Serial interface: controller pad 0 only, no rumble pak. */
#include "port_internal.h"

OSContPad gPortPad;

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *status) {
    int i;
    (void)mq;
    *bitpattern = 1;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        status[i].type = i == 0 ? CONT_TYPE_NORMAL : 0;
        status[i].status = 0;
        status[i].errno = i == 0 ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    return 0;
}

s32 osContStartReadData(OSMesgQueue *mq) {
    (void)mq;
    portPostEvent(OS_EVENT_SI);
    return 0;
}

void osContGetReadData(OSContPad *pad) {
    int i;
    u16 taps = portInputTakeTaps();
    for (i = 0; i < MAXCONTROLLERS; i++) {
        if (i == 0) {
            pad[i] = gPortPad;
            pad[i].button |= taps;
            pad[i].errno = 0;
        } else {
            memset(&pad[i], 0, sizeof pad[i]);
            pad[i].errno = CONT_NO_RESPONSE_ERROR;
        }
    }
}

s32 osContSetCh(u8 ch) {
    (void)ch;
    return 0;
}

s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel) {
    (void)mq;
    (void)pfs;
    (void)channel;
    return 1;
}

s32 __osMotorAccess(OSPfs *pfs, s32 flag) {
    (void)pfs;
    (void)flag;
    return 0;
}
