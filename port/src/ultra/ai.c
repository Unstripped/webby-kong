/* Audio interface: SDL queue at 22050 Hz stereo s16 (web); discarded in headless builds. */
#include "port_internal.h"
#ifndef PORT_HEADLESS
#include <SDL2/SDL.h>
#endif

#define AI_FREQ 22050
#define AI_MAX_QUEUED 17640u /* 0.2 s of s16 stereo */

static u32 sLastLen;
#ifndef PORT_HEADLESS
static SDL_AudioDeviceID sDev;
#else
static u32 sFakeQueued;
#endif

void portAudioOutInit(void) {
#ifndef PORT_HEADLESS
    SDL_AudioSpec want, have;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        portLog("PORT: SDL audio init failed: %s", SDL_GetError());
        return;
    }
    SDL_zero(want);
    want.freq = AI_FREQ;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    sDev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sDev == 0) {
        portLog("PORT: SDL audio open failed: %s", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(sDev, 0);
#endif
}

static u32 queuedBytes(void) {
#ifndef PORT_HEADLESS
    return sDev ? SDL_GetQueuedAudioSize(sDev) : 0;
#else
    return sFakeQueued;
#endif
}

/* headless: drain at the real rate, driven by the host loop */
void portAudioOutDrain(double ms) {
#ifdef PORT_HEADLESS
    u32 bytes = (u32)(ms * AI_FREQ * 4 / 1000.0);
    sFakeQueued = sFakeQueued > bytes ? sFakeQueued - bytes : 0;
#else
    (void)ms;
#endif
}

s32 osAiSetFrequency(u32 freq) {
    (void)freq;
    return AI_FREQ;
}

s32 osAiSetNextBuffer(void *buf, u32 len) {
    s16 *samples = (s16 *)portHostAddr((u32)buf);
    sLastLen = len;
    portAudioPeakFeed(samples, len / 2);
    if (queuedBytes() > AI_MAX_QUEUED) {
        return 0;
    }
#ifndef PORT_HEADLESS
    if (sDev) {
        SDL_QueueAudio(sDev, samples, len);
    }
#else
    sFakeQueued += len;
#endif
    return 0;
}

u32 osAiGetLength(void) {
    s32 q = (s32)queuedBytes() - (s32)sLastLen;
    if (q < 0) {
        q = 0;
    }
    if ((u32)q > sLastLen) {
        q = (s32)sLastLen;
    }
    return (u32)q;
}

u32 osAiGetStatus(void) {
    return 0;
}
