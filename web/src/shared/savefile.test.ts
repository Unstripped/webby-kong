import { afterEach, describe, expect, it, vi } from 'vitest';
import { fileName, parse } from './savefile';

describe('parse', () => {
  it('returns the bytes of a 2048-byte .eep', () => {
    const buf = new Uint8Array(2048).map((_, i) => i & 0xff);
    expect(Array.from(parse(buf.buffer))).toEqual(Array.from(buf));
  });

  it('returns the first 2048 bytes of a RetroArch .srm', () => {
    const buf = new Uint8Array(0x48800).map((_, i) => (i * 7) & 0xff);
    expect(Array.from(parse(buf.buffer))).toEqual(Array.from(buf.subarray(0, 2048)));
  });

  it('rejects other sizes with the byte count', () => {
    expect(() => parse(new ArrayBuffer(100))).toThrow('got 100 bytes');
  });

  it('rejects a blank EEPROM', () => {
    const blank = 'This file holds no save: its EEPROM is blank.';
    expect(() => parse(new Uint8Array(2048).fill(0xff).buffer)).toThrow(blank);
    expect(() => parse(new ArrayBuffer(2048))).toThrow(blank);
  });
});

describe('fileName', () => {
  afterEach(() => vi.useRealTimers());

  it('slugs the save name and stamps local time', () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date(2026, 0, 2, 3, 4));
    expect(fileName('My Save!')).toBe('dk64-my-save-20260102-0304.eep');
    expect(fileName('')).toBe('dk64-save-20260102-0304.eep');
  });
});
