import { expect, it } from 'vitest';
import { resample } from './resample';

function zeroCrossings(x: Float32Array): number {
  let n = 0;
  for (let i = 1; i < x.length; i++) if (x[i - 1]! < 0 !== x[i]! < 0) n++;
  return n;
}

it('keeps a 1 kHz tone at 1 kHz when converting 48000 to 22050', () => {
  const n = 48000;
  const input = new Float32Array(n);
  for (let i = 0; i < n; i++) input[i] = Math.sin((2 * Math.PI * 1000 * i) / 48000 + 0.3);
  const out = resample(input, 48000, 22050);
  expect(out.length).toBe(Math.floor((n * 22050) / 48000));
  const rateIn = zeroCrossings(input) / (n / 48000);
  const rateOut = zeroCrossings(out) / (out.length / 22050);
  expect(Math.abs(rateOut - rateIn) / rateIn).toBeLessThan(0.01);
});
