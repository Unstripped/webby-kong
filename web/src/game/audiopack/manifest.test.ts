import { describe, expect, it } from 'vitest';
import { parseManifest } from './manifest';

const FILES = new Set(['a.ogg', 'b.wav', 'notes.txt']);
const base = { format: 1, name: ' Pack ' };

describe('parseManifest', () => {
  it('normalises hex and decimal ids and fills defaults', () => {
    const m = parseManifest(
      { ...base, songs: { '0x2a': { file: 'a.ogg' } }, sfx: { '291': { file: 'b.wav', loop: true, gain: 0.5 } } },
      FILES,
    );
    expect(m.name).toBe('Pack');
    expect(m.author).toBe('');
    expect(m.songs.get(42)).toEqual({ file: 'a.ogg', loop: undefined, loopStart: 0, loopEnd: undefined, gain: 1 });
    expect(m.sfx.get(0x123)).toEqual({ file: 'b.wav', loop: true, loopStart: 0, loopEnd: undefined, gain: 0.5 });
  });

  const song = (entry: unknown) => ({ ...base, songs: { '16': entry } });
  it.each<[unknown, string]>([
    [{ ...base, format: 2, songs: { '1': { file: 'a.ogg' } } }, 'format must be 1'],
    [{ format: 1, name: '   ', songs: { '1': { file: 'a.ogg' } } }, 'name must be 1-60 characters'],
    [{ ...base, author: 'x'.repeat(61), songs: { '1': { file: 'a.ogg' } } }, 'author must be a string of at most 60 characters'],
    [{ ...base, version: 3, songs: { '1': { file: 'a.ogg' } } }, 'version must be a string of at most 20 characters'],
    [{ ...base, description: 'x'.repeat(501), songs: { '1': { file: 'a.ogg' } } }, 'description must be a string of at most 500 characters'],
    [{ ...base, songs: { '176': { file: 'a.ogg' } } }, 'songs key "176" is not a song id (0-175 or 0x0-0xAF)'],
    [{ ...base, sfx: { '0': { file: 'a.ogg' } } }, 'sfx key "0" is not a sound id (1-8191 or 0x1-0x1FFF)'],
    [{ ...base, sfx: { '0x2000': { file: 'a.ogg' } } }, 'sfx key "0x2000" is not a sound id (1-8191 or 0x1-0x1FFF)'],
    [{ ...base, songs: { '16': { file: 'a.ogg' }, '0x10': { file: 'a.ogg' } } }, 'songs id 0x10 is listed twice'],
    [song({ file: 'missing.ogg' }), 'songs 0x10: file "missing.ogg" is not in the pack'],
    [song({ file: 'notes.txt' }), 'songs 0x10: file must be .ogg, .oga, .opus, .mp3, .wav or .flac'],
    [song({ file: 'a.ogg', loop: 'yes' }), 'songs 0x10: loop is invalid'],
    [song({ file: 'a.ogg', loopStart: -1 }), 'songs 0x10: loopStart is invalid'],
    [song({ file: 'a.ogg', loopStart: 2, loopEnd: 2 }), 'songs 0x10: loopEnd is invalid'],
    [song({ file: 'a.ogg', gain: 0 }), 'songs 0x10: gain is invalid'],
    [song({ file: 'a.ogg', gain: 4.5 }), 'songs 0x10: gain is invalid'],
    [{ ...base }, 'the pack replaces nothing (no songs or sfx)'],
  ])('rejects %j', (json, message) => {
    expect(() => parseManifest(json, FILES)).toThrow(message);
  });
});
