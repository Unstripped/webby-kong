import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { loadAudioPacks } from './load';
import type { DecodedSong } from './runtime';
import { setEnabled } from './store';
import { packZip } from './testzip';

const PACKS: Record<string, Uint8Array<ArrayBuffer>> = {
  'api/packs/1/file': packZip({ format: 1, name: 'One', songs: { '0x10': { file: 'a.ogg' } } }, { 'a.ogg': 'one' }),
  'api/packs/2/file': packZip(
    { format: 1, name: 'Two', songs: { '16': { file: 'b.ogg' }, '17': { file: 'b.ogg' } } },
    { 'b.ogg': 'two' },
  ),
};

beforeEach(() => {
  localStorage.clear();
  vi.stubGlobal(
    'fetch',
    vi.fn(async (url: string) => {
      const body = PACKS[url];
      return body ? new Response(body) : new Response('{"error":"not_found"}', { status: 404 });
    }),
  );
});

afterEach(() => {
  vi.unstubAllGlobals();
});

// which file bytes the runtime decodes for a song: the winning pack's
async function songText(order: string[], id: number): Promise<{ text: string; fingerprint: string }> {
  for (const key of [...order].reverse()) setEnabled(key, true);
  const decoded: string[] = [];
  const rt = await loadAudioPacks({
    song: async (bytes): Promise<DecodedSong> => {
      decoded.push(new TextDecoder().decode(bytes));
      const s = new Float32Array(1);
      return { left: s, right: s };
    },
  });
  expect(rt).not.toBeNull();
  const buf = new ArrayBuffer(256);
  const i32 = new Int32Array(buf);
  i32.set([id, 1, 1], 0);
  rt!.mix(new Int16Array(buf), i32, new Float32Array(buf), 32, 4, 22050, 0);
  localStorage.clear();
  return { text: decoded[0]!, fingerprint: rt!.fingerprint };
}

describe('loadAudioPacks', () => {
  it('lets the first enabled pack win a song both replace', async () => {
    expect((await songText(['s:1', 's:2'], 16)).text).toBe('one');
    expect((await songText(['s:2', 's:1'], 16)).text).toBe('two');
    expect((await songText(['s:1', 's:2'], 17)).text).toBe('two');
  });

  it('changes the fingerprint with the order', async () => {
    const a = (await songText(['s:1', 's:2'], 16)).fingerprint;
    const b = (await songText(['s:2', 's:1'], 16)).fingerprint;
    expect(a).toMatch(/^[0-9a-f]{12}$/);
    expect(a).not.toBe(b);
  });

  it('turns off a pack the server no longer has', async () => {
    setEnabled('s:9', true);
    expect(await loadAudioPacks()).toBeNull();
    expect(localStorage.getItem('dk64.audiopacks')).toBe('[]');
  });
});
