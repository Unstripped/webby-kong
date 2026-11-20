/* 3D sound, renderer: executes PORT_AOP_SPATIAL_END for the audio HLE (naudio.c). The voice's dry contribution
 * (the difference ENVMIXER made to the dry mix; the voice is mixed centre-panned) is turned back into its mono
 * signal and re-rendered for headphones: a per-ear delay from a spherical head model (interaural time difference)
 * followed by the ear's minimum-phase HRIR, bilinearly interpolated between measured directions (hrir_kemar.c).
 * Direction changes crossfade the filters and ramp the delays over one 184-sample ENVMIXER block. */
#include "port_internal.h"
#include "audio/hrtf.h"
#include <math.h>
#include <string.h>

#define N HRTF_FRAME
#define MAX_TAPS 48
#define MAX_DELAY 32 /* samples at 44.1 kHz: the head model's largest delay is 0.66 ms = 29 samples */
#define HIST (MAX_DELAY + 2)
#define PI_F 3.14159265f
#define HEAD_RADIUS 0.0875f /* m */
#define SOUND_SPEED 343.0f  /* m/s */

typedef struct {
    s32 rate;                        /* rate the state was built for; another rate resets it */
    f32 h[2][MAX_TAPS];              /* filters in use */
    f32 delay[2];                    /* per-ear delays in use, samples */
    f32 mono[HIST + N];              /* mono input: HIST samples of history, then this block */
    f32 ear[2][MAX_TAPS - 1 + N];    /* delayed input per ear: taps-1 samples of history, then this block */
} HrtfVoice;

static HrtfVoice sVoices[HRTF_VOICES];

/* the dry gains ENVMIXER applied at pan 0x40: eqpower[64] left, eqpower[63] right (Q15) */
#define CENTRE_GAIN_SUM ((0x59F2 + 0x5B11) / 32768.0f)

static void addRing(int ring, f32 az, f32 w, int taps, const s16 *table, f32 h[2][MAX_TAPS]) {
    const HrirRing *r = &gHrirRings[ring];
    int j = 0, e, k;
    f32 t = 0.0f;
    const s16 *a, *b;
    if (w == 0.0f) {
        return;
    }
    if (r->count > 1) {
        while (j < r->count - 2 && gHrirAzimuth[r->first + j + 1] <= az) {
            j++;
        }
        t = (az - gHrirAzimuth[r->first + j]) / (f32)(gHrirAzimuth[r->first + j + 1] - gHrirAzimuth[r->first + j]);
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    }
    a = table + (r->first + j) * 2 * taps;
    b = r->count > 1 ? a + 2 * taps : a;
    for (e = 0; e < 2; e++) {
        for (k = 0; k < taps; k++) {
            h[e][k] += w * ((1.0f - t) * a[e * taps + k] + t * b[e * taps + k]);
        }
    }
}

/* HRIR pair for a direction: azimuth 0..360 clockwise from the front, elevation -90..90 (the set ends at -40) */
static void lookup(f32 az, f32 el, int taps, f32 h[2][MAX_TAPS]) {
    const s16 *table = taps == 48 ? &gHrir44[0][0][0] : &gHrir22[0][0][0];
    f32 scale = 1.0f / (f32)(1 << (taps == 48 ? gHrir44Q : gHrir22Q)), f, tmp;
    int mirror = az > 180.0f, r0, r1, k;
    if (mirror) {
        az = 360.0f - az;
    }
    el = el < -40.0f ? -40.0f : el > 90.0f ? 90.0f : el;
    f = (el + 40.0f) / 10.0f;
    r0 = (int)f;
    if (r0 >= HRIR_RINGS - 1) {
        r0 = HRIR_RINGS - 1;
    }
    r1 = r0 + 1 < HRIR_RINGS ? r0 + 1 : r0;
    f -= r0;
    memset(h, 0, sizeof(f32) * 2 * MAX_TAPS);
    addRing(r0, az, r1 == r0 ? 1.0f : 1.0f - f, taps, table, h);
    if (r1 != r0) {
        addRing(r1, az, f, taps, table, h);
    }
    for (k = 0; k < taps; k++) {
        h[0][k] *= scale;
        h[1][k] *= scale;
        if (mirror) {
            tmp = h[0][k];
            h[0][k] = h[1][k];
            h[1][k] = tmp;
        }
    }
}

