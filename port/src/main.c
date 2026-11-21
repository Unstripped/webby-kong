/* Host entry and main loop: VI ticks, fiber rounds, RSP tasks, timers, present. */
#include "port_internal.h"
#include <emscripten.h>

void portMemCheck(void);
void portBootStart(void);
void portAudioOutDrain(double ms);

static double sAccumMs;
f32 gPortDrawDistance = 4.0f;

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

static void runAll(void) {
    portRunUntilIdle();
    while (portSpRunPendingTasks()) {
        portRunUntilIdle();
    }
}

static void viTick(void) {
    u64 end = portTimeNow() + PORT_CYCLES_PER_VI;
    gPortStatVi++;
    portAudioPeakTick();
    portAiNetViTick();
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

void port_frame_step(double elapsedMs) {
    int n;
    portInputPoll();
    portAudioOutDrain(elapsedMs);
    sAccumMs += elapsedMs;
    n = (int)(sAccumMs / PORT_VI_PERIOD_MS);
    if (n > 4) {
        n = 4;
        sAccumMs = 0;
    } else {
        sAccumMs -= n * PORT_VI_PERIOD_MS;
    }
    /* netplay: behind the host's bundles (after a stall) catches up two ticks per frame */
    if (portNetActive() && portNetReadsAhead() >= portNetDelay() + 3) {
        n += 2;
        if (n > 8) {
            n = 8;
        }
    }
    while (n-- > 0) {
        if (!portNetTickAllowed()) {
            sAccumMs = 0; /* waiting for the network: no time debt */
            break;
        }
        viTick();
    }
    if (gPortViSwapped) {
        gPortViSwapped = 0;
        gPortStatFrames++;
        portGfxPresent(gPortViCurrentFb, gPortViBlack);
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
    int kind; /* 0 release, 1 press, 2 stick */
    int pad;  /* 0..PORT_MAX_PLAYERS-1 */
    u16 button;
    s8 x, y;
} InputEvent;

static InputEvent sEvents[256];
static int sEventCount;
static OSContPad sScriptPads[PORT_MAX_PLAYERS]; /* scripted pads; pad 0 drives gPortHeadlessButtons/Stick */

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
        char verb[16], arg[16];
        int x, y, pad = 0, n;
        InputEvent *ev = &sEvents[sEventCount];
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (sEventCount >= 256 || sscanf(line, "frame %u %15s %n", &frame, verb, &n) != 2) {
            continue;
        }
        ev->frame = frame;
        if (strcmp(verb, "stick") == 0) {
            if (sscanf(line + n, "%d %d pad %d", &x, &y, &pad) < 2) {
                portAbort("PORT: bad input line %s", line);
            }
            ev->kind = 2;
            ev->x = (s8)(x > 80 ? 80 : x < -80 ? -80 : x);
            ev->y = (s8)(y > 80 ? 80 : y < -80 ? -80 : y);
        } else {
            if (sscanf(line + n, "%15s pad %d", arg, &pad) < 1) {
                portAbort("PORT: bad input line %s", line);
            }
            ev->kind = strcmp(verb, "press") == 0;
            ev->button = buttonByName(arg);
        }
        if (pad < 0 || pad >= PORT_MAX_PLAYERS) {
            portAbort("PORT: bad pad %d (0..%d)", pad, PORT_MAX_PLAYERS - 1);
        }
        ev->pad = pad;
        sEventCount++;
    }
    fclose(f);
}

static u32 packScriptPad(int p) {
    return ((u32)sScriptPads[p].button << 16) | ((u32)(u8)sScriptPads[p].stick_x << 8) | (u8)sScriptPads[p].stick_y;
}

/* --net-sim N: the ring gets the next reads from the scripted pads; slots 1..N-1 join at frame --net-join */
static void netSimFeed(int players, u32 joinFrame, u32 frame) {
    u8 mask = frame >= joinFrame ? (u8)((1u << players) - 1) : 1;
    int k;
    for (k = portNetReadsAhead(); k < 3; k++) {
        port_net_push(gPortNetRead + (u32)k, mask, 0xFF, 0xFF, packScriptPad(0), packScriptPad(1), packScriptPad(2),
                      packScriptPad(3), packScriptPad(4));
    }
}

