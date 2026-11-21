#!/usr/bin/env python3
"""Generate port/src/audio/hrir_kemar.c: the head-related impulse responses the 3D sound renderer
(port/src/audio/hrtf.c) filters positional sound effects with.

Source: the MIT Media Lab KEMAR "compact" set (Bill Gardner and Keith Martin, 1994), 368 measured directions
(elevation -40..90 degrees in 10 degree rings, azimuth 0..180 to the right; the left side is the mirror image),
128 taps at 44.1 kHz, channel 0 = left ear. The data "is provided free with no restrictions on use, provided the
authors are cited": https://sound.media.mit.edu/resources/KEMAR.html. Download and unpack it once:

    curl -LO https://sound.media.mit.edu/resources/KEMAR/compact.zip && unzip compact.zip -d kemar
    python3 tools/gen_hrir.py kemar          # needs numpy; the build itself only uses the generated C file

Processing, per direction and ear:
1. diffuse-field equalisation: divide by the 1/3-octave smoothed RMS spectrum of all 736 responses, so the average
   over directions is flat and a sound keeps its timbre; only the direction-dependent part remains;
2. below 400 Hz the magnitude is held at its 400 Hz value (the compact set's low end is the measurement speaker's
   roll-off, and a head barely filters there); above 15 kHz it is held and tapered;
3. minimum phase (real cepstrum). The interaural time difference is removed by this and re-applied at run time
   from a spherical head model, so neighbouring directions interpolate without comb filtering;
4. truncated to 24 taps at 22.05 kHz / 48 taps at 44.1 kHz (>98.5% of every response's energy) with a half-Hann
   tail, and scaled so the mean energy per ear is 0.5: on average as loud as the game's centre pan.
"""

import argparse
import re
from pathlib import Path

import numpy as np

NFFT = 1024
FS = 44100
RATES = ((22050, 24), (44100, 48))
LOW_HOLD = 400.0
HIGH_HOLD = 15000.0
EAR_ENERGY = 0.5
OUT = Path(__file__).resolve().parent.parent / "port/src/audio/hrir_kemar.c"


def load(root: Path):
    rings = []
    for elev in range(-40, 100, 10):
        files = {}
        for f in (root / f"elev{elev}").iterdir():
            m = re.fullmatch(r"H-?\d+e(\d{3})a\.wav", f.name)
            if m:
                files[int(m.group(1))] = f
        azs = sorted(files)
        irs = []
        for az in azs:
            raw = files[az].read_bytes()
            # canonical 44-byte RIFF header, 16-bit stereo
            data = np.frombuffer(raw[44:44 + 128 * 4], "<i2").reshape(-1, 2).astype(np.float64) / 32768.0
            irs.append(data.T)
        rings.append((elev, azs, np.array(irs)))
    return rings


def smooth_third_octave(p, freqs):
    out = np.empty_like(p)
    for i, f in enumerate(freqs):
        sel = (freqs >= f / 2 ** (1 / 6)) & (freqs <= f * 2 ** (1 / 6))
        out[i] = p[sel].mean() if sel.any() else p[i]
    return out


def minimum_phase(mag, n):
    cep = np.fft.irfft(np.log(np.maximum(mag, 1e-6)), n)
    fold = np.zeros(n)
    fold[0] = 1.0
    fold[1:n // 2] = 2.0
    fold[n // 2] = 1.0
    return np.fft.irfft(np.exp(np.fft.rfft(cep * fold)), n)


def design(rings):
    freqs = np.fft.rfftfreq(NFFT, 1.0 / FS)
    allirs = np.concatenate([r[2] for r in rings])  # dirs, 2, 128
    power = (np.abs(np.fft.rfft(allirs, NFFT)) ** 2).reshape(-1, freqs.size).mean(0)
    eq_db = -10 * np.log10(smooth_third_octave(power, freqs))
    ref = eq_db[np.argmin(np.abs(freqs - 1000.0))]
    eq = 10 ** (np.clip(eq_db, ref - 20.0, ref + 10.0) / 20)
    lo = np.argmin(np.abs(freqs - LOW_HOLD))
    hi = np.argmin(np.abs(freqs - HIGH_HOLD))
    taper = 1.0 - np.clip((freqs - HIGH_HOLD) / (FS / 2 - HIGH_HOLD), 0, 1) * (1.0 - 10 ** (-12 / 20))

    mags = np.abs(np.fft.rfft(allirs, NFFT)) * eq
    mags[..., :lo] = mags[..., lo:lo + 1]
    mags[..., hi:] = mags[..., hi:hi + 1] * taper[hi:]

    tables = {}
    for rate, taps in RATES:
        n = NFFT * rate // FS
        h = np.array([[minimum_phase(m[: n // 2 + 1], n)[:taps] for m in pair] for pair in mags])
        fade = np.ones(taps)
        tail = taps // 4
        fade[-tail:] = 0.5 * (1 + np.cos(np.pi * (np.arange(tail) + 1) / (tail + 1)))
        h *= fade
        h *= np.sqrt(EAR_ENERGY / np.mean(np.sum(h ** 2, axis=-1)))
        tables[rate] = h
    return tables


def c_table(name, h):
    q = 14
    while np.max(np.abs(h)) * (1 << q) >= 32767:
        q -= 1
    v = np.round(h * (1 << q)).astype(int)
    lines = [f"const s16 {name}[HRIR_DIRS][2][{h.shape[2]}] = {{"]
    for pair in v:
        lines.append("    { " + ", ".join("{ " + ", ".join(str(x) for x in ear) + " }" for ear in pair) + " },")
    lines.append("};")
    return q, "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("kemar", type=Path, help="unpacked compact.zip (contains elev-40 .. elev90)")
    ap.add_argument("--out", type=Path, default=OUT)
    args = ap.parse_args()

    rings = load(args.kemar)
    tables = design(rings)
    dirs = sum(len(r[1]) for r in rings)
    out = [
        "/* Generated by tools/gen_hrir.py from the MIT Media Lab KEMAR compact HRTF set. Do not edit.",
        " * HRTF measurements of a KEMAR dummy-head microphone: Copyright 1994 MIT Media Laboratory, provided free with",
        " * no restrictions on use provided the authors are cited: Bill Gardner and Keith Martin,",
        " * https://sound.media.mit.edu/resources/KEMAR.html */",
        '#include "audio/hrtf.h"',
        "",
        f"const HrirRing gHrirRings[HRIR_RINGS] = {{",
    ]
    first = 0
    for elev, azs, _ in rings:
        out.append(f"    {{ {elev}, {len(azs)}, {first} }},")
        first += len(azs)
    out.append("};")
    out.append("")
    out.append("const u8 gHrirAzimuth[HRIR_DIRS] = {")
    flat = [a for _, azs, _ in rings for a in azs]
    for i in range(0, len(flat), 24):
        out.append("    " + ", ".join(str(a) for a in flat[i:i + 24]) + ",")
    out.append("};")
    for rate, _ in RATES:
        q, text = c_table(f"gHrir{rate // 1000}", tables[rate])
        out.append("")
        out.append(f"const s32 gHrir{rate // 1000}Q = {q};")
        out.append(text)
    out.append("")
    if dirs != 368:
        raise SystemExit(f"expected 368 directions, found {dirs}")
    args.out.write_text("\n".join(out))
    print(f"wrote {args.out}: {dirs} directions, " + ", ".join(f"{r} Hz x {t} taps" for r, t in RATES))


if __name__ == "__main__":
    main()
