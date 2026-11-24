/* Host entry and main loop: VI ticks, fiber rounds, RSP tasks, timers, present. */
#include "port_internal.h"
#include <emscripten.h>
#include <math.h>

void portMemCheck(void);
void portBootStart(void);
void portAudioOutDrain(double ms);

static double sAccumMs;       /* real time not yet run as VIs */
static double sRefreshMs;     /* display refresh interval estimate; 0 until the first frame */
static int sRefreshMisses;    /* consecutive frame intervals off that estimate */
static int sRefreshesSinceVi; /* display refreshes since the last frame that ran a VI */
f32 gPortDrawDistance = 4.0f;
s32 gPortKongSwap = 1;
s32 gPortStickCamera = 1;
s32 gPortCameraSpeed = 5;
s32 gPortCameraInvert = 0;

/* --drawdist <1..16>: multiplier on object draw distances and enemy spawn ranges (renderExtendDistance); 1 = N64.
 * Returns the value in effect. */
static float setDrawDistance(double v) {
    if (v >= 1.0 && v <= 16.0) {
        gPortDrawDistance = (f32)v;
    } else {
        portLog("PORT: ignoring drawdist %g (1..16)", v);
    }
    return gPortDrawDistance;
}

/* --audio-rate 22050|44100: synthesizer and output rate (22050 = N64); before boot only */
static void setAudioRate(int rate) {
    if (rate == 22050 || rate == 44100) {
        gPortAudioRate = rate;
    } else {
        portLog("PORT: ignoring audio rate %d (22050 or 44100)", rate);
    }
}

/* --hrtf 0|1: binaural 3D sound for positional effects; returns the value in effect */
static s32 setAudioHrtf(s32 on) {
    gPortAudioHrtf = on != 0;
    return gPortAudioHrtf;
}

/* --camera-speed <1..10>: right-stick camera turn rate (5 = the C-button turn); returns the value in effect */
static s32 setCameraSpeed(s32 v) {
    if (v >= 1 && v <= 10) {
        gPortCameraSpeed = v;
    } else {
        portLog("PORT: ignoring camera speed %d (1..10)", v);
    }
    return gPortCameraSpeed;
}

/* --camera-invert <0..3>: right-stick camera invert, bit 0 horizontal, bit 1 vertical; returns the value in effect */
static s32 setCameraInvert(s32 v) {
    if (v >= 0 && v <= 3) {
        gPortCameraInvert = v;
    } else {
        portLog("PORT: ignoring camera invert %d (0..3)", v);
    }
    return gPortCameraInvert;
}

static void runAll(void) {
    portRunUntilIdle();
    while (portSpRunPendingTasks()) {
        portRunUntilIdle();
    }
}

static void viTick(void) {
    u64 end = portTimeNow() + PORT_CYCLES_PER_VI;
    portGfxRunDeferred(); /* last VI's in-between image, before the game runs on */
    gPortStatVi++;
    portAudioPeakTick();
    portViTick();
    runAll();
    while (portTimersNextDue() < end) {
        portTimersFire(portTimersNextDue());
        runAll();
    }
    portTimeAdvanceTo(end);
}

void portInit(void) {
    portMemCheck();
    portGenLayoutCheck();
    portRomFill();
    portThreadsInit();
    portOverlayInit();
    portSaveInit();
    portGfxInit();
    portAudioOutInit();
    portBootStart();
}

/* VIs to run for a frame of elapsedMs. A display whose refresh is a whole fraction of the VI period (60, 120,
 * 240 Hz) is phase-locked: a VI every k refreshes, on cadence while the time owed stays within a band of 3/4 refresh
 * around the schedule; past the band a VI moves one refresh earlier or later, which re-centres the phase. The
 * display runs at 60 Hz against the VI's 59.94, so that happens once per ~1000 VIs, where a plain accumulator
 * flaps between early and late on every frame of timestamp jitter while its phase crosses a VI boundary. Other
 * refresh rates (144 Hz) use the plain accumulator. */
static int sPaceRefreshesPerVi = 1; /* k while phase-locked, else 1 */

int portPaceRefreshesPerVi(void) {
    return sPaceRefreshesPerVi;
}

