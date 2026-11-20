#ifndef PORT_INTERNAL_H
#define PORT_INTERNAL_H
#include "port/ultra.h"
#include <stdio.h>
#include <stdlib.h>

#define PORT_VI_HZ_NUM 60000
#define PORT_CPU_HZ 46875000ull
#define PORT_VI_PERIOD_MS 16.6834
#define PORT_CYCLES_PER_VI ((u64)(PORT_CPU_HZ * PORT_VI_PERIOD_MS / 1000.0))

_Noreturn void portAbort(const char *fmt, ...);
void portLog(const char *fmt, ...);
void portSetLastError(const char *msg);

/* threads (thread.c) */
void portThreadsInit(void);
void portRunUntilIdle(void);
int portInThread(void);
void portWakeViWaiters(void);
void portBlockCurrent(void *obj, int kind);
int portWakeOne(void *obj, int kind);
void portYieldToHigher(void);
#define PORT_WAIT_RECV 1
#define PORT_WAIT_SEND 2
#define PORT_WAIT_VI 3

/* events and time (event.c, time.c, vi.c) */
void portPostEvent(int ev);
void portTimeAdvanceTo(u64 t);
u64 portTimeNow(void);
void portTimersFire(u64 until);
u64 portTimersNextDue(void);
void portViTick(void);
extern void *gPortViCurrentFb;
extern void *gPortViPendingFb;
extern int gPortViBlack;
extern int gPortViSwapped;
extern u32 gPortViWidth;

/* sp.c */
int portSpRunPendingTasks(void);
void portGfxRunTask(OSTask *t);
void portAudioRunTask(OSTask *t);

/* gfx */
void portGfxInit(void);
int portGfxSetScale(int scale); /* 0 (auto) / 1..8; 0 on rejection; the next RSP task resizes the images */
int portGfxSetMsaa(int msaa);   /* 0/1/2/4/8 (1 = off); before gfxFbInit stores, after rebuilds; returns samples */
int portGfxMaxSamples(void);    /* GL_MAX_SAMPLES, 0 before gfxFbInit */
void portGfxPresent(void *fb, int black);

/* audio output (ai.c) */
void portAudioOutInit(void);

/* input.c */
void portInputPoll(void);
u16 portInputTakeTaps(void); /* buttons tapped since the last controller reads (osContGetReadData) */
extern OSContPad gPortPad;

/* rom (pi.c) */
u32 portRomSize(void);
void portRomCopy(void *dst, u32 romOff, u32 len);
u32 portRomRead32(u32 romOff);

/* save.c */
void portSaveInit(void);
void portSaveWritten(void);
void portSaveFlushIfDue(double nowMs);

/* overlay.c */
void portOverlayInit(void);
const char *port_debug_stats(void);
const char *port_level_progress(void); /* progress.c */
extern u16 gPortHeadlessButtons;
extern int gPortAssetStrict;
extern int gPortAssetTrace;
extern int gPortCurrentOverlay;

/* debug.c */
extern u32 gPortStatVi, gPortStatFrames, gPortStatGfxTasks, gPortStatAudTasks;
void portAudioPeakFeed(const s16 *samples, u32 count);
void portAudioPeakTick(void);
extern int gPortTracePrint; /* headless --trace: echo trace events to stdout */

/* main.c */
void portInit(void);
void port_frame_step(double elapsedMs);
void portGenLayoutCheck(void);

/* quicksave (port/src/qs.c): host-only statics kept from the live session across a restore */
typedef struct { void *p; u32 n; } QsRange;
#define QS_RANGE(v) ((QsRange){ (void *)&(v), (u32)sizeof(v) })
int portMainQsRanges(QsRange *out);
int portSaveQsRanges(QsRange *out);
int portInputQsRanges(QsRange *out);
int portAiQsRanges(QsRange *out);
int portDebugQsRanges(QsRange *out);
int portOverlayQsRanges(QsRange *out);
int gfxTmemQsRanges(QsRange *out);
int gfxBackendQsRanges(QsRange *out);
void gfxBackendQsRestored(void);
int portThreadsQsEncode(void);
void portThreadsQsRestoreLive(void);
int portThreadsQsDecode(void);
#endif
