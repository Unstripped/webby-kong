/* libultra message queues over the game's own OSMesgQueue fields; waiters live in the thread table. */
#include "port_internal.h"

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK || !portInThread()) {
            return -1;
        }
        portBlockCurrent(mq, PORT_WAIT_SEND);
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    if (portWakeOne(mq, PORT_WAIT_RECV)) {
        portYieldToHigher();
    }
    return 0;
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK || !portInThread()) {
            return -1;
        }
        portBlockCurrent(mq, PORT_WAIT_SEND);
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    if (portWakeOne(mq, PORT_WAIT_RECV)) {
        portYieldToHigher();
    }
    return 0;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag) {
    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) {
            return -1;
        }
        portBlockCurrent(mq, PORT_WAIT_RECV);
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    if (portWakeOne(mq, PORT_WAIT_SEND)) {
        portYieldToHigher();
    }
    return 0;
}