/* *ticks: refreshes this frame while phase-locked, else VIs (frame interpolation counts its images in them) */
static int viCountFor(double elapsedMs, int *ticks) {
    int n, k;
    if (elapsedMs > 2.0 && elapsedMs < 50.0) {
        /* a run of intervals off the estimate re-seeds it: the first frame's synthetic VI period (port_frame),
         * or the window moving to a display with another refresh rate */
        if (sRefreshMs != 0 && fabs(elapsedMs - sRefreshMs) < 0.3 * sRefreshMs) {
            sRefreshMs += 0.02 * (elapsedMs - sRefreshMs);
            sRefreshMisses = 0;
        } else if (sRefreshMs == 0 || ++sRefreshMisses >= 8) {
            sRefreshMs = elapsedMs;
            sRefreshMisses = 0;
        }
    }
    sAccumMs += elapsedMs;
    k = sRefreshMs > 0 ? (int)(PORT_VI_PERIOD_MS / sRefreshMs + 0.5) : 0;
    if (k >= 1 && k <= 8 && fabs(k * sRefreshMs - PORT_VI_PERIOD_MS) < 0.02 * PORT_VI_PERIOD_MS) {
        double band = 0.75 * sRefreshMs;
        int refreshes = (int)(elapsedMs / sRefreshMs + 0.5);
        refreshes = refreshes > 1 ? refreshes : 1;
        sRefreshesSinceVi += refreshes;
        sPaceRefreshesPerVi = k;
        *ticks = refreshes;
        if (sRefreshesSinceVi >= k) {
            n = sAccumMs - PORT_VI_PERIOD_MS >= -band; /* on cadence unless a band ahead */
        } else {
            /* early: what would be owed after a VI now, plus the refreshes still due before the cadence, exceeds the band */
            n = sAccumMs - PORT_VI_PERIOD_MS + (k - sRefreshesSinceVi) * sRefreshMs > band;
        }
        while (n >= 1 && n < 4 && sAccumMs - (n + 1) * PORT_VI_PERIOD_MS > band) {
            n++; /* a long frame */
        }
        if (n == 4 && sAccumMs - n * PORT_VI_PERIOD_MS > band) {
            sAccumMs = n * PORT_VI_PERIOD_MS; /* further behind than 4 VIs: drop the rest */
        }
    } else {
        n = (int)(sAccumMs / PORT_VI_PERIOD_MS);
        if (n > 4) {
            n = 4;
            sAccumMs = n * PORT_VI_PERIOD_MS;
        }
        sPaceRefreshesPerVi = 1;
        *ticks = n;
    }
    sAccumMs -= n * PORT_VI_PERIOD_MS;
    if (n > 0) {
        sRefreshesSinceVi = 0;
    }
    return n;
}

void port_frame_step(double elapsedMs) {
    int n, ticks;
    portInputPoll();
    portAudioOutDrain(elapsedMs);
    n = viCountFor(elapsedMs, &ticks);
    portGfxInterpTick(ticks); /* counts toward the pending in-between images, before a VI starts new ones */
    while (n-- > 0) {
        viTick();
    }
    if (gPortViSwapped) {
        gPortViSwapped = 0;
        gPortStatFrames++;
        portGfxPresent(gPortViCurrentFb, gPortViBlack);
    } else if (!gPortViBlack && portGfxPresentInterp()) {
        gPortStatFrames++; /* an in-between image on a refresh before the next game frame's swap */
    }
    portSaveFlushIfDue(emscripten_get_now());
}

#ifdef PORT_HEADLESS
/* ---------------------------------------------------------------- node headless runner */
#include <string.h>

EM_JS(int, port_node_load_rom, (const char *path), {
    var fs = require('fs');
    var crypto = require('crypto');
    var data;
    try {
        data = new Uint8Array(fs.readFileSync(UTF8ToString(path)));
    } catch (e) {
        console.log('PORT: cannot read ROM: ' + e.message);
        return 0;
    }
    var sha = crypto.createHash('sha1').update(data).digest('hex');
    if (sha !== 'cf806ff2603640a748fca5026ded28802f1f4a50') {
        console.log('PORT: unsupported ROM sha1 ' + sha);
        return 0;
    }
    Module.portRom = data;
    return 1;
});

typedef struct {
    u32 frame;
    int press;
    u16 button;
} InputEvent;

static InputEvent sEvents[256];
static int sEventCount;
static u16 sHeldButtons;
/* gPortHeadlessButtons lives in input.c */

int portMainQsRanges(QsRange *out) {
    (void)out;
    return 0;
}

