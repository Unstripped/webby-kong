/* RT64 texture packs: the page (web/src/game/texpacks/packs.ts) resolves the enabled packs into records
 * {hash, hash version, entry, flags, priority} and installs them with port_texpack_set; gfx_gl.c looks every texture
 * it decodes up here (cache misses only) and draws a READY entry's texture in its place. Entries load on first use:
 * gfxTexpackTexture asks the page for the image (Module.portTexpackRequest), which decodes it in a worker and uploads
 * it with port_texpack_upload_bitmap (PNG) or port_texpack_upload (DDS). The dump hands every newly drawn texture's
 * RGBA8 to the page (port_texdump_take), keyed by its version 5 hash. */
#include "gfx/gfx_texpack.h"
#include "gfx/gfx_rt64hash.h"
#include <emscripten.h>
#include <emscripten/html5_webgl.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM_EXT
#define GL_COMPRESSED_RGBA_BPTC_UNORM_EXT 0x8E8C
#endif

/* texture formats of port_texpack_upload, shared with web/src/game/texpacks/dds.ts */
enum { TEX_RGBA8, TEX_BC1, TEX_BC2, TEX_BC3, TEX_BC7 };

#define DUMP_RING 1024
#define DUMP_SEEN 65536

typedef struct {
    u64 hash;
    u32 entry;
    u16 priority; /* 0 = the top pack */
    u8 version;
    u8 used;
} PackSlot;

static struct {
    PackSlot *slots; /* open-addressed on (version, hash) */
    u32 cap;         /* power of two */
    GfxPackTex *entries;
    u32 count;
    u32 versions; /* bit v: some record has hash version v */
} sPack;

static struct {
    s32 on;
    u64 *seen; /* open-addressed set of dumped hashes, 0 empty (seenZero records hash 0) */
    u32 seenCount;
    u8 seenZero;
    u8 *ring[DUMP_RING]; /* records: u32 hashLo, hashHi, width, height, then RGBA8 */
    u32 head, count;
    u32 dropped;
} sDump;

int gfxTexpackQsRanges(QsRange *out) {
    out[0] = QS_RANGE(sPack);
    out[1] = QS_RANGE(sDump);
    return 2;
}

int gfxTexpackActive(void) {
    return sPack.count > 0;
}

static u32 slotStart(u64 hash, u32 version) {
    u64 k = hash ^ ((u64)version * 0x9E3779B97F4A7C15ull);
    return (u32)(k ^ (k >> 32)) & (sPack.cap - 1);
}

static PackSlot *slotFind(u64 hash, u32 version) {
    u32 i;
    for (i = slotStart(hash, version); sPack.slots[i].used; i = (i + 1) & (sPack.cap - 1)) {
        PackSlot *s = &sPack.slots[i];
        if (s->hash == hash && s->version == version) {
            return s;
        }
    }
    return NULL;
}

static void slotInsert(u64 hash, u32 version, u32 entry, u32 priority) {
    u32 i;
    for (i = slotStart(hash, version); sPack.slots[i].used; i = (i + 1) & (sPack.cap - 1)) {
        PackSlot *s = &sPack.slots[i];
        if (s->hash == hash && s->version == version) {
            if (priority < s->priority) {
                s->entry = entry;
                s->priority = (u16)priority;
            }
            return;
        }
    }
    sPack.slots[i].hash = hash;
    sPack.slots[i].version = (u8)version;
    sPack.slots[i].entry = entry;
    sPack.slots[i].priority = (u16)priority;
    sPack.slots[i].used = 1;
}

static int dumpSeen(u64 hash, int mark) {
    u32 i;
    if (hash == 0) {
        int was = sDump.seenZero;
        sDump.seenZero |= (u8)mark;
        return was;
    }
    for (i = (u32)(hash ^ (hash >> 32)) & (DUMP_SEEN - 1); sDump.seen[i] != 0; i = (i + 1) & (DUMP_SEEN - 1)) {
        if (sDump.seen[i] == hash) {
            return 1;
        }
    }
    if (mark) {
        sDump.seen[i] = hash;
        sDump.seenCount++;
    }
    return 0;
}

