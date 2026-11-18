/* Host entry and main loop: VI ticks, fiber rounds, RSP tasks, timers, present. */
#include "port_internal.h"
#include <emscripten.h>

void portMemCheck(void);
void portBootStart(void);
void portAudioOutDrain(double ms);

static double sAccumMs;

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
    while (n-- > 0) {
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
    int press;
    u16 button;
} InputEvent;

static InputEvent sEvents[256];
static int sEventCount;
static u16 sHeldButtons;
/* gPortHeadlessButtons lives in input.c */

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
        }
    }
    if (rom == NULL || !port_node_load_rom(rom)) {
        printf("usage: dk64_headless --rom <dk64-us.z64> [--frames N] [--input file] [--lax]\n");
        return 2;
    }
    if (input != NULL) {
        loadInputFile(input);
    }
    portInit();
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
    }
    return 0;
}
#else
/* ---------------------------------------------------------------- browser */
#include <SDL2/SDL.h>

static double sLastMs;

static void port_frame(void) {
    double now = emscripten_get_now();
    double dt = sLastMs == 0 ? PORT_VI_PERIOD_MS : now - sLastMs;
    sLastMs = now;
    port_frame_step(dt);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (portRomSize() == 0) {
        printf("PORT: Module.portRom not set\n");
        return 1;
    }
    portInit();
    emscripten_set_main_loop(port_frame, 0, 0);
    return 0;
}
#endif
