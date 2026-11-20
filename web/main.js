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
  let module;
  try {
    // the build id names the exact wasm this page runs: the same bytes are instantiated (quicksave.js)
    const wasmBytes = await fetch('dk64.wasm', { cache: 'no-cache' }).then((r) => {
      if (!r.ok) throw new Error(`dk64.wasm HTTP ${r.status}`);
      return r.arrayBuffer();
    });
    const buildId = await sha1Hex(wasmBytes);
    const wasmModule = await WebAssembly.compile(wasmBytes);
    module = await createDk64({
      canvas,
      // instantiate the hashed bytes (Module.wasmBinary is not in this build's INCOMING_MODULE_JS_API)
      instantiateWasm: (imports, done) => {
        WebAssembly.instantiate(wasmModule, imports).then(done, (err) => setStatus(`Start failed: ${err.message}`));
        return {};
      },
      preRun: [(m) => {
        m.FS.mkdir('/saves');
        m.FS.mount(m.IDBFS, {}, '/saves');
        m.addRunDependency('syncfs');
        m.FS.syncfs(true, (err) => {
          if (err) console.warn('save load failed', err);
          // saves.js: back up / restore the EEPROM through the save server before main reads it
          (window.dk64saves ? window.dk64saves.boot(m.FS) : Promise.resolve()).finally(() => m.removeRunDependency('syncfs'));
        });
      }],
      print: (t) => console.log(t),
      printErr: (t) => console.warn(t),
    });
    module.portBuildId = buildId;
  } catch (err) {
    setStatus(`Start failed: ${err.message}`);
    return;
  }
  module.portRom = rom;
  window.Module = module;
  if (window.dk64dbg) window.dk64dbg.moduleReady(); // before callMain: applies the saved trace mask from boot
  const args = window.dk64settings.args();
  module.callMain(args);
  window.dk64settings.started();
  if (window.dk64qs) window.dk64qs.started().catch((err) => console.warn('quicksave resume failed', err));
  if (window.dk64touch) window.dk64touch.started();
  if (window.dk64menu) window.dk64menu.started();
  const ctx = module.SDL2 && module.SDL2.audioContext;
  if (ctx && ctx.state !== 'running') ctx.resume();
  canvas.focus();
  setStatus('Running. Keys: arrows = stick, X = A, C = B, Z = Z, A/S = L/R, Enter = Start, IJKL = C, TFGH = D-pad, Esc = menu.');
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