static void dumpPush(u64 hash, const GfxTexImage *img) {
    u32 hdr[4];
    u8 *p;
    size_t px = (size_t)img->width * img->height * 4;
    if (dumpSeen(hash, 0)) {
        return;
    }
    if (sDump.count == DUMP_RING || sDump.seenCount >= DUMP_SEEN / 4 * 3) {
        sDump.dropped++; /* not marked seen: dumped if drawn again once there is room */
        return;
    }
    p = malloc(sizeof hdr + px);
    if (p == NULL) {
        sDump.dropped++;
        return;
    }
    dumpSeen(hash, 1);
    hdr[0] = (u32)hash;
    hdr[1] = (u32)(hash >> 32);
    hdr[2] = img->width;
    hdr[3] = img->height;
    memcpy(p, hdr, sizeof hdr);
    memcpy(p + sizeof hdr, img->rgba, px);
    sDump.ring[(sDump.head + sDump.count) % DUMP_RING] = p;
    sDump.count++;
}

s32 gfxTexpackLookup(const GfxState *st, int tile, const GfxTexImage *img) {
    const GfxTile *g = &st->tiles[tile & 7];
    Rt64Tile t;
    u16 w, h;
    u32 tlut, v, bestPriority = 0x10000;
    u64 hash, hash5 = 0, raw = 0;
    int isRaw, have5 = 0;
    s32 best = -1;
    if (sPack.count == 0 && !sDump.on) {
        return -1;
    }
    t.fmt = g->fmt;
    t.siz = g->siz;
    t.palette = g->palette;
    t.cms = g->cms;
    t.cmt = g->cmt;
    t.masks = g->masks;
    t.maskt = g->maskt;
    t.line = g->line;
    t.tmem = g->tmem;
    t.uls = g->uls;
    t.ult = g->ult;
    t.lrs = g->lrs;
    t.lrt = g->lrt;
    tlut = st->otherH & (3u << G_MDSFT_TEXTLUT);
    if (!rt64SampleSize(&t, &w, &h)) {
        return -1;
    }
    isRaw = rt64RequiresRawTmem(&t, w, h, tlut);
    if (isRaw && sPack.count != 0) {
        raw = rt64RawTmemHash(gGfxTmem);
    }
    for (v = 1; v <= 5 && sPack.count != 0; v++) {
        PackSlot *s;
        if (!(sPack.versions & (1u << v))) {
            continue;
        }
        hash = isRaw ? raw : rt64TmemHash(gGfxTmem, &t, w, h, tlut, v);
        if (v == 5) {
            hash5 = hash;
            have5 = 1;
        }
        s = slotFind(hash, v);
        if (s != NULL && s->priority < bestPriority) {
            best = (s32)s->entry;
            bestPriority = s->priority;
        }
    }
    if (sDump.on && !isRaw && img->rgba != NULL && w == img->width && h == img->height) {
        if (!have5) {
            hash5 = rt64TmemHash(gGfxTmem, &t, w, h, tlut, 5);
        }
        dumpPush(hash5, img);
    }
    return best;
}

EM_JS(void, texpack_js_request, (u32 entry), {
    if (Module.portTexpackRequest) Module.portTexpackRequest(entry);
});

GfxPackTex *gfxTexpackTexture(s32 entry) {
    GfxPackTex *e;
    if (entry < 0 || (u32)entry >= sPack.count) {
        return NULL;
    }
    e = &sPack.entries[entry];
    if (e->state == TEXPACK_READY) {
        return e;
    }
    if (e->state == TEXPACK_UNLOADED) {
        e->state = TEXPACK_REQUESTED;
        texpack_js_request((u32)entry);
    }
    return NULL;
}

static void packClear(void) {
    u32 i;
    for (i = 0; i < sPack.count; i++) {
        if (sPack.entries[i].tex != 0) {
            glDeleteTextures(1, &sPack.entries[i].tex);
        }
    }
    free(sPack.entries);
    free(sPack.slots);
    memset(&sPack, 0, sizeof sPack);
}

static u32 le32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

EMSCRIPTEN_KEEPALIVE u8 *port_texpack_alloc(u32 bytes) {
    return malloc(bytes);
}

/* installs the pack set: entryCount textures (all UNLOADED) and recordCount 16-byte little-endian records
 * {u32 hashLo, u32 hashHi, u32 entry, u8 version 1..5, u8 flags (bit 0: shift half), u16 priority}. Takes ownership
 * of records. entryCount 0 turns packs off. 0 on allocation failure (no packs then). */
