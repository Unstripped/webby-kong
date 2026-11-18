'use strict';
// Loads the user's ROM, verifies it, and starts the wasm port. The ROM stays in a JS Uint8Array.
const ROM_SHA1 = 'cf806ff2603640a748fca5026ded28802f1f4a50';
const statusEl = document.getElementById('status');
const canvas = document.getElementById('canvas');
let started = false;

function setStatus(text) {
  statusEl.textContent = text;
}

// z64 (big-endian) is native; v64 swaps 16-bit, n64 swaps 32-bit.
function normalizeRom(bytes) {
  const b = new Uint8Array(bytes);
  const magic = (b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) >>> 0;
  if (magic === 0x80371240) return b;
  const out = new Uint8Array(b.length);
  if (magic === 0x37804012) {
    for (let i = 0; i + 1 < b.length; i += 2) { out[i] = b[i + 1]; out[i + 1] = b[i]; }
    return out;
  }
  if (magic === 0x40123780) {
    for (let i = 0; i + 3 < b.length; i += 4) {
      out[i] = b[i + 3]; out[i + 1] = b[i + 2]; out[i + 2] = b[i + 1]; out[i + 3] = b[i];
    }
    return out;
  }
  return null;
}

async function sha1Hex(bytes) {
  const digest = await crypto.subtle.digest('SHA-1', bytes);
  return Array.from(new Uint8Array(digest), (x) => x.toString(16).padStart(2, '0')).join('');
}

async function start(romBytes) {
  if (started) return;
  const rom = normalizeRom(romBytes);
  if (!rom || (await sha1Hex(rom)) !== ROM_SHA1) {
    setStatus(`Unsupported ROM: need Donkey Kong 64 (USA), SHA-1 ${ROM_SHA1}`);
    return;
  }
  started = true;
  setStatus('Starting…');
  const module = await createDk64({
    canvas,
    preRun: [(m) => {
      m.FS.mkdir('/saves');
      m.FS.mount(m.IDBFS, {}, '/saves');
      m.addRunDependency('syncfs');
      m.FS.syncfs(true, (err) => {
        if (err) console.warn('save load failed', err);
        m.removeRunDependency('syncfs');
      });
    }],
    print: (t) => console.log(t),
    printErr: (t) => console.warn(t),
  });
  module.portRom = rom;
  window.Module = module;
  module.callMain([]);
  const ctx = module.SDL2 && module.SDL2.audioContext;
  if (ctx && ctx.state !== 'running') ctx.resume();
  canvas.focus();
  setStatus('Running. Keys: arrows = stick, X = A, C = B, Z = Z, A/S = L/R, Enter = Start, IJKL = C, TFGH = D-pad.');
}

document.getElementById('rom').addEventListener('change', async (e) => {
  const file = e.target.files[0];
  if (file) await start(await file.arrayBuffer());
});

// resume audio on any later gesture (autoplay policies)
window.addEventListener('keydown', () => {
  const ctx = window.Module && window.Module.SDL2 && window.Module.SDL2.audioContext;
  if (ctx && ctx.state !== 'running') ctx.resume();
});

// dev only: ?rom=<url>
const romUrl = new URLSearchParams(location.search).get('rom');
if (romUrl) {
  setStatus('Fetching ROM…');
  fetch(romUrl).then((r) => {
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.arrayBuffer();
  }).then(start).catch((err) => setStatus(`ROM fetch failed: ${err.message}`));
}
