/* Audio interface: SDL queue of s16 stereo at gPortAudioRate (web); discarded in headless builds.
 * Netplay: osAiGetLength steers the game's audio (and the sfx code draws from the sim RNG), so it reads a virtual
 * queue drained by VI ticks instead of the host's audio clock. */
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
/* netplay virtual queue: sim state (in the snapshot) */
static u32 sNetQueued, sNetLastLen;

void portAiNetViTick(void) {
    u32 bytes = (u32)(gPortAudioRate * 4 * PORT_VI_PERIOD_MS / 1000.0);
    if (portNetActive()) {
        sNetQueued = sNetQueued > bytes ? sNetQueued - bytes : 0;
    }
}

/* headless keeps nothing live: sFakeQueued/sLastLen are restored for determinism */
int portAiQsRanges(QsRange *out) {
#ifndef PORT_HEADLESS
    out[0] = QS_RANGE(sDev);
    out[1] = QS_RANGE(sLastLen);
    out[2] = QS_RANGE(sDevRate);
    return 3;
#else
    (void)out;
    return 0;
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
    u32 ceiling = (u32)gPortAudioRate * 4 * AI_MAX_QUEUED_MS / 1000;
    if (portNetActive()) {
        sNetLastLen = len;
    } else {
        sLastLen = len;
    }
    portAudioPeakFeed(samples, len / 2);
#ifndef PORT_HEADLESS
    if (sDev && sDevRate != gPortAudioRate) {
        openDevice();
    }
#endif
    if (portNetActive()) {
        if (sNetQueued > ceiling) {
            return 0;
        }
        sNetQueued += len;
#ifndef PORT_HEADLESS
        /* the host clock drifts from the VI clock: drop a backlog instead of letting latency grow */
        if (sDev && SDL_GetQueuedAudioSize(sDev) > (u32)gPortAudioRate * 4 * 400 / 1000) {
            SDL_ClearQueuedAudio(sDev);
        }
        if (sDev) {
            SDL_QueueAudio(sDev, samples, len);
        }
#endif
        return 0;
    }
    if (queuedBytes() > ceiling) {
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
    u32 last = portNetActive() ? sNetLastLen : sLastLen;
    s32 q = (s32)(portNetActive() ? sNetQueued : queuedBytes()) - (s32)last;
    if (q < 0) {
        q = 0;
    }
    if ((u32)q > last) {
        q = (s32)last;
    }
    return (u32)q;
}

u32 osAiGetStatus(void) {
    return 0;
}