EMSCRIPTEN_KEEPALIVE s32 port_texpack_set(u32 entryCount, u8 *records, u32 recordCount) {
    u32 i, cap = 16;
    s32 ok = 1;
    packClear();
    if (entryCount != 0) {
        while (cap < 2 * recordCount) {
            cap <<= 1;
        }
        sPack.entries = calloc(entryCount, sizeof *sPack.entries);
        sPack.slots = calloc(cap, sizeof *sPack.slots);
        if (sPack.entries == NULL || sPack.slots == NULL) {
            free(sPack.entries);
            free(sPack.slots);
            memset(&sPack, 0, sizeof sPack);
            ok = 0;
        } else {
            sPack.cap = cap;
            sPack.count = entryCount;
            for (i = 0; i < recordCount; i++) {
                const u8 *r = records + i * 16;
                u64 hash = (u64)le32(r) | (u64)le32(r + 4) << 32;
                u32 entry = le32(r + 8), version = r[12], priority = (u32)r[14] | (u32)r[15] << 8;
                if (entry >= entryCount || version < 1 || version > 5) {
                    continue;
                }
                slotInsert(hash, version, entry, priority);
                sPack.entries[entry].shiftHalf = r[13] & 1;
                sPack.versions |= 1u << version;
            }
        }
    }
    free(records);
    gfxGlTexCacheFlush(); /* cached textures were looked up against the old set */
    gfxGlStateReset();
    return ok;
}

/* bit 0: RGBA8 (always); bit 1: BC1-BC3 (WEBGL_compressed_texture_s3tc); bit 2: BC7 (EXT_texture_compression_bptc) */
EMSCRIPTEN_KEEPALIVE s32 port_texpack_formats(void) {
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
    s32 f = 1;
    if (ctx != 0) {
        if (emscripten_webgl_enable_extension(ctx, "WEBGL_compressed_texture_s3tc")) {
            f |= 2;
        }
        if (emscripten_webgl_enable_extension(ctx, "EXT_texture_compression_bptc")) {
            f |= 4;
        }
    } else {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        if (ext != NULL && strstr(ext, "GL_WEBGL_compressed_texture_s3tc") != NULL) {
            f |= 2;
        }
        if (ext != NULL && strstr(ext, "GL_EXT_texture_compression_bptc") != NULL) {
            f |= 4;
        }
    }
    return f;
}

/* an entry the page may upload: asked for or not yet */
static GfxPackTex *uploadable(u32 entry) {
    GfxPackTex *e;
    if (entry >= sPack.count) {
        return NULL;
    }
    e = &sPack.entries[entry];
    return e->state == TEXPACK_UNLOADED || e->state == TEXPACK_REQUESTED ? e : NULL;
}

static void beginUpload(GfxPackTex *e) {
    while (glGetError() != GL_NO_ERROR) {
    }
    glGenTextures(1, &e->tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, e->tex);
}

static s32 endUpload(GfxPackTex *e, int ok) {
    if (ok && glGetError() == GL_NO_ERROR) {
        e->state = TEXPACK_READY;
        e->sampling = 0;
    } else {
        glDeleteTextures(1, &e->tex);
        e->tex = 0;
        e->state = TEXPACK_FAILED;
        ok = 0;
    }
    gfxGlStateReset(); /* the bind on unit 0 changed GL state behind gfx_gl.c's shadow */
    return ok;
}

EM_JS(int, texpack_js_upload_bitmap, (void), {
    var b = Module.portTexpackBitmap;
    var gl = typeof GLctx !== 'undefined' ? GLctx : GL.currentContext.GLctx;
    if (!b) return 0;
    try {
        gl.pixelStorei(0x9241, false); /* UNPACK_PREMULTIPLY_ALPHA_WEBGL */
        gl.pixelStorei(0x9243, 0); /* UNPACK_COLORSPACE_CONVERSION_WEBGL: NONE */
        gl.pixelStorei(0x9240, false); /* UNPACK_FLIP_Y_WEBGL */
        gl.texImage2D(0x0DE1, 0, 0x8058, 0x1908, 0x1401, b); /* TEXTURE_2D, RGBA8, RGBA, UNSIGNED_BYTE */
        gl.pixelStorei(0x9243, 0x9244); /* BROWSER_DEFAULT_WEBGL */
    } catch (e) {
        return 0;
    }
    return 1;
});

