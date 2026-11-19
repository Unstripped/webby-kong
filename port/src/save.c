/* EEPROM persistence: /saves/dk64.eep (IDBFS in the browser, backed up through web/saves.js; NODERAWFS-free memory
 * in headless). */
#include "port_internal.h"
#include <emscripten.h>

extern u8 gPortEeprom[0x800];

#define SAVE_PATH "/saves/dk64.eep"

static int sDirty;
static double sDirtySince;

int portSaveQsRanges(QsRange *out) {
    out[0] = QS_RANGE(sDirty);
    out[1] = QS_RANGE(sDirtySince);
    return 2;
}

#ifndef PORT_HEADLESS
EM_JS(void, port_save_sync, (void), {
    if (typeof FS !== 'undefined' && FS.syncfs) {
        FS.syncfs(false, function(err) {
            if (err) console.log('PORT: save sync failed: ' + err);
        });
        if (typeof window !== 'undefined' && window.dk64saves) window.dk64saves.upload(FS); /* web/saves.js */
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
    FILE *f = fopen(SAVE_PATH, "wb");
    if (f == NULL) {
        portLog("PORT: cannot write %s", SAVE_PATH);
        return;
    }
    fwrite(gPortEeprom, 1, sizeof gPortEeprom, f);
    fclose(f);
    port_save_sync();
#endif
}

void portSaveInit(void) {
    memset(gPortEeprom, 0xFF, sizeof gPortEeprom);
#ifndef PORT_HEADLESS
    {
        FILE *f = fopen(SAVE_PATH, "rb");
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
