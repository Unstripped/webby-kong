// Module.portAudioPack: what port/src/audio/pack.c asks the page for while the game runs.
// - Sound effects: bakeSfx() converts a replaced sound to s16 at the rate the game's synthesizer will play it and
//   appends it to the PCM pool, which the game reads like ROM (read()) at offsets >= PORT_PACK_PCM_BASE.
// - Songs: mix() adds the replacement of each sequence player's song into the final s16 stereo buffer, following the
//   players' state (PackSeq in pack.c: id, start generation, state, level).
import { toast } from '../ui/toast';
import { hexId, type ManifestEntry } from './manifest';
import { resample } from './resample';

export interface SongSrc {
  entry: ManifestEntry;
  bytes: Uint8Array;
}

export interface SfxSrc {
  id: number;
  entry: ManifestEntry;
  pcm: Float32Array; // mono, 48 kHz
}

export interface DecodedSong {
  left: Float32Array;
  right: Float32Array; // the same array as left for a mono file
}

export type SongDecoder = (bytes: Uint8Array, rate: number) => Promise<DecodedSong>;

export const SFX_SOURCE_RATE = 48000;
const POOL_LIMIT = 0x07f00000; // ROM-space window above PORT_PACK_PCM_BASE (0x08000000) inside pi.c's 28-bit mask
const MIN_LOOP = 256; // samples: bounds the commands one RAW16 pull emits (n_alRaw16Pull)
const FADE_FRAMES = 256;
const CACHE_LIMIT = 256 * 1024 * 1024; // bytes of decoded Float32
const LEVEL_SCALE = 25000 / 32767; // the game's sequence volume scale (seqUpdateVolume)

interface CacheEntry {
  song: DecodedSong | null; // null while decoding
  bytes: number;
  used: number; // mix() call count at the last use
}

interface Voice {
  key: string;
  id: number;
  pos: number;
  rate: number;
  gain: number | null;
  fade: number; // frames left of the fade-out; -1: playing
}

const defaultDecoder: SongDecoder = async (bytes, rate) => {
  const buf = await new OfflineAudioContext(2, 1, rate).decodeAudioData(bytes.slice().buffer);
  const left = buf.getChannelData(0);
  return { left, right: buf.numberOfChannels > 1 ? buf.getChannelData(1) : left };
};

export class AudioPackRuntime {
  readonly fingerprint: string;
  private readonly songs: Map<number, SongSrc>;
  private readonly sfx: SfxSrc[];
  private readonly decode: SongDecoder;
  private readonly failed = new Set<number>();
  private readonly cache = new Map<string, CacheEntry>();
  private readonly players: Array<Voice | null> = [null, null, null, null];
  private fading: Voice[] = [];
  private pool = new Uint8Array(1 << 20);
  private poolUsed = 0;
  private calls = 0;

  constructor(opts: { songs: Map<number, SongSrc>; sfx: SfxSrc[]; fingerprint: string; decode?: SongDecoder }) {
    this.songs = opts.songs;
    this.sfx = [...opts.sfx].sort((a, b) => a.id - b.id);
    this.fingerprint = opts.fingerprint;
    this.decode = opts.decode ?? defaultDecoder;
  }

  hasSong(id: number): boolean {
    return this.songs.has(id) && !this.failed.has(id);
  }

  songCount(): number {
    return this.songs.size;
  }

  sfxCount(): number {
    return this.sfx.length;
  }

  sfxId(i: number): number {
    return this.sfx[i]?.id ?? 0;
  }

  // pack.c port_pack_sfx_bake: u32[outIdx..+4] = offset, byte length, loop start, loop end, loop count
  bakeSfx(i: number, rate: number, origLoopCount: number, u32: Uint32Array, outIdx: number): boolean {
    const src = this.sfx[i];
    if (!src) return false;
    const r = Math.min(192000, Math.max(1000, rate));
    const data = resample(src.pcm, SFX_SOURCE_RATE, r);
    const len = data.length;
    const offset = (this.poolUsed + 7) & ~7;
    if (offset + len * 2 > POOL_LIMIT) {
      console.warn(`audio pack: sound ${hexId(src.id)} does not fit in the sound pool`);
      return false;
    }
    this.reserve(offset + len * 2);
    const out = new Int16Array(this.pool.buffer, offset, len);
    for (let n = 0; n < len; n++) {
      out[n] = Math.max(-32768, Math.min(32767, Math.round(data[n]! * src.entry.gain * 32767)));
    }
    this.poolUsed = offset + len * 2;

    const { loop, loopStart, loopEnd } = src.entry;
    let count = loop === false ? 0 : loop === true ? 0xffffffff : origLoopCount >>> 0;
    let start = Math.min(len, Math.max(0, Math.round(loopStart * r)));
    let end = loopEnd === undefined ? len : Math.min(len, Math.round(loopEnd * r));
    if (end - start < MIN_LOOP) {
      start = 0;
      end = len;
    }
    if (len < MIN_LOOP) count = 0;
    u32.set(count ? [offset, len * 2, start, end, count] : [offset, len * 2, 0, 0, 0], outIdx);
    return true;
  }

