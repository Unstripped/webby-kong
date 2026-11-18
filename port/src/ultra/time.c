/* Virtual clock at the N64 CPU counter rate; advanced by the host loop. */
#include "port_internal.h"

static u64 sNow;
static u64 sOffset;
static OSTimer *sTimers; /* sorted by expiry (absolute time in OSTimer.value) */

u64 portTimeNow(void) {
    return sNow;
}

void portTimeAdvanceTo(u64 t) {
    if (t > sNow) {
        sNow = t;
    }
}

OSTime osGetTime(void) {
    return sNow - sOffset;
}

u32 osGetCount(void) {
    return (u32)sNow;
}

void osSetTime(OSTime t) {
    sOffset = sNow - t;
}

static void unlinkTimer(OSTimer *t) {
    OSTimer **pp = &sTimers;
    while (*pp != NULL) {
        if (*pp == t) {
            *pp = t->next;
            t->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void insertTimer(OSTimer *t) {
    OSTimer **pp = &sTimers;
    while (*pp != NULL && (*pp)->value <= t->value) {
        pp = &(*pp)->next;
    }
    t->next = *pp;
    t->prev = NULL;
    *pp = t;
}

int osSetTimer(OSTimer *t, OSTime countdown, OSTime interval, OSMesgQueue *mq, OSMesg msg) {
    unlinkTimer(t);
    t->interval = interval;
    t->value = sNow + (countdown != 0 ? countdown : interval);
    t->mq = mq;
    t->msg = msg;
    insertTimer(t);
    return 0;
}

int osStopTimer(OSTimer *t) {
    unlinkTimer(t);
    return 0;
}

u64 portTimersNextDue(void) {
    return sTimers != NULL ? sTimers->value : ~0ull;
}

/* Fire every timer due at or before `until`, in time order, running threads after each. */
void portTimersFire(u64 until) {
    while (sTimers != NULL && sTimers->value <= until) {
        OSTimer *t = sTimers;
        sTimers = t->next;
        t->next = NULL;
        portTimeAdvanceTo(t->value);
        if (t->interval != 0) {
            t->value += t->interval;
            insertTimer(t);
        }
        if (t->mq != NULL) {
            osSendMesg(t->mq, t->msg, OS_MESG_NOBLOCK);
        }
        portRunUntilIdle();
    }
}
