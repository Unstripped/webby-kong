import { describe, expect, it } from 'vitest';
import { checkWildcard, hashFromString, parseDatabase, resolve } from './rt64db';

describe('parseDatabase', () => {
  it('defaults a missing configuration to the current one', () => {
    expect(parseDatabase('{}').config).toEqual({
      autoPath: 'rt64',
      defaultOperation: 'stream',
      defaultShift: 'half',
      configurationVersion: 3,
      hashVersion: 5,
    });
  });

  it('defaults missing configuration keys as RT64 does', () => {
    const c = parseDatabase('{"configuration":{}}').config;
    expect(c.configurationVersion).toBe(1);
    expect(c.hashVersion).toBe(1);
    expect(c.defaultShift).toBe('none');
    expect(parseDatabase('{"configuration":{"configurationVersion":3}}').config.defaultShift).toBe('half');
    expect(parseDatabase('{"configuration":{"defaultOperation":"fast"}}').config.defaultOperation).toBe('preload');
  });

  it('reads legacy rt64v1 hashes', () => {
    const db = parseDatabase('{"textures":[{"hashes":{"rt64v1":"ab"}},{"hashes":{"rt64v1":"ab","rt64":"cd"}}]}');
    expect(db.textures.map((t) => t.rt64)).toEqual(['ab', 'cd']);
  });

  it('rejects invalid JSON and non-objects', () => {
    expect(() => parseDatabase('{')).toThrow(/^rt64\.json: /);
    expect(() => parseDatabase('[]')).toThrow('rt64.json: not a JSON object');
  });
});

describe('checkWildcard', () => {
  it('matches like RT64', () => {
    expect(checkWildcard('Terrain/Grass001.png', 'Terrain/Grass001.*')).toBe(true);
    expect(checkWildcard('Text/a/b.png', 'Text/*')).toBe(true);
    expect(checkWildcard('UI/Button1.png', '*/Button*')).toBe(true);
    expect(checkWildcard('UI/LetterA.dds', 'UI/Letter?.*')).toBe(true);
    expect(checkWildcard('UI/LetterAB.dds', 'UI/Letter?.*')).toBe(false);
    expect(checkWildcard('ui/x.png', 'UI/*')).toBe(false);
  });
});

describe('hashFromString', () => {
  it('parses like strtoull base 16', () => {
    expect(hashFromString('0001d0556bc980b5')).toBe(0x0001d0556bc980b5n);
    expect(hashFromString('  0xABC')).toBe(0xabcn);
    expect(hashFromString('zz')).toBe(0n);
    expect(hashFromString('123456789abcdef01')).toBe(0xffffffffffffffffn);
  });
});

describe('resolve', () => {
  const paths = ['a/tex.png', 'a/tex.dds', 'b/610135f4c4a8283a.png', 'b/x.PNG'];

  it('resolves explicit and auto paths, first per hash', () => {
    const db = parseDatabase(
      JSON.stringify({
        textures: [
          { hashes: { rt64: '1' }, path: 'a/tex.png' },
          { hashes: { rt64: '610135f4c4a8283a' }, path: '' },
          { hashes: { rt64: '2' }, path: 'b/x' },
          { hashes: { rt64: '3' }, path: 'missing' },
          { hashes: { rt64: '1' }, path: 'b/x' },
          { hashes: { rt64: '' }, path: 'a/tex' },
        ],
      }),
    );
    const r = resolve(db, paths);
    expect(r.textures.map((t) => t.path)).toEqual(['a/tex.dds', 'b/610135f4c4a8283a.png', 'b/x.PNG']);
    expect(r.textures.map((t) => t.hash)).toEqual([1n, 0x610135f4c4a8283an, 2n]);
    expect(r.total).toBe(5);
    expect(r.missing).toBe(1);
  });

  it('applies filters in order, explicit values first', () => {
    const db = parseDatabase(
      JSON.stringify({
        operationFilters: [
          { wildcard: 'a/*', operation: 'preload' },
          { wildcard: 'a/tex.*', operation: 'stall' },
        ],
        shiftFilters: [{ wildcard: '*', shift: 'half' }],
        textures: [
          { hashes: { rt64: '1' }, path: 'a/tex' },
          { hashes: { rt64: '2' }, path: 'b/x', shift: 'none' },
        ],
      }),
    );
    const [tex, x] = resolve(db, paths).textures;
    expect(tex).toMatchObject({ path: 'a/tex.dds', operation: 'stall', shift: 'half' });
    expect(x).toMatchObject({ path: 'b/x.PNG', operation: 'stream', shift: 'none' });
  });
});
