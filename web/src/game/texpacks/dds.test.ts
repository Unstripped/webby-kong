import { describe, expect, it } from 'vitest';
import { parseDds, TEX_BC1, TEX_BC7, TEX_RGBA8 } from './dds';

const code = (s: string) => s.charCodeAt(0) | (s.charCodeAt(1) << 8) | (s.charCodeAt(2) << 16) | (s.charCodeAt(3) << 24);

interface Header {
  width: number;
  height: number;
  mips?: number;
  fourCC?: string;
  dxgi?: number;
  masks?: [number, number, number, number]; // 32-bit RGB: R, G, B, A
}

function dds(h: Header, data: number[] | number): Uint8Array<ArrayBuffer> {
  const head = h.dxgi === undefined ? 128 : 148;
  const body = typeof data === 'number' ? new Array<number>(data).fill(0) : data;
  const out = new Uint8Array(head + body.length);
  const v = new DataView(out.buffer);
  v.setUint32(0, code('DDS '), true);
  v.setUint32(4, 124, true);
  v.setUint32(8, h.mips ? 0x20000 : 0, true);
  v.setUint32(12, h.height, true);
  v.setUint32(16, h.width, true);
  v.setUint32(28, h.mips ?? 0, true);
  v.setUint32(76, 32, true);
  if (h.masks) {
    v.setUint32(80, 0x40, true);
    v.setUint32(88, 32, true);
    h.masks.forEach((m, i) => v.setUint32(92 + i * 4, m, true));
  } else {
    v.setUint32(80, 0x4, true);
    v.setUint32(84, code(h.dxgi === undefined ? h.fourCC! : 'DX10'), true);
  }
  if (h.dxgi !== undefined) {
    v.setUint32(128, h.dxgi, true);
    v.setUint32(132, 3, true);
    v.setUint32(140, 1, true);
  }
  out.set(body, head);
  return out;
}

describe('parseDds', () => {
  it('reads a DXT1 mip chain', () => {
    const img = parseDds(dds({ width: 8, height: 8, mips: 2, fourCC: 'DXT1' }, 40));
    expect(img.format).toBe(TEX_BC1);
    expect(img.levels.map((l) => l.length)).toEqual([32, 8]);
  });

  it('reads DX10 BC7', () => {
    const img = parseDds(dds({ width: 16, height: 16, dxgi: 98 }, 256));
    expect(img.format).toBe(TEX_BC7);
    expect(img.levels.map((l) => l.length)).toEqual([256]);
  });

  it('swizzles DX10 BGRA to RGBA', () => {
    const img = parseDds(dds({ width: 2, height: 1, dxgi: 87 }, [1, 2, 3, 4, 5, 6, 7, 8]));
    expect(img.format).toBe(TEX_RGBA8);
    expect([...img.levels[0]!]).toEqual([3, 2, 1, 4, 7, 6, 5, 8]);
  });

  it('swizzles legacy BGR masks and makes them opaque without an alpha mask', () => {
    const img = parseDds(dds({ width: 1, height: 1, masks: [0xff0000, 0xff00, 0xff, 0] }, [10, 20, 30, 0]));
    expect([...img.levels[0]!]).toEqual([30, 20, 10, 255]);
  });

  it('stops a compressed chain at the first size WebGL rejects', () => {
    const img = parseDds(dds({ width: 24, height: 24, mips: 4, fourCC: 'DXT1' }, 288 + 72 + 32 + 8));
    expect(img.levels.map((l) => l.length)).toEqual([288, 72]);
  });

  it('rejects unsupported formats, unaligned compressed sizes and truncated data', () => {
    expect(() => parseDds(dds({ width: 4, height: 4, dxgi: 2 }, 256))).toThrow(/^DDS: /);
    expect(() => parseDds(dds({ width: 6, height: 6, fourCC: 'DXT1' }, 32))).toThrow(/^DDS: /);
    expect(() => parseDds(dds({ width: 8, height: 8, fourCC: 'DXT1' }, 16))).toThrow('DDS: truncated data');
  });
});
