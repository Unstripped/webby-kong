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

// crypto.subtle exists only in secure contexts: co-op joiners on http://<LAN-IP> hash with sha1Js
async function sha1Hex(bytes) {
  if (!(window.crypto && window.crypto.subtle)) return sha1Js(bytes);
  const digest = await crypto.subtle.digest('SHA-1', bytes);
  return Array.from(new Uint8Array(digest), (x) => x.toString(16).padStart(2, '0')).join('');
}

function sha1Js(input) {
  const data = input instanceof Uint8Array ? input : new Uint8Array(input);
  const n = data.length;
  // int32 arithmetic throughout (sums re-truncated with | 0) keeps V8 on its fast integer path
  let h0 = 0x67452301, h1 = 0xefcdab89 | 0, h2 = 0x98badcfe | 0, h3 = 0x10325476, h4 = 0xc3d2e1f0 | 0;
  const w = new Int32Array(80);
  function block(b, o) {
    for (let i = 0; i < 16; i++, o += 4) w[i] = (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3];
    for (let i = 16; i < 80; i++) {
      const x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
      w[i] = (x << 1) | (x >>> 31);
    }
    let a = h0, bb = h1, c = h2, d = h3, e = h4, t = 0;
    for (let i = 0; i < 20; i++) {
      t = ((((a << 5) | (a >>> 27)) + ((bb & c) | (~bb & d))) | 0) + ((e + 0x5a827999 + w[i]) | 0) | 0;
      e = d; d = c; c = (bb << 30) | (bb >>> 2); bb = a; a = t;
    }
    for (let i = 20; i < 40; i++) {
      t = ((((a << 5) | (a >>> 27)) + (bb ^ c ^ d)) | 0) + ((e + 0x6ed9eba1 + w[i]) | 0) | 0;
      e = d; d = c; c = (bb << 30) | (bb >>> 2); bb = a; a = t;
    }
    for (let i = 40; i < 60; i++) {
      // - 0x70e44324 = + 0x8f1bbcdc (mod 2^32)
      t = ((((a << 5) | (a >>> 27)) + ((bb & c) | (bb & d) | (c & d))) | 0) + ((e - 0x70e44324 + w[i]) | 0) | 0;
      e = d; d = c; c = (bb << 30) | (bb >>> 2); bb = a; a = t;
    }
    for (let i = 60; i < 80; i++) {
      // - 0x359d3e2a = + 0xca62c1d6 (mod 2^32)
      t = ((((a << 5) | (a >>> 27)) + (bb ^ c ^ d)) | 0) + ((e - 0x359d3e2a + w[i]) | 0) | 0;
      e = d; d = c; c = (bb << 30) | (bb >>> 2); bb = a; a = t;
    }
    h0 = (h0 + a) | 0;
    h1 = (h1 + bb) | 0;
    h2 = (h2 + c) | 0;
    h3 = (h3 + d) | 0;
    h4 = (h4 + e) | 0;
  }
  const full = n - (n % 64);
  for (let o = 0; o < full; o += 64) block(data, o);
  const tail = new Uint8Array(n - full < 56 ? 64 : 128);
  tail.set(data.subarray(full));
  tail[n - full] = 0x80;
  const tv = new DataView(tail.buffer);
  tv.setUint32(tail.length - 8, Math.floor(n / 0x20000000)); // bit length, high word
  tv.setUint32(tail.length - 4, (n * 8) >>> 0);
  for (let o = 0; o < tail.length; o += 64) block(tail, o);
  return [h0, h1, h2, h3, h4].map((x) => (x >>> 0).toString(16).padStart(8, '0')).join('');
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
          // saves.js: back up / restore the EEPROM through the save server before main reads it. Co-op joiners
          // skip it: their own save stays as it is and the host's save server never sees it (web/net.js)
          const joiner = window.dk64net && window.dk64net.role() === 'join';
          (window.dk64saves && !joiner ? window.dk64saves.boot(m.FS) : Promise.resolve()).finally(() => m.removeRunDependency('syncfs'));
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
  const resumed = window.dk64qs
    ? window.dk64qs.started().catch((err) => console.warn('quicksave resume failed', err))
    : Promise.resolve();
  // co-op starts once a resumed snapshot is in place (loading a snapshot clears the input ring)
  if (window.dk64net) resumed.then(() => window.dk64net.started());
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

// ?rom=<url> (dev) or the server's mounted ROM
const romUrl = new URLSearchParams(location.search).get('rom');
if (romUrl) {
  setStatus('Fetching ROM…');
  fetch(romUrl).then((r) => {
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.arrayBuffer();
  }).then(start).catch((err) => setStatus(`ROM fetch failed: ${err.message}`));
} else {
  // tools/serve.py --rom (the Docker image); 404 = none mounted: keep the file picker
  fetch('api/rom', { cache: 'no-store' }).then((r) => {
    if (!r.ok) return;
    setStatus('Loading ROM from server…');
    return r.arrayBuffer().then(start);
  }).catch((err) => { console.warn('server ROM fetch failed', err); setStatus('Select your ROM to start.'); });
}