/* uploads Module.portTexpackBitmap (an ImageBitmap, not premultiplied) as entry's texture, with GL mips */
EMSCRIPTEN_KEEPALIVE s32 port_texpack_upload_bitmap(u32 entry, u32 w, u32 h) {
    GfxPackTex *e = uploadable(entry);
    int ok;
    if (e == NULL) {
        return 0;
    }
    beginUpload(e);
    ok = texpack_js_upload_bitmap();
    if (ok) {
        glGenerateMipmap(GL_TEXTURE_2D);
        e->mipmapped = 1;
        e->w = w;
        e->h = h;
    }
    return endUpload(e, ok);
}

static u32 levelBytes(u32 format, u32 w, u32 h) {
    if (format == TEX_RGBA8) {
        return w * h * 4;
    }
    return ((w + 3) / 4) * ((h + 3) / 4) * (format == TEX_BC1 ? 8 : 16);
}

/* uploads a DDS image: `levels` mips of format (TEX_*) back to back in data (malloc'd by port_texpack_alloc; freed
 * here) */
EMSCRIPTEN_KEEPALIVE s32 port_texpack_upload(u32 entry, u32 format, u32 width, u32 height, u32 levels, u8 *data,
                                             u32 bytes) {
    static const GLenum kInternal[] = { GL_RGBA8, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,
                                        GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_RGBA_BPTC_UNORM_EXT };
    GfxPackTex *e = uploadable(entry);
    u32 l, total = 0;
    const u8 *p = data;
    if (e == NULL) {
        free(data);
        return 0;
    }
    if (format > TEX_BC7 || width == 0 || height == 0 || levels == 0 || levels > 16) {
        free(data);
        e->state = TEXPACK_FAILED;
        return 0;
    }
    for (l = 0; l < levels; l++) {
        u32 lw = width >> l ? width >> l : 1, lh = height >> l ? height >> l : 1;
        total += levelBytes(format, lw, lh);
    }
    if (total > bytes) {
        free(data);
        e->state = TEXPACK_FAILED;
        return 0;
    }
    beginUpload(e);
    for (l = 0; l < levels; l++) {
        u32 lw = width >> l ? width >> l : 1, lh = height >> l ? height >> l : 1, n = levelBytes(format, lw, lh);
        if (format == TEX_RGBA8) {
            glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)lw, (GLsizei)lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
        } else {
            glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)l, kInternal[format], (GLsizei)lw, (GLsizei)lh, 0,
                                   (GLsizei)n, p);
        }
        p += n;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    if (format == TEX_RGBA8 && levels == 1) {
        glGenerateMipmap(GL_TEXTURE_2D);
        e->mipmapped = 1;
    } else {
        e->mipmapped = levels > 1;
    }
    e->w = width;
    e->h = height;
    free(data);
    return endUpload(e, 1);
}

/* the page could not load entry: never asked for again */
EMSCRIPTEN_KEEPALIVE void port_texpack_fail(u32 entry) {
    if (entry < sPack.count) {
        sPack.entries[entry].state = TEXPACK_FAILED;
    }
}

static void dumpFreeRing(void) {
    while (sDump.count > 0) {
        free(sDump.ring[sDump.head]);
        sDump.head = (sDump.head + 1) % DUMP_RING;
        sDump.count--;
    }
    sDump.head = 0;
}

/* on: start a dump (queued records dropped, every texture looked up again); off: stop, keeping queued records */
EMSCRIPTEN_KEEPALIVE void port_texdump_set(s32 on) {
    free(sDump.seen);
    sDump.seen = NULL;
    sDump.on = 0;
    if (on) {
        dumpFreeRing();
        sDump.dropped = 0;
        sDump.seenCount = 0;
        sDump.seenZero = 0;
        sDump.seen = calloc(DUMP_SEEN, sizeof *sDump.seen);
        sDump.on = sDump.seen != NULL;
        gfxGlTexCacheFlush(); /* textures cached before the dump miss again and get dumped */
        gfxGlStateReset();
    }
}

/* the oldest queued record, or NULL; the page frees it with port_texdump_free */
EMSCRIPTEN_KEEPALIVE u8 *port_texdump_take(void) {
    u8 *p;
    if (sDump.count == 0) {
        return NULL;
    }
    p = sDump.ring[sDump.head];
    sDump.head = (sDump.head + 1) % DUMP_RING;
    sDump.count--;
    return p;
}

EMSCRIPTEN_KEEPALIVE void port_texdump_free(u8 *p) {
    free(p);
}

/* textures skipped since the dump started: the queue or the seen set was full */
EMSCRIPTEN_KEEPALIVE u32 port_texdump_dropped(void) {
    return sDump.dropped;
}
