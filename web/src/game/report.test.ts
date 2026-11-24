import { describe, expect, it, vi } from 'vitest';
import { decode, encode, issueUrl, stackFrames, trimProbe, type Report } from './report';

describe('report blob', () => {
  it('round-trips through DK64R1', async () => {
    const obj = { v: 1, save: 'ab'.repeat(64), resume: { map: 7 }, settings: { drawdist: 4 }, mark: null };
    const blob = await encode(obj);
    expect(blob.startsWith('DK64R1.')).toBe(true);
    expect(await decode(`issue text ${blob} more`)).toEqual(obj);
  });

  it('rejects text without a blob and other versions', async () => {
    await expect(decode('nothing')).rejects.toThrow('no DK64R1 report data found');
    await expect(decode(await encode({ v: 2 }))).rejects.toThrow('unsupported report version');
  });
});

describe('stackFrames', () => {
  it('names wasm frames by function index and offset, at most 24', () => {
    const stack = ['RuntimeError: unreachable'];
    for (let i = 0; i < 30; i++) stack.push(`    at dk64.wasm.fn${i} (http://localhost/dk64.wasm:wasm-function[${12 + i}]:0x1f)`);
    const frames = stackFrames(stack.join('\n'));
    expect(frames).toHaveLength(24);
    expect(frames[0]).toBe('fn0 [12]+0x1f');
  });
});

describe('issueUrl', () => {
  it('fits a long bug report under the URL limit', () => {
    const report: Report = {
      data: {
        v: 1, version: null, revision: null, buildId: null, save: null, resume: null, progress: null,
        settings: {}, crash: null, stack: null, mark: null,
      },
      table: [['Version', 'source build']],
      trace: Array.from({ length: 30 }, (_, i) => `vi ${i}  event ${'x'.repeat(200)}`),
      log: Array.from({ length: 30 }, (_, i) => `line ${i} ${'y'.repeat(200)}`),
      blobs: ['DK64R1.' + 'A'.repeat(3000)],
      place: 'map 0x7',
    };
    const url = issueUrl('bug', 'z'.repeat(20000), report);
    expect(url.length).toBeLessThanOrEqual(7500);
    expect(url.startsWith('https://github.com/Unstripped/webby-kong/issues/new?')).toBe(true);
  });
});

describe('trimProbe', () => {
  it('drops the probe at cap 0', () => {
    expect(trimProbe('#1 dl=0 ...', 0)).toBeNull();
  });
});
