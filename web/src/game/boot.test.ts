import { describe, expect, it, vi } from 'vitest';
import { normalizeRom } from './boot';

vi.mock('./menu/control', () => ({ started: vi.fn() }));

const z64 = [0x80, 0x37, 0x12, 0x40, 0x0f, 0x00, 0x00, 0x00];

describe('normalizeRom', () => {
  it('keeps z64 (big-endian) bytes', () => {
    expect(Array.from(normalizeRom(new Uint8Array(z64))!)).toEqual(z64);
  });

  it('unswaps v64 (16-bit) and n64 (32-bit) dumps', () => {
    const v64 = [0x37, 0x80, 0x40, 0x12, 0x00, 0x0f, 0x00, 0x00];
    const n64 = [0x40, 0x12, 0x37, 0x80, 0x00, 0x00, 0x00, 0x0f];
    expect(Array.from(normalizeRom(new Uint8Array(v64))!)).toEqual(z64);
    expect(Array.from(normalizeRom(new Uint8Array(n64))!)).toEqual(z64);
  });

  it('rejects an unknown header', () => {
    expect(normalizeRom(new Uint8Array(8))).toBeNull();
  });
});
