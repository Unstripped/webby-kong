// boot.ts: the enabled audio packs merged into one AudioPackRuntime before the game starts (null: none applies).
// For each song or sound the highest enabled pack that replaces it wins. Never throws: a pack that cannot be
// fetched or read is skipped with a toast.
import { sha1Hex } from '../../shared/sha1';
import { toast } from '../ui/toast';
import type { ManifestEntry } from './manifest';
import { AudioPackRuntime, SFX_SOURCE_RATE, type SfxSrc, type SongDecoder, type SongSrc } from './runtime';
import { enabled, getLocalBytes, setEnabled } from './store';
import { openPack } from './zip';

export type SfxDecoder = (bytes: Uint8Array) => Promise<Float32Array>;

// mono at SFX_SOURCE_RATE (channels averaged)
const defaultSfxDecoder: SfxDecoder = async (bytes) => {
  const buf = await new OfflineAudioContext(1, 1, SFX_SOURCE_RATE).decodeAudioData(bytes.slice().buffer);
  const out = new Float32Array(buf.length);
  for (let c = 0; c < buf.numberOfChannels; c++) {
    const ch = buf.getChannelData(c);
    for (let i = 0; i < out.length; i++) out[i]! += ch[i]! / buf.numberOfChannels;
  }
  return out;
};

// the pack's bytes, or null when it is gone (and was turned off)
async function packBytes(key: string): Promise<Uint8Array<ArrayBuffer> | null> {
  if (key.startsWith('s:')) {
    const id = key.slice(2);
    const r = await fetch(`api/packs/${encodeURIComponent(id)}/file`, { credentials: 'same-origin' });
    if (r.status === 404) {
      // only tools/packs.py's own answer: a server without the packs API (plain http.server) keeps the choice
      const body = (await r.json().catch(() => null)) as { error?: string } | null;
      if (body?.error === 'not_found') {
        setEnabled(key, false);
        toast(`Audio pack ${id} is no longer on the server and was turned off`);
        return null;
      }
    }
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return new Uint8Array(await r.arrayBuffer());
  }
  const bytes = key.startsWith('l:') ? await getLocalBytes(key.slice(2)) : null;
  if (!bytes) setEnabled(key, false);
  return bytes;
}

// decoders: the browser's (OfflineAudioContext) unless a test supplies its own
export async function loadAudioPacks(
  decoders: { sfx?: SfxDecoder; song?: SongDecoder } = {},
): Promise<AudioPackRuntime | null> {
  const decodeSfx = decoders.sfx ?? defaultSfxDecoder;
  const keys = enabled();
  if (!keys.length) return null;
  const applied: string[] = [];
  const songs = new Map<number, SongSrc>();
  const sfx = new Map<number, { entry: ManifestEntry; bytes: Uint8Array; pack: string }>();
  for (const key of keys) {
    let label = key;
    try {
      const bytes = await packBytes(key);
      if (!bytes) continue;
      const { files, manifest } = await openPack(bytes);
      label = manifest.name;
      applied.push(key + ':' + (await sha1Hex(bytes)));
      for (const [id, entry] of manifest.songs) {
        if (!songs.has(id)) songs.set(id, { entry, bytes: files.get(entry.file)! });
      }
      for (const [id, entry] of manifest.sfx) {
        if (!sfx.has(id)) sfx.set(id, { entry, bytes: files.get(entry.file)!, pack: manifest.name });
      }
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      console.warn(`audio pack ${key} skipped`, err);
      toast(`Audio pack "${label}" skipped: ${message}`);
    }
  }

  const sounds: SfxSrc[] = [];
  const failures = new Map<string, number>();
  for (const [id, s] of sfx) {
    try {
      sounds.push({ id, entry: s.entry, pcm: await decodeSfx(s.bytes) });
    } catch (err) {
      console.warn(`audio pack: cannot decode sound ${id}`, err);
      failures.set(s.pack, (failures.get(s.pack) ?? 0) + 1);
    }
  }
  for (const [pack, n] of failures) toast(`Audio pack "${pack}": ${n} sounds could not be decoded`);

  if (songs.size + sounds.length === 0) return null;
  const fingerprint = (await sha1Hex(new TextEncoder().encode(applied.join('\n')))).slice(0, 12);
  return new AudioPackRuntime({ songs, sfx: sounds, fingerprint, decode: decoders.song });
}
