/* N64 threads as emscripten fibers: cooperative, priority-based, libultra blocking/preemption semantics.
 * Every switch goes through the host context, which picks the highest-priority runnable thread
 * (ties: FIFO by the time the thread became runnable). */
#include "port_internal.h"
#include <emscripten/fiber.h>

#define MAX_THREADS 12
#define CSTACK_SIZE (128 * 1024)
#define ASTACK_SIZE (64 * 1024)

enum { TS_FREE, TS_CREATED, TS_STOPPED, TS_RUNNABLE, TS_WAITING, TS_RUNNING };

typedef struct {
    OSThread *os;
    int state;
    s32 pri;
    u64 seq;
    void *waitObj;
    int waitKind;
    void (*entry)(void *);
    void *arg;
    int fiberInit;
    emscripten_fiber_t fiber;
} PortThread;

static PortThread sThreads[MAX_THREADS];
static u8 sCStacks[MAX_THREADS][CSTACK_SIZE] __attribute__((aligned(16)));
static u8 sAStacks[MAX_THREADS][ASTACK_SIZE] __attribute__((aligned(16)));
static u8 sHostAStack[ASTACK_SIZE] __attribute__((aligned(16)));
static emscripten_fiber_t sHost;
static PortThread *sCurrent;
static u64 sSeq;
static int sInit;
static u8 sViObj;

void portThreadsInit(void) {
    if (!sInit) {
        emscripten_fiber_init_from_current_context(&sHost, sHostAStack, sizeof sHostAStack);
        sInit = 1;
    }
}

static PortThread *find(OSThread *t) {
    int i;
    for (i = 0; i < MAX_THREADS; i++) {
        if (sThreads[i].state != TS_FREE && sThreads[i].os == t) {
            return &sThreads[i];
        }
    }
    return NULL;
}

static PortThread *resolve(OSThread *t) {
    PortThread *r;
    if (t == NULL) {
        if (sCurrent == NULL) {
            portAbort("PORT: NULL thread outside a thread");
        }
        return sCurrent;
    }
    r = find(t);
    if (r == NULL) {
        portAbort("PORT: unknown thread %p", (void *)t);
    }
    return r;
}

int portInThread(void) {
    return sCurrent != NULL;
}

static void makeRunnable(PortThread *t) {
    t->state = TS_RUNNABLE;
    t->seq = ++sSeq;
    t->waitObj = NULL;
    t->waitKind = 0;
    if (t->os) {
        t->os->state = OS_STATE_RUNNABLE;
    }
}

static void switchToHost(void) {
    PortThread *t = sCurrent;
    emscripten_fiber_swap(&t->fiber, &sHost);
}

static void fiberMain(void *p) {
    PortThread *t = p;
    t->entry(t->arg);
    t->state = TS_STOPPED;
    for (;;) {
        switchToHost();
    }
}

static PortThread *pickRunnable(void) {
    PortThread *best = NULL;
    int i;
    for (i = 0; i < MAX_THREADS; i++) {
        PortThread *t = &sThreads[i];
        if (t->state != TS_RUNNABLE) {
            continue;
        }
        if (best == NULL || t->pri > best->pri || (t->pri == best->pri && t->seq < best->seq)) {
            best = t;
        }
    }
    return best;
}

void portRunUntilIdle(void) {
    PortThread *t;
    if (sCurrent != NULL) {
        portAbort("PORT: portRunUntilIdle from a thread");
    }
    while ((t = pickRunnable()) != NULL) {
        if (!t->fiberInit) {
            int idx = (int)(t - sThreads);
            emscripten_fiber_init(&t->fiber, fiberMain, t, sCStacks[idx], CSTACK_SIZE, sAStacks[idx], ASTACK_SIZE);
            t->fiberInit = 1;
        }
        t->state = TS_RUNNING;
        if (t->os) {
            t->os->state = OS_STATE_RUNNING;
        }
        sCurrent = t;
        emscripten_fiber_swap(&sHost, &t->fiber);
        sCurrent = NULL;
    }
}

void portBlockCurrent(void *obj, int kind) {
    PortThread *t = sCurrent;
    if (t == NULL) {
        portAbort("PORT: blocking call on the host context");
    }
    t->state = TS_WAITING;
    t->waitObj = obj;
    t->waitKind = kind;
    t->seq = ++sSeq;
    if (t->os) {
        t->os->state = OS_STATE_WAITING;
    }
    switchToHost();
}

int portWakeOne(void *obj, int kind) {
    PortThread *best = NULL;
    int i;
    for (i = 0; i < MAX_THREADS; i++) {
        PortThread *t = &sThreads[i];
        if (t->state == TS_WAITING && t->waitObj == obj && t->waitKind == kind) {
            if (best == NULL || t->pri > best->pri || (t->pri == best->pri && t->seq < best->seq)) {
                best = t;
            }
        }
    }
    if (best == NULL) {
        return 0;
    }
    makeRunnable(best);
    return 1;
}

void portWakeViWaiters(void) {
    while (portWakeOne(&sViObj, PORT_WAIT_VI)) {
    }
}

void portIdleWait(void) {
    portBlockCurrent(&sViObj, PORT_WAIT_VI);
}

/* Called by a running thread after it made another thread runnable or changed priorities:
 * switch away if a strictly higher-priority thread is runnable. */
void portYieldToHigher(void) {
    PortThread *t = sCurrent;
    PortThread *b;
    if (t == NULL) {
        return;
    }
    b = pickRunnable();
    if (b != NULL && b->pri > t->pri) {
        makeRunnable(t);
        switchToHost();
    }
}

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri) {
    PortThread *r = find(t);
    int i;
    (void)sp;
    if (r == NULL) {
        for (i = 0; i < MAX_THREADS; i++) {
            if (sThreads[i].state == TS_FREE) {
                r = &sThreads[i];
                break;
            }
        }
    }
    if (r == NULL) {
        portAbort("PORT: too many threads");
    }
    if (r->fiberInit && r == sCurrent) {
        portAbort("PORT: thread re-created while running");
    }
    memset(r, 0, sizeof *r);
    r->os = t;
    r->state = TS_CREATED;
    r->pri = pri;
    r->entry = entry;
    r->arg = arg;
    t->id = id;
    t->priority = pri;
    t->state = OS_STATE_STOPPED;
    t->next = NULL;
    t->queue = NULL;
}

void osStartThread(OSThread *t) {
    PortThread *r = resolve(t);
    if (r->state == TS_CREATED || r->state == TS_STOPPED) {
        if (r->state == TS_STOPPED && r->fiberInit && r->entry == NULL) {
            return;
        }
        makeRunnable(r);
        portYieldToHigher();
    }
}

void osStopThread(OSThread *t) {
    PortThread *r = resolve(t);
    r->state = TS_STOPPED;
    if (r->os) {
        r->os->state = OS_STATE_STOPPED;
    }
    if (r == sCurrent) {
        switchToHost();
    }
}

void osSetThreadPri(OSThread *t, OSPri pri) {
    PortThread *r = resolve(t);
    r->pri = pri;
    if (r->os) {
        r->os->priority = pri;
    }
    portYieldToHigher();
}

OSPri osGetThreadPri(OSThread *t) {
    return resolve(t)->pri;
}

OSId osGetThreadId(OSThread *t) {
    PortThread *r = resolve(t);
    return r->os ? r->os->id : 0;
}

void osYieldThread(void) {
    if (sCurrent != NULL) {
        makeRunnable(sCurrent);
        switchToHost();
    }
}

OSThread *__osGetCurrFaultedThread(void) {
    return NULL;
}