  private reserve(size: number): void {
    if (size <= this.pool.length) return;
    let cap = this.pool.length;
    while (cap < size) cap *= 2;
    const grown = new Uint8Array(Math.min(cap, POOL_LIMIT));
    grown.set(this.pool.subarray(0, this.poolUsed));
    this.pool = grown;
  }

  // pack.c port_pack_read: pool bytes [off, off + len) into u8 at dst, zeros past the pool's end
  read(u8: Uint8Array, dst: number, off: number, len: number): void {
    const avail = Math.max(0, Math.min(len, this.poolUsed - off));
    if (avail > 0) u8.set(this.pool.subarray(off, off + avail), dst);
    if (avail < len) u8.fill(0, dst + avail, dst + len);
  }

  // pack.c port_pack_mix: add the songs into i16[bufIdx..] (frames interleaved stereo frames at rate)
  mix(
    i16: Int16Array,
    i32: Int32Array,
    f32: Float32Array,
    bufIdx: number,
    frames: number,
    rate: number,
    stateIdx: number,
  ): void {
    this.calls++;
    for (let p = 0; p < 4; p++) {
      const s = stateIdx + 4 * p;
      const id = i32[s]!;
      const gen = i32[s + 1]! >>> 0;
      const state = i32[s + 2]!;
      const level = f32[s + 3]!;
      const want = state !== 0 && this.hasSong(id) ? `${id}:${gen}` : null;
      let voice = this.players[p] ?? null;
      if (want !== (voice?.key ?? null)) {
        if (voice && voice.pos > 0) {
          voice.fade = FADE_FRAMES;
          this.fading.push(voice);
        }
        voice = want ? { key: want, id, pos: 0, rate, gain: null, fade: -1 } : null;
        this.players[p] = voice;
      }
      if (!voice) continue;
      const target = level * LEVEL_SCALE * this.songs.get(id)!.entry.gain;
      this.render(voice, i16, bufIdx, frames, rate, voice.gain ?? target, target);
      voice.gain = target;
    }
    for (const v of this.fading) {
      this.render(v, i16, bufIdx, frames, rate, v.gain ?? 0, v.gain ?? 0);
      v.fade -= frames;
    }
    this.fading = this.fading.filter((v) => v.fade > 0);
  }

  private song(id: number, rate: number): DecodedSong | null {
    const key = `${id}@${rate}`;
    const hit = this.cache.get(key);
    if (hit) {
      hit.used = this.calls;
      return hit.song;
    }
    const entry: CacheEntry = { song: null, bytes: 0, used: this.calls };
    this.cache.set(key, entry);
    this.decode(this.songs.get(id)!.bytes, rate).then(
      (song) => {
        entry.song = song;
        entry.bytes = (song.left.length + (song.right === song.left ? 0 : song.right.length)) * 4;
        this.evict();
      },
      (err: unknown) => {
        this.cache.delete(key);
        if (this.failed.has(id)) return;
        this.failed.add(id);
        console.warn('audio pack: cannot decode song 0x%X (%s)', id, err instanceof Error ? err.message : String(err));
        toast(`Audio pack song ${hexId(id)} could not be decoded; the original plays from its next start`);
      },
    );
    return null;
  }

  // least recently used decodes first, never one a voice plays
  private evict(): void {
    const inUse = new Set(
      [...this.players, ...this.fading].filter((v): v is Voice => v !== null).map((v) => `${v.id}@${v.rate}`),
    );
    let total = 0;
    for (const e of this.cache.values()) total += e.bytes;
    const order = [...this.cache.entries()].sort((a, b) => a[1].used - b[1].used);
    for (const [key, e] of order) {
      if (total <= CACHE_LIMIT) break;
      if (inUse.has(key) || !e.song) continue;
      this.cache.delete(key);
      total -= e.bytes;
    }
  }

  private render(v: Voice, i16: Int16Array, bufIdx: number, frames: number, rate: number, g0: number, g1: number) {
    if (v.rate !== rate) {
      // a quickload restored the other output rate: same place in the song, counted in the new rate
      v.pos = Math.round((v.pos * rate) / v.rate);
      v.rate = rate;
    }
    const song = this.song(v.id, v.rate);
    if (!song) return;
    const entry = this.songs.get(v.id)!.entry;
    const len = song.left.length;
    let loopStart = Math.min(len, Math.round(entry.loopStart * rate));
    let loopEnd = entry.loopEnd === undefined ? len : Math.min(len, Math.round(entry.loopEnd * rate));
    if (loopEnd <= loopStart) {
      loopStart = 0;
      loopEnd = len;
    }
    const loops = entry.loop !== false;
    const end = loops ? loopEnd : len;
    for (let i = 0; i < frames; i++) {
      if (v.pos >= end) {
        if (!loops || loopEnd <= loopStart) break;
        v.pos = loopStart + ((v.pos - loopEnd) % (loopEnd - loopStart));
      }
      let g = g0 + ((g1 - g0) * i) / frames;
      if (v.fade >= 0) g *= Math.max(0, v.fade - i) / FADE_FRAMES;
      const o = bufIdx + 2 * i;
      i16[o] = Math.max(-32768, Math.min(32767, i16[o]! + Math.round(song.left[v.pos]! * g * 32767)));
      i16[o + 1] = Math.max(-32768, Math.min(32767, i16[o + 1]! + Math.round(song.right[v.pos]! * g * 32767)));
      v.pos++;
    }
  }
}