/* snapshot bytes <-> wasm memory, in port_qs_table order (JS: the RDRAM range starts at address 0) */
EM_JS(void, port_qs_copy_out, (u8 *dst, const u32 *t), {
    var o = dst;
    for (var k = 0; k < 6; k += 2) {
        var s = HEAPU32[(t >> 2) + k], n = HEAPU32[(t >> 2) + k + 1];
        HEAPU8.copyWithin(o, s, s + n);
        o += n;
    }
});

EM_JS(void, port_qs_copy_in, (const u8 *src, const u32 *t), {
    var o = src;
    for (var k = 0; k < 6; k += 2) {
        var s = HEAPU32[(t >> 2) + k], n = HEAPU32[(t >> 2) + k + 1];
        HEAPU8.copyWithin(s, o, o + n);
        o += n;
    }
});

u32 *port_qs_table(void);
u32 port_qs_size(void);
int port_qs_save_begin(void);
void port_qs_save_end(void);
u8 *port_qs_load_begin(void);
int port_qs_load_end(u8 *stash);
void port_resume_request(s32 slot, s32 kong, s32 map, s32 exit, s32 hasPos, f32 x, f32 y, f32 z, s32 yaw);

static u16 buttonByName(const char *s) {
    static const struct { const char *n; u16 b; } map[] = {
        { "a", A_BUTTON }, { "b", B_BUTTON }, { "z", Z_TRIG }, { "start", START_BUTTON },
        { "dup", U_JPAD }, { "ddown", D_JPAD }, { "dleft", L_JPAD }, { "dright", R_JPAD },
        { "l", L_TRIG }, { "r", R_TRIG }, { "cup", U_CBUTTONS }, { "cdown", D_CBUTTONS },
        { "cleft", L_CBUTTONS }, { "cright", R_CBUTTONS },
    };
    size_t i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcmp(map[i].n, s) == 0) {
            return map[i].b;
        }
    }
    portAbort("PORT: unknown button %s", s);
}

