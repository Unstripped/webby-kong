// Texture dump: while it runs, the port queues every newly drawn texture (port_texdump_*, gfx_texpack.c) with its
// RT64 hash; each is encoded as <hash>.png. Stopping downloads them as a zip with an rt64.json that RT64 and this
// port both load as a pack (auto paths map <hash>.png by name).
import { signal } from '@preact/signals';
import { callNumber, callVoid, requireModule } from '../port';
import { encodePng } from './png';
import { writeZip } from './zip';

export const dumping = signal(false);
export const dumpCount = signal(0);

const pngs = new Map<string, Promise<Uint8Array<ArrayBuffer>>>();
let frame = 0;

function drain(): void {
  const m = requireModule('texture dump');
  for (;;) {
    const ptr = callNumber('port_texdump_take');
    if (ptr === 0) break;
    const v = new DataView(m.HEAPU8.buffer, ptr, 16);
    const hex = v.getUint32(4, true).toString(16).padStart(8, '0') + v.getUint32(0, true).toString(16).padStart(8, '0');
    const width = v.getUint32(8, true);
    const height = v.getUint32(12, true);
    const rgba = m.HEAPU8.slice(ptr + 16, ptr + 16 + width * height * 4);
    callVoid('port_texdump_free', ptr);
    if (!pngs.has(hex)) pngs.set(hex, encodePng(rgba, width, height));
  }
  dumpCount.value = pngs.size;
}

function poll(): void {
  drain();
  frame = requestAnimationFrame(poll);
}

export function startDump(): void {
  pngs.clear();
  callVoid('port_texdump_set', 1);
  dumping.value = true;
  dumpCount.value = 0;
  frame = requestAnimationFrame(poll);
}

// stops the dump and downloads it; the note for the view
export async function stopDump(): Promise<string> {
  cancelAnimationFrame(frame);
  callVoid('port_texdump_set', 0);
  dumping.value = false;
  drain();
  const dropped = callNumber('port_texdump_dropped');
  const hashes = [...pngs.keys()].sort();
  const images = await Promise.all(hashes.map((h) => pngs.get(h)!));
  pngs.clear();
  dumpCount.value = 0;
  if (hashes.length === 0) return 'No textures were drawn while dumping.';
  const db = {
    configuration: {
      autoPath: 'rt64',
      configurationVersion: 3,
      defaultOperation: 'stream',
      defaultShift: 'half',
      hashVersion: 5,
    },
    extraFiles: [],
    operationFilters: [],
    shiftFilters: [],
    textures: hashes.map((rt64) => ({ hashes: { rice: '', rt64 }, path: '' })),
  };
  let zip: Blob;
  try {
    zip = writeZip([
      ...hashes.map((h, i) => ({ name: `${h}.png`, data: images[i]! })),
      { name: 'rt64.json', data: new TextEncoder().encode(JSON.stringify(db, null, 4)) },
    ]);
  } catch (err) {
    return (err as Error).message;
  }
  const d = new Date();
  const two = (n: number) => String(n).padStart(2, '0');
  const a = document.createElement('a');
  a.href = URL.createObjectURL(zip);
  a.download = `dk64-textures-${d.getFullYear()}${two(d.getMonth() + 1)}${two(d.getDate())}-${two(d.getHours())}${two(d.getMinutes())}.zip`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  return `Saved ${hashes.length} textures.` + (dropped > 0 ? ` (${dropped} skipped: too many at once)` : '');
}
