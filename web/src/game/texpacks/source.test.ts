// @vitest-environment node
import { describe, expect, it } from 'vitest';
import { findRoot, openSource } from './source';
import { writeZip } from './zip';

describe('findRoot', () => {
  it('finds rt64.json at the top or in one top-level directory', () => {
    expect(findRoot(['rt64.json', 'x.png'])).toBe('');
    expect(findRoot(['RT64.JSON'])).toBe('');
    expect(findRoot(['Pack/rt64.json', 'Pack/x.png'])).toBe('Pack/');
    expect(findRoot(['A/rt64.json', 'B/rt64.json'])).toBeNull();
    expect(findRoot(['x.png'])).toBeNull();
  });
});

describe('openSource', () => {
  it('lists a zip under its prefix and matches paths case-insensitively', async () => {
    const data = new TextEncoder().encode('png');
    const blob = writeZip([
      { name: 'Pack/rt64.json', data: new TextEncoder().encode('{}') },
      { name: 'Pack/x.png', data },
      { name: 'other.txt', data },
    ]);
    const src = await openSource({ kind: 'zip', blob, prefix: 'Pack/' });
    expect(src.paths()).toEqual(['rt64.json', 'x.png']);
    expect(src.locate('X.PNG')?.entry?.name).toBe('Pack/x.png');
    expect(await src.read('X.PNG')).toEqual(data);
    await expect(src.read('y.png')).rejects.toThrow('y.png missing');
  });
});
