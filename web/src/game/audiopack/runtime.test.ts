import { describe, expect, it } from 'vitest';
import type { ManifestEntry } from './manifest';
import { AudioPackRuntime, type DecodedSong, type SfxSrc } from './runtime';

const FRAMES = 64;
const RATE = 1000;
const STATE = 0; // i32 index of PackSeq[0] (16 bytes each)
const BUF = 64; // i16 index of the mix buffer
const PEAK = 25000; // level 1, gain 1: the game's 25000/32767 sequence scale

const entry = (e: Partial<ManifestEntry> = {}): ManifestEntry => ({
  file: 'x.ogg',
  loop: undefined,
  loopStart: 0,
  loopEnd: undefined,
  gain: 1,
  ...e,
});

interface Mem {
  i16: Int16Array;
  i32: Int32Array;
  f32: Float32Array;
}

function memory(): Mem {
  const buf = new ArrayBuffer(4096);
  return { i16: new Int16Array(buf), i32: new Int32Array(buf), f32: new Float32Array(buf) };
}

// the decode resolves at once; awaiting `ready` after the first mix() lets the runtime's own then() run first
function songRuntime(samples: Float32Array, e: Partial<ManifestEntry> = {}) {
  const ready = Promise.resolve<DecodedSong>({ left: samples, right: samples });
  const rt = new AudioPackRuntime({
    songs: new Map([[42, { entry: entry(e), bytes: new Uint8Array(1) }]]),
    sfx: [],
    fingerprint: 'f',
    decode: () => ready,
  });
  return { rt, ready };
}

function run(rt: AudioPackRuntime, m: Mem, frames = FRAMES): Int16Array {
  m.i16.fill(0, BUF, BUF + frames * 2);
  rt.mix(m.i16, m.i32, m.f32, BUF, frames, RATE, STATE);
  return m.i16.slice(BUF, BUF + frames * 2);
}

function play(m: Mem, gen: number, state = 1) {
  m.i32[STATE] = 42;
  m.i32[STATE + 1] = gen;
  m.i32[STATE + 2] = state;
  m.f32[STATE + 3] = 1;
}

describe('AudioPackRuntime.mix', () => {
  it('plays only while the sequence plays, and restarts on a new start', async () => {
    const impulse = new Float32Array(10000);
    impulse[0] = 1;
    const { rt, ready } = songRuntime(impulse, { loop: false });
    const m = memory();
    play(m, 1, 0);
    expect(run(rt, m).some((v) => v !== 0)).toBe(false);
    play(m, 1);
    run(rt, m); // requests the decode
    await ready;
    expect(run(rt, m)[0]).toBe(PEAK);
    expect(run(rt, m)[0]).toBe(0); // the same start keeps going
    play(m, 2);
    expect(run(rt, m)[0]).toBe(PEAK); // a restart begins at 0
  });

  it('fades out within 256 frames once the sequence stops', async () => {
    const { rt, ready } = songRuntime(new Float32Array(10000).fill(0.5));
    const m = memory();
    play(m, 1);
    run(rt, m);
    await ready;
    expect(run(rt, m)[0]).toBe(PEAK / 2);
    play(m, 1, 0);
    const out = run(rt, m, 512);
    expect(out[0]).not.toBe(0);
    expect(out.slice(256 * 2).every((v) => v === 0)).toBe(true);
  });

  it('wraps from loopEnd to loopStart', async () => {
    const ramp = Float32Array.from({ length: 30 }, (_, n) => (n + 1) / 1000);
    const { rt, ready } = songRuntime(ramp, { loopStart: 0.01, loopEnd: 0.02 }); // frames 10..20 at RATE
    const m = memory();
    play(m, 1);
    run(rt, m);
    await ready;
    const out = run(rt, m, 25);
    expect(out[19 * 2]).toBe(20 * 25);
    expect(out[20 * 2]).toBe(11 * 25);
    expect(out[24 * 2 + 1]).toBe(15 * 25);
  });
});

describe('AudioPackRuntime.bakeSfx', () => {
  const sfx = (id: number, loop: boolean | undefined): SfxSrc => ({
    id,
    entry: entry({ loop }),
    pcm: new Float32Array(4800).fill(0.25),
  });
  const rt = new AudioPackRuntime({
    songs: new Map(),
    sfx: [sfx(3, undefined), sfx(1, true), sfx(2, false)],
    fingerprint: 'f',
  });
  const len = Math.floor((4800 * 22050) / 48000);

  it('lists sounds by id and bakes them 8-aligned with the loop rules', () => {
    expect([0, 1, 2].map((i) => rt.sfxId(i))).toEqual([1, 2, 3]);
    const out = new Uint32Array(5);
    const bake = (i: number, origLoop: number) => {
      expect(rt.bakeSfx(i, 22050, origLoop, out, 0)).toBe(true);
      return [...out];
    };
    const forever = bake(0, 0); // loop: true
    expect(forever).toEqual([0, len * 2, 0, len, 0xffffffff]);
    const never = bake(1, 5); // loop: false
    expect(never).toEqual([(len * 2 + 7) & ~7, len * 2, 0, 0, 0]);
    expect(never[0]! % 8).toBe(0);
    const asOriginal = bake(2, 5); // loop absent: the original's count
    expect(asOriginal.slice(2)).toEqual([0, len, 5]);
    expect(bake(2, 0).slice(2)).toEqual([0, 0, 0]);
    const u8 = new Uint8Array(8);
    rt.read(u8, 0, forever[0]!, 4);
    expect(new Int16Array(u8.buffer, 0, 2)[0]).toBe(Math.round(0.25 * 32767));
  });
});
