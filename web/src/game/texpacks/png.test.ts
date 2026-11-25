// @vitest-environment node
import { describe, expect, it } from 'vitest';
import { encodePng } from './png';
import { crc32 } from './zip';

describe('encodePng', () => {
  it('writes exact RGBA, including the RGB of transparent pixels', async () => {
    const rgba = new Uint8Array([
      255, 0, 0, 255, 0, 255, 0, 128, 10, 20, 30, 0, //
      1, 2, 3, 4, 200, 100, 50, 25, 0, 0, 0, 255,
    ]);
    const png = await encodePng(rgba, 3, 2);
    const v = new DataView(png.buffer, png.byteOffset, png.byteLength);
    expect([...png.subarray(0, 8)]).toEqual([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const chunks = new Map<string, Uint8Array>();
    for (let at = 8; at < png.length; ) {
      const len = v.getUint32(at);
      const type = String.fromCharCode(...png.subarray(at + 4, at + 8));
      expect(v.getUint32(at + 8 + len)).toBe(crc32(png.subarray(at + 4, at + 8 + len)));
      chunks.set(type, png.subarray(at + 8, at + 8 + len));
      at += 12 + len;
    }
    expect([...chunks.keys()]).toEqual(['IHDR', 'IDAT', 'IEND']);
    expect([...chunks.get('IHDR')!]).toEqual([0, 0, 0, 3, 0, 0, 0, 2, 8, 6, 0, 0, 0]);
    const idat = chunks.get('IDAT')!.slice();
    const raw = new Uint8Array(
      await new Response(new Blob([idat]).stream().pipeThrough(new DecompressionStream('deflate'))).arrayBuffer(),
    );
    expect([...raw]).toEqual([0, ...rgba.subarray(0, 12), 0, ...rgba.subarray(12)]);
  });
});
