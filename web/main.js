'use strict';
// Loads the user's ROM (drop zone, ?rom= or /api/rom), verifies it, and starts the wasm port. The ROM stays in a JS Uint8Array.
const ROM_SHA1 = 'cf806ff2603640a748fca5026ded28802f1f4a50';
const canvas = document.getElementById('canvas');
let started = false;

// #rom-drop (index.html): the no-ROM screen. Click it or drop a file anywhere; it also shows load progress and errors,
// and is removed once the game runs.
const drop = document.getElementById('rom-drop');
const zone = document.getElementById('rom-zone');
const romInput = document.getElementById('rom');
const titleEl = document.getElementById('rom-title');
const detailEl = document.getElementById('rom-detail');
const IDLE = ['INSERT CARTRIDGE', matchMedia('(hover: none)').matches ? 'Tap to choose your DK64 ROM' : 'Drop your DK64 ROM or click'];
let shown = IDLE;
let dragDepth = 0;

// idle / error take a file; loading / failed don't (failed: the ROM passed but the start failed, which needs a reload,
// as before: `started` is never reset)
function setDrop(state, title, detail) {
  shown = [title, detail];
  drop.hidden = false;
  drop.dataset.state = state;
  drop.classList.remove('over');
  zone.disabled = state === 'loading' || state === 'failed';
  titleEl.textContent = title;
  detailEl.textContent = detail;
}

function setOver(on) {
  drop.classList.toggle('over', on);
  titleEl.textContent = on ? 'RELEASE TO INSERT' : shown[0];
  detailEl.textContent = on ? '.z64 · .n64 · .v64' : shown[1];
}

function accepting() {
  return drop.isConnected && !drop.hidden && !zone.disabled;
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
  setDrop('loading', 'CHECKING ROM…', '');
  const rom = normalizeRom(romBytes);
  if (!rom || (await sha1Hex(rom)) !== ROM_SHA1) {
    setDrop('error', 'WRONG CARTRIDGE', 'Needs Donkey Kong 64 (USA) · try another');
    return;
  }
  started = true;
  setDrop('loading', 'STARTING…', 'Donkey Kong 64 (USA)');
  // saves.js: which save game this page plays (?save=<id>); its EEPROM file is passed as --save-file below
  try {
    await window.dk64saves.select();
  } catch (err) {
    setDrop('failed', 'START FAILED', err.message);
    return;
  }
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
        WebAssembly.instantiate(wasmModule, imports).then(done, (err) => setDrop('failed', 'START FAILED', err.message));
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
      // report.js keeps the last console lines for bug reports; abort() is a crash
      print: (t) => {
        console.log(t);
        if (window.dk64report) window.dk64report.log(t);
      },
      printErr: (t) => {
        console.warn(t);
        if (window.dk64report) window.dk64report.log(t);
      },
      onAbort: (what) => {
        if (window.dk64report) window.dk64report.crashed('Aborted(' + what + ')', new Error().stack);
      },
    });
    module.portBuildId = buildId;
  } catch (err) {
    setDrop('failed', 'START FAILED', err.message);
    return;
  }
  module.portRom = rom;
  window.Module = module;
  if (window.dk64dbg) window.dk64dbg.moduleReady(); // before callMain: applies the saved trace mask from boot
  const args = window.dk64settings.args();
  args.push('--save-file', window.dk64saves.path());
  module.callMain(args);
  window.dk64settings.started();
  if (window.dk64qs) window.dk64qs.started().catch((err) => console.warn('quicksave resume failed', err));
  if (window.dk64touch) window.dk64touch.started();
  if (window.dk64menu) window.dk64menu.started();
  const ctx = module.SDL2 && module.SDL2.audioContext;
  if (ctx && ctx.state !== 'running') ctx.resume();
  canvas.focus();
  drop.remove();
}

async function loadFile(file) {
  await start(await file.arrayBuffer());
}

zone.addEventListener('click', () => romInput.click());
romInput.addEventListener('change', () => {
  const file = romInput.files[0];
  romInput.value = ''; // so picking the same file again after an error fires change again
  if (file) loadFile(file);
});

// A file dragged anywhere over the page goes to the drop zone. Every file drag is preventDefault'ed, also once the
// game runs: otherwise the browser opens the dropped file in place of the game.
function isFileDrag(e) {
  return !!e.dataTransfer && Array.from(e.dataTransfer.types).includes('Files');
}
window.addEventListener('dragenter', (e) => {
  if (!isFileDrag(e)) return;
  e.preventDefault();
  if (++dragDepth === 1 && accepting()) setOver(true);
});
window.addEventListener('dragover', (e) => {
  if (!isFileDrag(e)) return;
  e.preventDefault();
  e.dataTransfer.dropEffect = accepting() ? 'copy' : 'none';
});
window.addEventListener('dragleave', (e) => {
  if (!isFileDrag(e)) return;
  dragDepth = Math.max(0, dragDepth - 1);
  if (dragDepth === 0 && drop.classList.contains('over')) setOver(false);
});
window.addEventListener('drop', (e) => {
  if (!isFileDrag(e)) return;
  e.preventDefault();
  dragDepth = 0;
  if (drop.classList.contains('over')) setOver(false);
  const file = e.dataTransfer.files[0];
  if (file && accepting()) loadFile(file);
});

// resume audio on any later gesture (autoplay policies)
window.addEventListener('keydown', () => {
  const ctx = window.Module && window.Module.SDL2 && window.Module.SDL2.audioContext;
  if (ctx && ctx.state !== 'running') ctx.resume();
});

// ?rom=<url> (dev) or the server's mounted ROM
const romUrl = new URLSearchParams(location.search).get('rom');
if (romUrl) {
  setDrop('loading', 'LOADING ROM…', '');
  fetch(romUrl).then((r) => {
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.arrayBuffer();
  }).then(start).catch((err) => setDrop('error', 'ROM FETCH FAILED', err.message));
} else {
  // tools/serve.py --rom (the Docker image); 404 = none mounted: show the drop zone
  fetch('api/rom', { cache: 'no-store' }).then((r) => {
    if (!r.ok) { setDrop('idle', ...IDLE); return; }
    setDrop('loading', 'LOADING ROM…', '');
    return r.arrayBuffer().then(start);
  }).catch((err) => { console.warn('server ROM fetch failed', err); setDrop('idle', ...IDLE); });
}
