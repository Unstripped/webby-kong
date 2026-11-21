// Player settings (draw distance, render scale, MSAA, touch controller, 3D sound, audio rate): stored in localStorage
// `dk64.settings`, a URL parameter overrides the stored value for that page load; the Esc menu changes them live,
// except the audio rate, which the game reads at boot (it applies on the next load).
(function () {
  'use strict';

  const KEY = 'dk64.settings';
  const num = (s) => (s === '' ? NaN : Number(s));
  const DEFS = {
    drawdist: { def: 4, valid: (v) => Number.isFinite(v) && v >= 1 && v <= 16, parse: num, fn: 'port_set_drawdist' },
    scale: { def: 0, valid: (v) => Number.isInteger(v) && v >= 0 && v <= 8, parse: num, fn: 'port_set_scale' },
    msaa: { def: 4, valid: (v) => [0, 1, 2, 4, 8].includes(v), parse: num, fn: 'port_set_msaa' },
    touch: {
      def: 'auto',
      valid: (v) => v === 'auto' || v === 'on' || v === 'off',
      parse: (s) => (s === '1' ? 'on' : s === '0' ? 'off' : s),
      fn: null,
      apply: (v) => window.dk64touch && window.dk64touch.setMode(v),
    },
    audio3d: { def: 1, valid: (v) => v === 0 || v === 1, parse: num, fn: 'port_set_audio_hrtf' },
    audiorate: { def: 44100, valid: (v) => v === 22050 || v === 44100, parse: num, fn: null },
  };

  let stored = {};
  try {
    const parsed = JSON.parse(localStorage.getItem(KEY));
    if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) stored = parsed;
  } catch (err) {
    console.warn('settings: ignoring stored settings', err);
  }

  const values = {};
  const params = new URLSearchParams(location.search);
  for (const [k, d] of Object.entries(DEFS)) {
    values[k] = d.valid(stored[k]) ? stored[k] : d.def;
    if (params.has(k)) {
      const raw = params.get(k);
      const v = d.parse(raw);
      if (d.valid(v)) values[k] = v;
      else console.warn(`settings: ignoring ${k}=${raw}`);
    }
  }

  let isStarted = false;

  function get(key) {
    return values[key];
  }

  function set(key, value) {
    const d = DEFS[key];
    if (!d.valid(value)) return values[key];
    values[key] = value;
    stored[key] = value; // only this key: a URL override of another key never reaches storage
    try {
      localStorage.setItem(KEY, JSON.stringify(stored));
    } catch (err) {
      console.warn('settings: cannot store', err);
    }
    if (isStarted) {
      if (d.fn) window.Module.ccall(d.fn, 'number', ['number'], [value]);
      else if (d.apply) d.apply(value);
    }
    return values[key];
  }

  function args() {
    return [
      '--drawdist', String(values.drawdist), '--scale', String(values.scale), '--msaa', String(values.msaa),
      '--hrtf', String(values.audio3d), '--audio-rate', String(values.audiorate),
    ];
  }

  function started() {
    isStarted = true;
  }

  function maxSamples() {
    return isStarted ? window.Module.ccall('port_gfx_max_samples', 'number') : 0;
  }

  // the rate the running game uses: a quickload restores the rate its snapshot was made at
  function audioRate() {
    return isStarted ? window.Module.ccall('port_audio_rate', 'number') : 0;
  }

  window.dk64settings = { get, set, args, started, maxSamples, audioRate };
})();
