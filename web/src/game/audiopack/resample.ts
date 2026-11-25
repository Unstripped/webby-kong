// Lanczos-3 sample rate conversion for audio pack sounds (baked to the rate the game's synthesizer plays them).
// Downsampling widens the kernel by inRate/outRate, so it also low-passes below the new Nyquist frequency.

function sinc(t: number): number {
  if (t === 0) return 1;
  const x = Math.PI * t;
  return Math.sin(x) / x;
}

export function resample(input: Float32Array, inRate: number, outRate: number): Float32Array {
  const len = Math.floor((input.length * outRate) / inRate);
  const out = new Float32Array(len);
  const step = inRate / outRate;
  const s = Math.min(1, outRate / inRate);
  const reach = 3 / s;
  for (let n = 0; n < len; n++) {
    const x = n * step;
    const lo = Math.max(0, Math.ceil(x - reach));
    const hi = Math.min(input.length - 1, Math.floor(x + reach));
    let acc = 0;
    let norm = 0;
    for (let k = lo; k <= hi; k++) {
      const d = s * (x - k);
      const w = sinc(d) * sinc(d / 3);
      acc += input[k]! * w;
      norm += w;
    }
    out[n] = norm === 0 ? 0 : acc / norm;
  }
  return out;
}