static void loadInputFile(const char *path) {
    FILE *f = fopen(path, "r");
    char line[128];
    if (f == NULL) {
        portAbort("PORT: cannot open input file %s", path);
    }
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned frame;
        char verb[16], btn[16];
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (sscanf(line, "frame %u %15s %15s", &frame, verb, btn) == 3 && sEventCount < 256) {
            sEvents[sEventCount].frame = frame;
            sEvents[sEventCount].press = strcmp(verb, "press") == 0;
            sEvents[sEventCount].button = buttonByName(btn);
            sEventCount++;
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *rom = NULL, *input = NULL;
    u32 frames = 600, i;
    int a, e;
    u8 *qsBuf = NULL;
    u32 qsAt = 0, qsLoadAt = 0;
    int qsLoaded = 0;
    const char *resume = NULL;
    int progress = 0;
    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--rom") == 0 && a + 1 < argc) {
            rom = argv[++a];
        } else if (strcmp(argv[a], "--frames") == 0 && a + 1 < argc) {
            frames = (u32)atoi(argv[++a]);
        } else if (strcmp(argv[a], "--input") == 0 && a + 1 < argc) {
            input = argv[++a];
        } else if (strcmp(argv[a], "--lax") == 0) {
            gPortAssetStrict = 0;
        } else if (strcmp(argv[a], "--trace-assets") == 0) {
            gPortAssetTrace = 1;
        } else if (strcmp(argv[a], "--trace") == 0 && a + 1 < argc) {
            gPortTraceMask = (u32)strtoul(argv[++a], NULL, 0);
            gPortTracePrint = 1;
        } else if (strcmp(argv[a], "--qs-save") == 0 && a + 1 < argc) {
            qsAt = (u32)strtoul(argv[++a], NULL, 0);
        } else if (strcmp(argv[a], "--qs-load") == 0 && a + 1 < argc) {
            qsLoadAt = (u32)strtoul(argv[++a], NULL, 0);
        } else if (strcmp(argv[a], "--resume") == 0 && a + 1 < argc) {
            resume = argv[++a];
        } else if (strcmp(argv[a], "--drawdist") == 0 && a + 1 < argc) {
            setDrawDistance(atof(argv[++a]));
        } else if (strcmp(argv[a], "--audio-rate") == 0 && a + 1 < argc) {
            setAudioRate(atoi(argv[++a]));
        } else if (strcmp(argv[a], "--hrtf") == 0 && a + 1 < argc) {
            setAudioHrtf(atoi(argv[++a]));
        } else if (strcmp(argv[a], "--progress") == 0) {
            progress = 1;
        }
    }
    if (rom == NULL || !port_node_load_rom(rom)) {
        printf("usage: dk64_headless --rom <dk64-us.z64> [--frames N] [--input file] [--lax] [--trace mask] "
               "[--qs-save F --qs-load G] [--resume slot,kong,map,exit,hasPos,x,y,z,yaw] [--drawdist K] "
               "[--audio-rate 22050|44100] [--hrtf 0|1] [--progress]\n");
        return 2;
    }
    if (input != NULL) {
        loadInputFile(input);
    }
    portInit();
    if (resume != NULL) {
        int rs, rk, rm, re, rp, ryaw;
        float rx, ry, rz;
        if (sscanf(resume, "%d,%d,%i,%d,%d,%f,%f,%f,%d", &rs, &rk, &rm, &re, &rp, &rx, &ry, &rz, &ryaw) != 9) {
            portAbort("PORT: bad --resume %s", resume);
        }
        port_resume_request(rs, rk, rm, re, rp, rx, ry, rz, ryaw);
    }
    for (i = 0; i < frames; i++) {
        for (e = 0; e < sEventCount; e++) {
            if (sEvents[e].frame == i) {
                if (sEvents[e].press) {
                    sHeldButtons |= sEvents[e].button;
                } else {
                    sHeldButtons &= ~sEvents[e].button;
                }
            }
        }
        gPortHeadlessButtons = sHeldButtons;
        port_frame_step(PORT_VI_PERIOD_MS);
        if ((i + 1) % 60 == 0) {
            printf("frame %u %s\n", i + 1, port_debug_stats());
            fflush(stdout);
        }
        if (qsAt != 0 && i + 1 == qsAt) {
            if (!port_qs_save_begin()) {
                portAbort("PORT: qs save failed");
            }
            qsBuf = malloc(port_qs_size());
            port_qs_copy_out(qsBuf, port_qs_table());
            port_qs_save_end();
            printf("QS save at frame %u\n", i + 1);
            fflush(stdout);
        }
        if (qsLoadAt != 0 && i + 1 == qsLoadAt && !qsLoaded && qsBuf != NULL) {
            u8 *stash = port_qs_load_begin();
            if (stash == NULL) {
                portAbort("PORT: qs load failed");
            }
            port_qs_copy_in(qsBuf, port_qs_table());
            if (!port_qs_load_end(stash)) {
                portAbort("PORT: qs load failed");
            }
            qsLoaded = 1;
            printf("QS load at frame %u -> %u\n", i + 1, qsAt);
            fflush(stdout);
            i = qsAt - 1;
        }
    }
    if (progress) {
        printf("progress %s\n", port_level_progress());
        printf("summary %s\n", port_save_summary());
        fflush(stdout);
    }
    return 0;
}
#else
/* ---------------------------------------------------------------- browser */
#include <SDL2/SDL.h>
#include <string.h>

static double sLastMs;
static int sMenuPaused; /* Esc menu (web/src/game/menu/) open: the game is frozen */

int portMainQsRanges(QsRange *out) {
    out[0] = QS_RANGE(sAccumMs);
    out[1] = QS_RANGE(sLastMs);
    out[2] = QS_RANGE(gPortDrawDistance); /* launch config: a quickload keeps this run's value */
    out[3] = QS_RANGE(sMenuPaused);       /* UI state: F9 while the menu is open must not unfreeze or freeze */
    out[4] = QS_RANGE(gPortAudioHrtf);    /* setting: a quickload keeps it (the audio rate comes from the snapshot) */
    out[5] = QS_RANGE(gPortKongSwap);     /* setting: a quickload keeps it */
    out[6] = QS_RANGE(gPortStickCamera);  /* settings: a quickload keeps them */
    out[7] = QS_RANGE(gPortCameraSpeed);
    out[8] = QS_RANGE(gPortCameraInvert);
    return 9;
}

/* JS menu: 1 stops stepping (no input poll, audio drain or present; audio starves to silence), 0 resumes */
EMSCRIPTEN_KEEPALIVE void port_set_paused(s32 paused) {
    sMenuPaused = paused != 0;
    if (!sMenuPaused) {
        sLastMs = 0; /* the first frame after resume steps one VI period, not the paused time */
    }
}

