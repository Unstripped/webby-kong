import { describe, expect, it } from 'vitest';
import { buildZip, deflateRaw } from './testzip';
import { readZip } from './zip';

const enc = new TextEncoder();

describe('readZip', () => {
  it('reads stored and deflated entries', async () => {
    const text = enc.encode('hello hello hello hello audio pack');
    const zip = buildZip([
      { name: 'manifest.json', method: 0, data: enc.encode('{}'), size: 2 },
      { name: 'music/a.ogg', method: 8, data: await deflateRaw(text), size: text.length },
    ]);
    const files = await readZip(zip);
    expect([...files.keys()]).toEqual(['manifest.json', 'music/a.ogg']);
    expect(new TextDecoder().decode(files.get('manifest.json'))).toBe('{}');
    expect(files.get('music/a.ogg')).toEqual(text);
  });

  it('rejects a ../ entry name', async () => {
    const zip = buildZip([{ name: '../a.ogg', method: 0, data: enc.encode('x'), size: 1 }]);
    await expect(readZip(zip)).rejects.toThrow('entry "../a.ogg" has an unsafe name');
  });

  it('rejects an entry whose declared size does not match its data', async () => {
    const text = enc.encode('some audio bytes');
    const zip = buildZip([{ name: 'a.ogg', method: 8, data: await deflateRaw(text), size: text.length + 100 }]);
    await expect(readZip(zip)).rejects.toThrow('corrupt entry a.ogg');
  });

  it('rejects what is not a zip', async () => {
    await expect(readZip(enc.encode('not a zip at all, just text'))).rejects.toThrow('not a zip file');
  });
});