/* Woodworth's spherical head: the far ear hears the source (a/c)(theta + sin theta) later, theta the angle from
 * the median plane */
static void earDelays(f32 az, f32 el, s32 rate, f32 d[2]) {
    f32 s = cosf(el * (PI_F / 180.0f)) * sinf(az * (PI_F / 180.0f)); /* + = right */
    f32 lat = asinf(fabsf(s) > 1.0f ? 1.0f : fabsf(s));
    f32 itd = HEAD_RADIUS / SOUND_SPEED * (lat + sinf(lat)) * (f32)rate;
    if (itd > MAX_DELAY) {
        itd = MAX_DELAY;
    }
    d[0] = s > 0.0f ? itd : 0.0f;
    d[1] = s > 0.0f ? 0.0f : itd;
}

static inline s16 sat16(f32 v) {
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (s16)lrintf(v);
}

void portHrtfMix(u32 cmd, const s16 *savedL, const s16 *savedR, s16 *dryL, s16 *dryR) {
    HrtfVoice *v = &sVoices[SPATIAL_SLOT(cmd)];
    s32 rate = gPortAudioRate;
    int taps = rate == 44100 ? 48 : 24;
    f32 az = SPATIAL_AZ(cmd) * (360.0f / 1024.0f);
    f32 el = SPATIAL_EL(cmd) * (180.0f / 255.0f) - 90.0f;
    f32 clarity = SPATIAL_CLARITY(cmd) / 127.0f;
    f32 h1[2][MAX_TAPS], d1[2], delta[2][N], out[2][N];
    s16 *dry[2] = { dryL, dryR };
    const s16 *saved[2] = { savedL, savedR };
    int changed, e, n, k;

    lookup(az, el, taps, h1);
    earDelays(az, el, rate, d1);
    if ((cmd & SPATIAL_RESET) || v->rate != rate) {
        memset(v, 0, sizeof *v);
        v->rate = rate;
        memcpy(v->h, h1, sizeof h1);
        v->delay[0] = d1[0];
        v->delay[1] = d1[1];
    }
    changed = memcmp(v->h, h1, sizeof h1) != 0;

    for (n = 0; n < N; n++) {
        delta[0][n] = (f32)(dryL[n] - savedL[n]);
        delta[1][n] = (f32)(dryR[n] - savedR[n]);
        v->mono[HIST + n] = (delta[0][n] + delta[1][n]) * (1.0f / CENTRE_GAIN_SUM);
    }

    for (e = 0; e < 2; e++) {
        f32 *x = v->ear[e] + taps - 1;
        f32 d0 = v->delay[e], dd = (d1[e] - d0) / N;
        for (n = 0; n < N; n++) {
            f32 pos = (f32)(HIST + n) - (d0 + dd * (n + 1));
            int i = (int)pos;
            f32 fr = pos - (f32)i;
            x[n] = fr > 0.0f ? v->mono[i] + fr * (v->mono[i + 1] - v->mono[i]) : v->mono[i];
        }
        for (n = 0; n < N; n++) {
            f32 acc0 = 0.0f, acc1 = 0.0f, y;
            const f32 *xn = x + n;
            for (k = 0; k < taps; k++) {
                acc0 += v->h[e][k] * xn[-k];
            }
            if (changed) {
                for (k = 0; k < taps; k++) {
                    acc1 += h1[e][k] * xn[-k];
                }
                y = acc0 + (acc1 - acc0) * ((f32)(n + 1) / N);
            } else {
                y = acc0;
            }
            out[e][n] = clarity * y + (1.0f - clarity) * delta[e][n];
        }
        memmove(v->ear[e], v->ear[e] + N, sizeof(f32) * (taps - 1));
    }
    memmove(v->mono, v->mono + N, sizeof(f32) * HIST);
    memcpy(v->h, h1, sizeof h1);
    v->delay[0] = d1[0];
    v->delay[1] = d1[1];

    for (e = 0; e < 2; e++) {
        for (n = 0; n < N; n++) {
            dry[e][n] = sat16((f32)saved[e][n] + out[e][n]);
        }
    }
}