/* web/src/game/settings.ts: applied between frames; each returns the value in effect */
EMSCRIPTEN_KEEPALIVE f32 port_set_drawdist(f32 k) { return setDrawDistance(k); }
EMSCRIPTEN_KEEPALIVE s32 port_set_scale(s32 scale) { return portGfxSetScale(scale); }
EMSCRIPTEN_KEEPALIVE s32 port_set_msaa(s32 msaa) { return portGfxSetMsaa(msaa); }
EMSCRIPTEN_KEEPALIVE s32 port_gfx_max_samples(void) { return portGfxMaxSamples(); }
EMSCRIPTEN_KEEPALIVE s32 port_set_audio_hrtf(s32 on) { return setAudioHrtf(on); }
EMSCRIPTEN_KEEPALIVE s32 port_audio_rate(void) { return gPortAudioRate; }
EMSCRIPTEN_KEEPALIVE s32 port_set_interp(s32 on) { return portGfxSetInterp(on); }
EMSCRIPTEN_KEEPALIVE s32 port_set_kong_swap(s32 on) {
    gPortKongSwap = on != 0;
    return gPortKongSwap;
}
EMSCRIPTEN_KEEPALIVE s32 port_set_stick_camera(s32 on) {
    gPortStickCamera = on != 0;
    return gPortStickCamera;
}
EMSCRIPTEN_KEEPALIVE s32 port_set_camera_speed(s32 v) { return setCameraSpeed(v); }
EMSCRIPTEN_KEEPALIVE s32 port_set_camera_invert(s32 v) { return setCameraInvert(v); }

/* the animation frame's time (its start, vsync-aligned; document.timeline holds it during the frame), not the
 * callback's clock: callbacks start at varying points of the frame, and that jitter alone made one frame step two
 * VIs and the next none. The emscripten main loop stays: it survives the fibers' asyncify unwinds, an html5
 * animation frame loop stops at the first one. */
EM_JS(double, port_frame_time, (void), {
    var t = typeof document !== 'undefined' && document.timeline ? document.timeline.currentTime : null;
    return typeof t === 'number' ? t : performance.now();
});

static void port_frame(void) {
    double now, dt;
    if (sMenuPaused) {
        return;
    }
    now = port_frame_time();
    dt = sLastMs == 0 ? PORT_VI_PERIOD_MS : now - sLastMs;
    sLastMs = now;
    port_frame_step(dt);
}

int main(int argc, char **argv) {
    int scale = 0, msaa = 4, a;
    for (a = 1; a + 1 < argc; a++) {
        if (strcmp(argv[a], "--scale") == 0) {
            scale = atoi(argv[++a]);
        } else if (strcmp(argv[a], "--msaa") == 0) {
            msaa = atoi(argv[++a]);
        } else if (strcmp(argv[a], "--drawdist") == 0) {
            setDrawDistance(atof(argv[++a]));
        } else if (strcmp(argv[a], "--audio-rate") == 0) {
            setAudioRate(atoi(argv[++a]));
        } else if (strcmp(argv[a], "--hrtf") == 0) {
            setAudioHrtf(atoi(argv[++a]));
        } else if (strcmp(argv[a], "--interp") == 0) {
            portGfxSetInterp(atoi(argv[++a]));
        } else if (strcmp(argv[a], "--kong-swap") == 0) {
            gPortKongSwap = atoi(argv[++a]) != 0;
        } else if (strcmp(argv[a], "--stick-camera") == 0) {
            gPortStickCamera = atoi(argv[++a]) != 0;
        } else if (strcmp(argv[a], "--camera-speed") == 0) {
            setCameraSpeed(atoi(argv[++a]));
        } else if (strcmp(argv[a], "--camera-invert") == 0) {
            setCameraInvert(atoi(argv[++a]));
        } else if (strcmp(argv[a], "--save-file") == 0) {
            portSaveSetPath(argv[++a]); /* before portInit, which reads it */
        }
    }
    if (portRomSize() == 0) {
        printf("PORT: Module.portRom not set\n");
        return 1;
    }
    portGfxSetScale(scale);
    portGfxSetMsaa(msaa);
    portInit();
    emscripten_set_main_loop(port_frame, 0, 0);
    return 0;
}
#endif
