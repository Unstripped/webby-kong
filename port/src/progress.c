/* Current level's collectables as JSON for the web menu (web/menu.js); the game fills PortLevelProgress. */
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