int main(int argc, char **argv) {
    const char *rom = NULL, *input = NULL;
    u32 frames = 600, i;
    int a, e, netSim = 0;
    u32 netJoin = 0;
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
        } else if (strcmp(argv[a], "--net-sim") == 0 && a + 1 < argc) {
            netSim = atoi(argv[++a]);
            if (netSim < 1 || netSim > PORT_MAX_PLAYERS) {
                portAbort("PORT: --net-sim %d (1..%d)", netSim, PORT_MAX_PLAYERS);
            }
        } else if (strcmp(argv[a], "--net-join") == 0 && a + 1 < argc) {
            netJoin = (u32)strtoul(argv[++a], NULL, 0);
        } else if (strcmp(argv[a], "--hash-every") == 0 && a + 1 < argc) {
            gPortNetHashEvery = atoi(argv[++a]);
        }
    }
    if (rom == NULL || !port_node_load_rom(rom)) {
        printf("usage: dk64_headless --rom <dk64-us.z64> [--frames N] [--input file] [--lax] [--trace mask] "
               "[--qs-save F --qs-load G] [--resume slot,kong,map,exit,hasPos,x,y,z,yaw] [--drawdist K] "
               "[--audio-rate 22050|44100] [--hrtf 0|1] [--progress] [--net-sim N [--net-join F] [--hash-every K]]\n");
        return 2;
    }
    if (input != NULL) {
        loadInputFile(input);
    }
    portInit();
    if (netSim != 0) {
        if (gPortNetHashEvery <= 0) {
            gPortNetHashEvery = 30;
        }
        port_net_begin(1, 0, 2);
    }
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
            const InputEvent *ev = &sEvents[e];
            OSContPad *p = &sScriptPads[ev->pad];
            if (ev->frame != i) {
                continue;
            }
            if (ev->kind == 2) {
                p->stick_x = ev->x;
                p->stick_y = ev->y;
            } else if (ev->kind == 1) {
                p->button |= ev->button;
            } else {
                p->button &= ~ev->button;
            }
        }
        gPortHeadlessButtons = sScriptPads[0].button;
        gPortHeadlessStickX = sScriptPads[0].stick_x;
        gPortHeadlessStickY = sScriptPads[0].stick_y;
        if (netSim != 0) {
            netSimFeed(netSim, netJoin, i);
        }
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
        fflush(stdout);
    }
    return 0;
}
#else
/* ---------------------------------------------------------------- browser */
#include <SDL2/SDL.h>
#include <string.h>

static double sLastMs;
static int sMenuPaused; /* web/menu.js open: the game is frozen */

int portMainQsRanges(QsRange *out) {
    out[0] = QS_RANGE(sAccumMs);
    out[1] = QS_RANGE(sLastMs);
    out[2] = QS_RANGE(gPortDrawDistance); /* launch config: a quickload keeps this run's value */
    out[3] = QS_RANGE(sMenuPaused);       /* UI state: F9 while the menu is open must not unfreeze or freeze */
    out[4] = QS_RANGE(gPortAudioHrtf);    /* setting: a quickload keeps it (the audio rate comes from the snapshot) */
    return 5;
}

/* JS menu: 1 stops stepping (no input poll, audio drain or present; audio starves to silence), 0 resumes.
 * A netplay session never pauses: the other players keep playing (web/menu.js mutes the local pad instead). */
EMSCRIPTEN_KEEPALIVE void port_set_paused(s32 paused) {
    if (portNetActive()) {
        return;
    }
    sMenuPaused = paused != 0;
    if (!sMenuPaused) {
        sLastMs = 0; /* the first frame after resume steps one VI period, not the paused time */
    }
}

/* web/settings.js: applied between frames; each returns the value in effect */
EMSCRIPTEN_KEEPALIVE f32 port_set_drawdist(f32 k) { return setDrawDistance(k); }
EMSCRIPTEN_KEEPALIVE f32 port_get_drawdist(void) { return gPortDrawDistance; }
EMSCRIPTEN_KEEPALIVE s32 port_set_scale(s32 scale) { return portGfxSetScale(scale); }
EMSCRIPTEN_KEEPALIVE s32 port_set_msaa(s32 msaa) { return portGfxSetMsaa(msaa); }
EMSCRIPTEN_KEEPALIVE s32 port_gfx_max_samples(void) { return portGfxMaxSamples(); }
EMSCRIPTEN_KEEPALIVE s32 port_set_audio_hrtf(s32 on) { return setAudioHrtf(on); }
EMSCRIPTEN_KEEPALIVE s32 port_audio_rate(void) { return gPortAudioRate; }

static void port_frame(void) {
    double now, dt;
    if (portNetActive()) {
        sMenuPaused = 0; /* a menu opened before the session must not freeze the game after it */
    } else if (sMenuPaused) {
        return;
    }
    now = emscripten_get_now();
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
