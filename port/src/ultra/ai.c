/* Audio interface: SDL queue of s16 stereo at gPortAudioRate (web); discarded in headless builds. */
#include "port_internal.h"
#ifndef PORT_HEADLESS
#include <SDL2/SDL.h>
#endif

#define AI_MAX_QUEUED_MS 200 /* queue ceiling */

static u32 sLastLen;
#ifndef PORT_HEADLESS
static SDL_AudioDeviceID sDev;
static s32 sDevRate; /* rate sDev was opened at: a quickload may restore a snapshot made at the other rate */
#else
static u32 sFakeQueued;
#endif

/* headless keeps nothing live: sFakeQueued/sLastLen are restored for determinism */
void portAiQsRanges(QsSink *out) {
#ifndef PORT_HEADLESS
    QS_KEEP(out, sDev);
    QS_KEEP(out, sLastLen);
    QS_KEEP(out, sDevRate);
#else
    (void)out;
#endif
}

#ifndef PORT_HEADLESS
static void openDevice(void) {
    SDL_AudioSpec want, have;
    if (sDev) {
        SDL_CloseAudioDevice(sDev);
        sDev = 0;
    }
    SDL_zero(want);
    want.freq = gPortAudioRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = gPortAudioRate > 22050 ? 1024 : 512;
    sDevRate = gPortAudioRate;
    sDev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sDev == 0) {
        portLog("PORT: SDL audio open failed: %s", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(sDev, 0);
}
#endif

void portAudioOutInit(void) {
#ifndef PORT_HEADLESS
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        portLog("PORT: SDL audio init failed: %s", SDL_GetError());
        return;
    }
    openDevice();
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
    u32 bytes = (u32)(ms * gPortAudioRate * 4 / 1000.0);
    sFakeQueued = sFakeQueued > bytes ? sFakeQueued - bytes : 0;
#else
    (void)ms;
#endif
}

s32 osAiSetFrequency(u32 freq) {
    (void)freq;
    return gPortAudioRate;
}

s32 osAiSetNextBuffer(void *buf, u32 len) {
    s16 *samples = (s16 *)portHostAddr((u32)buf);
    sLastLen = len;
    portAudioPeakFeed(samples, len / 2);
#ifndef PORT_HEADLESS
    if (sDev && sDevRate != gPortAudioRate) {
        openDevice();
    }
#endif
    if (queuedBytes() > (u32)gPortAudioRate * 4 * AI_MAX_QUEUED_MS / 1000) {
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
