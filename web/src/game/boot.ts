// Loads the user's ROM (drop zone, ?rom= or /api/rom), verifies it, and starts the wasm port. The ROM stays in a JS
// Uint8Array.
import * as dbg from './debug/api';
import { loadDk64 } from './emscripten';
import * as menu from './menu/control';
import { setModule, type Dk64Module } from './port';
import * as qs from './quicksave';
import * as report from './report';
import { accepting, over, removeDrop, setDrop, setOver } from './romdrop';
import * as saves from './saves';
import * as settings from './settings';
import * as texpacks from './texpacks/packs';
import * as touch from './touch/pad';

const ROM_SHA1 = 'cf806ff2603640a748fca5026ded28802f1f4a50';
let started = false;
let idle: [string, string] = ['INSERT CARTRIDGE', 'Drop your DK64 ROM or click'];

const errorMessage = (err: unknown) => (err instanceof Error ? err.message : String(err));

// z64 (big-endian) is native; v64 swaps 16-bit, n64 swaps 32-bit.
export function normalizeRom(bytes: ArrayBuffer | Uint8Array): Uint8Array<ArrayBuffer> | null {
  const b = new Uint8Array(bytes);
  const magic = ((b[0]! << 24) | (b[1]! << 16) | (b[2]! << 8) | b[3]!) >>> 0;
  if (magic === 0x80371240) return b;
  const out = new Uint8Array(b.length);
  if (magic === 0x37804012) {
    for (let i = 0; i + 1 < b.length; i += 2) {
      out[i] = b[i + 1]!;
      out[i + 1] = b[i]!;
    }
    return out;
  }
  if (magic === 0x40123780) {
    for (let i = 0; i + 3 < b.length; i += 4) {
      out[i] = b[i + 3]!;
      out[i + 1] = b[i + 2]!;
      out[i + 2] = b[i + 1]!;
      out[i + 3] = b[i]!;
    }
    return out;
  }
  return null;
}

export async function sha1Hex(bytes: BufferSource): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-1', bytes);
  return Array.from(new Uint8Array(digest), (x) => x.toString(16).padStart(2, '0')).join('');
}

export async function start(romBytes: ArrayBuffer): Promise<void> {
  if (started) return;
  setDrop('loading', 'CHECKING ROM…', '');
  const rom = normalizeRom(romBytes);
  if (!rom || (await sha1Hex(rom)) !== ROM_SHA1) {
    setDrop('error', 'WRONG CARTRIDGE', 'Needs Donkey Kong 64 (USA) · try another');
    return;
  }
  started = true;
  setDrop('loading', 'STARTING…', 'Donkey Kong 64 (USA)');
  // saves.ts: which save game this page plays (?save=<id>); its EEPROM file is passed as --save-file below
  try {
    await saves.select();
  } catch (err) {
    setDrop('failed', 'START FAILED', errorMessage(err));
    return;
  }
  let module: Dk64Module;
  try {
    const createDk64 = await loadDk64();
    // the build id names the exact wasm this page runs: the same bytes are instantiated (quicksave.ts)
    const wasmBytes = await fetch('dk64.wasm', { cache: 'no-cache' }).then((r) => {
      if (!r.ok) throw new Error(`dk64.wasm HTTP ${r.status}`);
      return r.arrayBuffer();
    });
    const buildId = await sha1Hex(wasmBytes);
    const wasmModule = await WebAssembly.compile(wasmBytes);
    module = await createDk64({
      canvas: document.getElementById('canvas') as HTMLCanvasElement,
      // instantiate the hashed bytes (Module.wasmBinary is not in this build's INCOMING_MODULE_JS_API)
      instantiateWasm: (imports, done) => {
        WebAssembly.instantiate(wasmModule, imports).then(done, (err: Error) =>
          setDrop('failed', 'START FAILED', err.message),
        );
        return {};
      },
      preRun: [
        (m) => {
          m.FS.mkdir('/saves');
          m.FS.mount(m.IDBFS, {}, '/saves');
          m.addRunDependency('syncfs');
          m.FS.syncfs(true, (err) => {
            if (err) console.warn('save load failed', err);
            // saves.ts: back up / restore the EEPROM through the save server before main reads it
            saves.boot(m.FS).finally(() => m.removeRunDependency('syncfs'));
          });
        },
      ],
      // report.ts keeps the last console lines for bug reports; abort() is a crash
      print: (t) => {
        console.log(t);
        report.log(t);
      },
      printErr: (t) => {
        console.warn(t);
        report.log(t);
      },
      onAbort: (what) => {
        report.crashed('Aborted(' + String(what) + ')', new Error().stack);
      },
    });
    module.portBuildId = buildId;
  } catch (err) {
    setDrop('failed', 'START FAILED', errorMessage(err));
    return;
  }
  module.portRom = rom;
  setModule(module);
  window.Module = module;
  dbg.moduleReady(); // before callMain: applies the saved trace mask from boot
  module.callMain([...settings.args(), '--save-file', saves.path()]);
  settings.started();
  qs.started().catch((err) => console.warn('quicksave resume failed', err));
  touch.started();
  menu.started();
  texpacks.started().catch((err) => console.warn('texture packs failed', err));
  const ctx = module.SDL2 && module.SDL2.audioContext;
  if (ctx && ctx.state !== 'running') void ctx.resume();
  document.getElementById('canvas')!.focus();
  removeDrop();
}

export async function loadFile(file: File): Promise<void> {
  await start(await file.arrayBuffer());
}

// A file dragged anywhere over the page goes to the drop zone. Every file drag is preventDefault'ed, also once the
// game runs: otherwise the browser opens the dropped file in place of the game.
function isFileDrag(e: DragEvent): e is DragEvent & { dataTransfer: DataTransfer } {
  return !!e.dataTransfer && Array.from(e.dataTransfer.types).includes('Files');
}

export function installBoot(): void {
  idle = ['INSERT CARTRIDGE', matchMedia('(hover: none)').matches ? 'Tap to choose your DK64 ROM' : 'Drop your DK64 ROM or click'];
  let dragDepth = 0;
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
    if (dragDepth === 0 && over.value) setOver(false);
  });
  window.addEventListener('drop', (e) => {
    if (!isFileDrag(e)) return;
    e.preventDefault();
    dragDepth = 0;
    if (over.value) setOver(false);
    const file = e.dataTransfer.files[0];
    if (file && accepting()) void loadFile(file);
  });

  // resume audio on any later gesture (autoplay policies)
  window.addEventListener('keydown', () => {
    const ctx = window.Module?.SDL2?.audioContext;
    if (ctx && ctx.state !== 'running') void ctx.resume();
  });

  // ?rom=<url> (dev) or the server's mounted ROM
  const romUrl = new URLSearchParams(location.search).get('rom');
  if (romUrl) {
    setDrop('loading', 'LOADING ROM…', '');
    fetch(romUrl)
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        return r.arrayBuffer();
      })
      .then(start)
      .catch((err) => setDrop('error', 'ROM FETCH FAILED', errorMessage(err)));
  } else {
    // tools/serve.py --rom (the Docker image); 404 = none mounted: show the drop zone
    fetch('api/rom', { cache: 'no-store' })
      .then((r) => {
        if (!r.ok) {
          setDrop('idle', ...idle);
          return;
        }
        setDrop('loading', 'LOADING ROM…', '');
        return r.arrayBuffer().then(start);
      })
      .catch((err) => {
        console.warn('server ROM fetch failed', err);
        setDrop('idle', ...idle);
      });
  }
}
