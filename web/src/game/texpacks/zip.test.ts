// @vitest-environment node
import zlib from 'node:zlib';
import { describe, expect, it } from 'vitest';
import { crc32, listZip, readZipEntry, writeZip } from './zip';

const bytes = (s: string) => new TextEncoder().encode(s);

// a zip whose entries are already compressed with `method` (the writer only stores)
function rawZip(entries: Array<{ name: string; method: number; data: Uint8Array; size: number }>): Blob {
  const parts: Uint8Array[] = [];
  const central: Uint8Array[] = [];
  let offset = 0;
  for (const e of entries) {
    const name = bytes(e.name);
    const local = new Uint8Array(30 + name.length);
    const lv = new DataView(local.buffer);
    lv.setUint32(0, 0x04034b50, true);
    lv.setUint16(8, e.method, true);
    lv.setUint32(18, e.data.length, true);
    lv.setUint32(22, e.size, true);
    lv.setUint16(26, name.length, true);
    local.set(name, 30);
    const cen = new Uint8Array(46 + name.length);
    const cv = new DataView(cen.buffer);
    cv.setUint32(0, 0x02014b50, true);
    cv.setUint16(10, e.method, true);
    cv.setUint32(20, e.data.length, true);
    cv.setUint32(24, e.size, true);
    cv.setUint16(28, name.length, true);
    cv.setUint32(42, offset, true);
    cen.set(name, 46);
    parts.push(local, e.data);
    central.push(cen);
    offset += local.length + e.data.length;
  }
  const size = central.reduce((n, c) => n + c.length, 0);
  const end = new Uint8Array(22);
  const ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true);
  ev.setUint16(8, entries.length, true);
  ev.setUint16(10, entries.length, true);
  ev.setUint32(12, size, true);
  ev.setUint32(16, offset, true);
  return new Blob([...parts, ...central, end] as BlobPart[]);
}

describe('zip', () => {
  it('round-trips writeZip through listZip and readZipEntry', async () => {
    const files = [
      { name: 'rt64.json', data: bytes('{"textures":[]}') },
      { name: 'dir/äb.png', data: new Uint8Array([1, 2, 3, 250]) },
    ];
    const blob = writeZip(files);
    const entries = await listZip(blob);
    expect(entries.map((e) => [e.name, e.method, e.size])).toEqual([
      ['rt64.json', 0, 15],
      ['dir/äb.png', 0, 4],
    ]);
    for (const [i, e] of entries.entries()) expect(await readZipEntry(blob, e)).toEqual(files[i]!.data);
  });

  it('computes CRC-32', () => {
    expect(crc32(bytes('hello'))).toBe(0x3610a686);
  });

  it('reads deflate and zstd entries', async () => {
    const text = bytes('texture '.repeat(100));
    const blob = rawZip([
      { name: 'a.txt', method: 8, data: zlib.deflateRawSync(text), size: text.length },
      { name: 'b.txt', method: 93, data: zlib.zstdCompressSync(text), size: text.length },
    ]);
    const [a, b] = await listZip(blob);
    expect(await readZipEntry(blob, a!)).toEqual(text);
    expect(await readZipEntry(blob, b!)).toEqual(text);
  });

  it('rejects other compression methods and non-zips', async () => {
    const blob = rawZip([{ name: 'a.txt', method: 12, data: bytes('xx'), size: 2 }]);
    const [a] = await listZip(blob);
    await expect(readZipEntry(blob, a!)).rejects.toThrow('Unsupported zip compression (method 12)');
    await expect(listZip(new Blob(['not a zip']))).rejects.toThrow('Not a zip archive');
  });

  it('normalises names and drops directories and escapes', async () => {
    const blob = rawZip(
      ['a\\b.png', '../evil.png', 'dir/', './c.png'].map((name) => ({ name, method: 0, data: bytes('x'), size: 1 })),
    );
    expect((await listZip(blob)).map((e) => e.name)).toEqual(['a/b.png', 'c.png']);
  });
});
