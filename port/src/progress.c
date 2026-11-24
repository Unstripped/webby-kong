/* Current level's collectables as JSON for the web menu (web/src/game/menu/), and the save's headline numbers for
 * the dashboard (web/src/game/saves.ts upload); the game fills PortLevelProgress and PortSaveSummary. */
#include "port_internal.h"
#include <emscripten.h>
#include <stdio.h>

static int jnum(char *dst, size_t n, int v) {
    return v < 0 ? snprintf(dst, n, "null") : snprintf(dst, n, "%d", v);
}

/* JSON: {"ok":true,"world":..,"kong":..|null,"kongs":[{"gb":..,"bananas":..,"medal":..,"blueprint":..} x5],
 * "fairies":..,"fairiesTotal":..,"crowns":..,"crownsTotal":..,"keys":..,"keysTotal":..} (null: not in this
 * level) or {"ok":false,"reason":"..."} */
EMSCRIPTEN_KEEPALIVE const char *port_level_progress(void) {
    static char buf[1024];
    PortLevelProgress p;
    const char *reason = "";
    char *o = buf, *end = buf + sizeof buf;
    int k;
    if (!mapCaptureLevelProgress(&p, &reason)) {
        snprintf(buf, sizeof buf, "{\"ok\":false,\"reason\":\"%s\"}", reason);
        return buf;
    }
    o += snprintf(o, end - o, "{\"ok\":true,\"world\":%d,\"kong\":", (int)p.world);
    o += jnum(o, end - o, p.kong);
    o += snprintf(o, end - o, ",\"kongs\":[");
    for (k = 0; k < 5; k++) {
        o += snprintf(o, end - o, "%s{\"gb\":", k ? "," : "");
        o += jnum(o, end - o, p.kongs[k].goldenBananas);
        o += snprintf(o, end - o, ",\"bananas\":");
        o += jnum(o, end - o, p.kongs[k].bananas);
        o += snprintf(o, end - o, ",\"medal\":");
        o += jnum(o, end - o, p.kongs[k].medal);
        o += snprintf(o, end - o, ",\"blueprint\":");
        o += jnum(o, end - o, p.kongs[k].blueprint);
        o += snprintf(o, end - o, "}");
    }
    snprintf(o, end - o,
             "],\"fairies\":%d,\"fairiesTotal\":%d,\"crowns\":%d,\"crownsTotal\":%d,\"keys\":%d,\"keysTotal\":%d}",
             (int)p.fairies, (int)p.fairiesTotal, (int)p.crowns, (int)p.crownsTotal, (int)p.keys, (int)p.keysTotal);
    return buf;
}

/* JSON object of tools/saves.py SUMMARY_KEYS, or "" outside a single-player adventure (the upload then keeps the
 * save's previous summary) */
const char *port_save_summary(void) {
    static char buf[256];
    PortSaveSummary s;
    const char *reason = "";
    if (!mapCaptureSaveSummary(&s, &reason)) {
        buf[0] = 0;
        return buf;
    }
    snprintf(buf, sizeof buf,
             "{\"slot\":%d,\"percent\":%d,\"goldenBananas\":%d,\"keys\":%d,\"crowns\":%d,\"medals\":%d,"
             "\"fairies\":%d,\"blueprints\":%d,\"kongs\":%d,\"playSeconds\":%d}",
             (int)s.slot, (int)s.percent, (int)s.goldenBananas, (int)s.keys, (int)s.crowns, (int)s.medals,
             (int)s.fairies, (int)s.blueprints, (int)s.kongs, (int)s.playSeconds);
    return buf;
}
