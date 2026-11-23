/* EEPROM persistence: one file per save game, /saves/s<id>.eep (--save-file from web/main.js; /saves/dk64.eep without
 * the save server). IDBFS in the browser, backed up through web/saves.js with the save's summary; NODERAWFS-free
 * memory in headless. port_save_request/port_save_busy run the in-game save for web/menu.js "Exit to dashboard". */
#include "port_internal.h"
#include <emscripten.h>
#include <stdio.h>

extern u8 gPortEeprom[0x800];

static char sSavePath[64] = "/saves/dk64.eep";
static int sDirty;
static double sDirtySince;

/* part of the quicksave snapshot, like the game's EEPROM message queue they mirror */
s32 gPortSaveRequest;
s32 gPortEepromQueued;

void portSaveSetPath(const char *path) {
    snprintf(sSavePath, sizeof sSavePath, "%s", path);
}

int portSaveQsRanges(QsRange *out) {
    out[0] = QS_RANGE(sDirty);
    out[1] = QS_RANGE(sDirtySince);
    out[2] = QS_RANGE(sSavePath); /* launch config: a quickload keeps this page's save file */
    return 3;
}

#ifndef PORT_HEADLESS
EM_JS(void, port_save_sync, (const char *summary), {
    if (typeof FS !== 'undefined' && FS.syncfs) {
        FS.syncfs(false, function(err) {
            if (err) console.log('PORT: save sync failed: ' + err);
        });
        if (typeof window !== 'undefined' && window.dk64saves) { /* web/saves.js */
            window.dk64saves.upload(FS, UTF8ToString(summary));
        }
    }
});

EM_JS(void, port_save_install_visibility_hook, (void), {
    document.addEventListener('visibilitychange', function() {
        if (document.visibilityState === 'hidden') {
            Module.ccall('port_save_flush_now', null, [], []);
        }
    });
});
#endif

static void writeFile(void) {
#ifndef PORT_HEADLESS
    FILE *f = fopen(sSavePath, "wb");
    if (f == NULL) {
        portLog("PORT: cannot write %s", sSavePath);
        return;
    }
    fwrite(gPortEeprom, 1, sizeof gPortEeprom, f);
    fclose(f);
    port_save_sync(port_save_summary());
#endif
}

void portSaveInit(void) {
    memset(gPortEeprom, 0xFF, sizeof gPortEeprom);
#ifndef PORT_HEADLESS
    {
        FILE *f = fopen(sSavePath, "rb");
        if (f != NULL) {
            fread(gPortEeprom, 1, sizeof gPortEeprom, f);
            fclose(f);
        }
        port_save_install_visibility_hook();
    }
#endif
}

void portSaveWritten(void) {
    sDirty = 1;
    sDirtySince = emscripten_get_now();
}

void portSaveFlushIfDue(double nowMs) {
    if (sDirty && nowMs - sDirtySince >= 1000.0) {
        sDirty = 0;
        writeFile();
    }
}

EMSCRIPTEN_KEEPALIVE void port_save_flush_now(void) {
    if (sDirty) {
        sDirty = 0;
        writeFile();
    }
}

/* web/menu.js exit: 1 requests the in-game save (consumed by mainThreadLoop), 0 withdraws it */
EMSCRIPTEN_KEEPALIVE void port_save_request(s32 on) {
    gPortSaveRequest = on != 0;
}

/* 1 while the requested save or an EEPROM write is in flight; when idle, flushes the save file (and uploads) */
EMSCRIPTEN_KEEPALIVE s32 port_save_busy(void) {
    if (gPortSaveRequest || gPortEepromQueued > 0) {
        return 1;
    }
    port_save_flush_now();
    return 0;
}
