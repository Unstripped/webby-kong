// Texture packs at run time: the imported packs (store.ts), each resolved (rt64db.ts) and installed into the port as
// hash records (port_texpack_set, port/src/gfx/gfx_texpack.c). The port asks for a texture the first time a draw
// needs it (Module.portTexpackRequest); preload/stall entries are asked for up front. Files are decoded in a worker
// (decode.worker.ts) and uploaded with port_texpack_upload_bitmap / port_texpack_upload. Every mutation (import,
// on/off, order, removal) is persisted, then the whole pack set is installed again.
import { signal } from '@preact/signals';
import { callNumber, callVoid, requireModule } from '../port';
import { TEX_BC7, TEX_RGBA8 } from './dds';
import type { DecodeRequest, DecodeResult } from './decode';
import { parseDatabase, resolve } from './rt64db';
import { findRoot, openSource, type Locator, type PackSource, type StoredSource } from './source';
import { deletePack, listPacks, putPacks, type PackRecord } from './store';
import { listZip, readZipEntry } from './zip';

export interface PackInfo {
  id: string;
  name: string;
  enabled: boolean;
  total: number; // textures the pack's rt64.json lists
  resolved: number; // of those, with a file in the pack
  loaded: number;
  failed: number;
  error: string | null;
}

// all imported packs, top (highest priority) first
export const packList = signal<PackInfo[]>([]);

interface Entry {
  pack: number; // index into records / infos
  path: string;
  locator: Locator;
}

const MAX_IN_FLIGHT = 4;
const FORMAT_S3TC = 2;
const FORMAT_BPTC = 4;

let records: PackRecord[] = [];
let infos: PackInfo[] = [];
let entries: Entry[] = [];
let generation = 0;
let formats = 1; // port_texpack_formats
let chain: Promise<unknown> = Promise.resolve();

const queue: number[] = [];
const queued = new Set<number>(); // queued or in flight this generation
const requests = new Map<number, { entry: number; gen: number }>();
let nextRequest = 1;
let worker: Worker | null = null;
let publishTimer: ReturnType<typeof setTimeout> | null = null;

const errorMessage = (err: unknown) => (err instanceof Error ? err.message : String(err));

// one mutation at a time: each sees the records the previous one left
function serial<T>(fn: () => Promise<T>): Promise<T> {
  const run = chain.then(fn);
  chain = run.catch(() => undefined);
  return run;
}

function publish(): void {
  clearTimeout(publishTimer ?? undefined);
  publishTimer = null;
  packList.value = infos.map((i) => ({ ...i }));
}

function publishSoon(): void {
  publishTimer ??= setTimeout(publish, 250);
}

async function readDatabase(src: PackSource) {
  const db = parseDatabase(new TextDecoder().decode(await src.read('rt64.json')));
  return resolve(db, src.paths());
}

// installs the enabled packs into the port, replacing what was there
async function apply(): Promise<void> {
  const m = requireModule('texture packs');
  generation++;
  queue.length = 0;
  queued.clear();
  callNumber('port_texpack_set', 0, 0, 0);
  entries = [];
  infos = records.map((r) => ({
    id: r.id,
    name: r.name,
    enabled: r.enabled,
    total: 0,
    resolved: 0,
    loaded: 0,
    failed: 0,
    error: null,
  }));
  const keyed = new Map<string, { hash: bigint; entry: number; version: number; shiftHalf: boolean; priority: number }>();
  const preload: number[] = [];
  let priority = 0;
  for (const [pack, r] of records.entries()) {
    if (!r.enabled) continue;
    const info = infos[pack]!;
    const p = priority++;
    try {
      const src = await openSource(r.source);
      const res = await readDatabase(src);
      if (res.hashVersion > 5) throw new Error(`Needs a newer RT64 (hash version ${res.hashVersion})`);
      const version = Math.max(1, res.hashVersion);
      info.total = res.total;
      info.resolved = res.textures.length;
      for (const t of res.textures) {
        const key = `${version}:${t.hash}`;
        if (keyed.has(key)) continue; // a higher pack replaces this texture
        const entry = entries.length;
        entries.push({ pack, path: t.path, locator: src.locate(t.path)! });
        keyed.set(key, { hash: t.hash, entry, version, shiftHalf: t.shift === 'half', priority: p });
        // the GL backend cannot wait for a file: stall loads like preload
        if (t.operation !== 'stream') preload.push(entry);
      }
    } catch (err) {
      info.error = errorMessage(err);
    }
  }
  if (entries.length > 0) {
    const n = keyed.size;
    const ptr = callNumber('port_texpack_alloc', n * 16);
    let ok = ptr !== 0;
    if (ok) {
      const v = new DataView(m.HEAPU8.buffer, ptr, n * 16); // after the alloc: memory may have grown
      let o = 0;
      for (const r of keyed.values()) {
        v.setUint32(o, Number(r.hash & 0xffffffffn), true);
        v.setUint32(o + 4, Number(r.hash >> 32n), true);
        v.setUint32(o + 8, r.entry, true);
        v.setUint8(o + 12, r.version);
        v.setUint8(o + 13, r.shiftHalf ? 1 : 0);
        v.setUint16(o + 14, r.priority, true);
        o += 16;
      }
      ok = callNumber('port_texpack_set', entries.length, ptr, n) !== 0;
    }
    if (!ok) {
      entries = [];
      for (const i of infos) if (i.enabled && !i.error) i.error = 'Out of memory for the pack set';
    }
    for (const e of ok ? preload : []) enqueue(e);
  }
  publish();
}

function enqueue(entry: number): void {
  if (queued.has(entry) || entry >= entries.length) return;
  queued.add(entry);
  queue.push(entry);
  // asked for during a draw: post to the worker once the frame's JS returns
  queueMicrotask(pump);
}

function decoder(): Worker {
  if (!worker) {
    worker = new Worker(new URL('./decode.worker.ts', import.meta.url), { type: 'module' });
    worker.onmessage = (e: MessageEvent<DecodeResult>) => received(e.data);
    worker.onerror = (e) => console.warn('texture pack decoder failed', e.message);
  }
  return worker;
}

function pump(): void {
  while (requests.size < MAX_IN_FLIGHT && queue.length > 0) {
    const entry = queue.shift()!;
    const { blob, entry: zipEntry } = entries[entry]!.locator;
    const id = nextRequest++;
    requests.set(id, { entry, gen: generation });
    const req: DecodeRequest = { id, blob, entry: zipEntry };
    decoder().postMessage(req);
  }
}

function fail(info: PackInfo, entry: number, error: string): void {
  callVoid('port_texpack_fail', entry);
  info.failed++;
  info.error ??= error;
}

function received(r: DecodeResult): void {
  const req = requests.get(r.id);
  requests.delete(r.id);
  if (!req || req.gen !== generation) {
    if (r.kind === 'bitmap') r.bitmap.close();
    pump();
    return;
  }
  const { entry } = req;
  const e = entries[entry]!;
  const info = infos[e.pack]!;
  const m = requireModule('texture packs');
  let ok = false;
  if (r.kind === 'bitmap') {
    m.portTexpackBitmap = r.bitmap;
    ok = callNumber('port_texpack_upload_bitmap', entry, r.bitmap.width, r.bitmap.height) !== 0;
    m.portTexpackBitmap = null;
    r.bitmap.close();
  } else if (r.kind === 'dds') {
    const needs = r.format === TEX_RGBA8 ? 0 : r.format === TEX_BC7 ? FORMAT_BPTC : FORMAT_S3TC;
    if (needs && !(formats & needs)) {
      fail(
        info,
        entry,
        needs === FORMAT_BPTC
          ? 'BC7 (DDS) textures need EXT_texture_compression_bptc, which this browser lacks'
          : 'BC1–BC3 (DDS) textures need WEBGL_compressed_texture_s3tc, which this browser lacks',
      );
      publishSoon();
      pump();
      return;
    }
    const ptr = callNumber('port_texpack_alloc', r.data.byteLength);
    if (ptr !== 0) {
      m.HEAPU8.set(new Uint8Array(r.data), ptr);
      ok = callNumber('port_texpack_upload', entry, r.format, r.width, r.height, r.levels, ptr, r.data.byteLength) !== 0;
    }
  } else {
    fail(info, entry, `${e.path}: ${r.message}`);
    publishSoon();
    pump();
    return;
  }
  if (ok) {
    info.loaded++;
  } else {
    info.failed++;
    info.error ??= 'GPU upload failed';
  }
  publishSoon();
  pump();
}

// after callMain: installs the stored packs
export async function started(): Promise<void> {
  const m = requireModule('texture packs');
  formats = callNumber('port_texpack_formats');
  m.portTexpackRequest = (entry) => enqueue(entry);
  await serial(async () => {
    records = await listPacks();
    await apply();
  });
}

// a pack's name: its mod.json display_name (DK64 Rekongpiled mods), else `fallback`
async function displayName(src: PackSource, fallback: string): Promise<string> {
  if (src.locate('mod.json')) {
    try {
      const mod: unknown = JSON.parse(new TextDecoder().decode(await src.read('mod.json')));
      const name = (mod as { display_name?: unknown } | null)?.display_name;
      if (typeof name === 'string' && name.trim().length >= 1 && name.trim().length <= 80) return name.trim();
    } catch {
      // no usable mod.json: the file name
    }
  }
  return fallback;
}

// stores a new pack on top, enabled, and installs the set; the note for the view
async function add(source: StoredSource, label: string, fallbackName: string): Promise<string> {
  const src = await openSource(source);
  let res;
  try {
    res = await readDatabase(src);
  } catch (err) {
    return `${label}: ${errorMessage(err)}`;
  }
  if (res.hashVersion > 5) return `${label} needs a newer RT64 (hash version ${res.hashVersion}).`;
  const name = await displayName(src, fallbackName);
  const rec: PackRecord = {
    id: crypto.randomUUID(),
    name,
    enabled: true,
    order: records.reduce((min, r) => Math.min(min, r.order), 0) - 1,
    added: new Date().toISOString(),
    source,
  };
  try {
    await putPacks([rec]);
  } catch (err) {
    return `Couldn’t store the pack (${errorMessage(err)}).`;
  }
  records = [rec, ...records];
  await apply();
  return `Added “${name}”: ${res.textures.length} textures.`;
}

// an .rtz / .zip with rt64.json at its root or in one folder, or an archive holding exactly one such archive (DK64
// Rekongpiled releases: a .zip around the mod's .nrm)
export function importArchive(file: File): Promise<string> {
  return serial(async () => {
    let entries;
    try {
      entries = await listZip(file);
    } catch {
      return `${file.name} isn’t a zip, .rtz or .nrm archive.`;
    }
    try {
      let blob: Blob = file;
      let root = findRoot(entries.map((e) => e.name));
      const inner = entries.filter((e) => /\.(rtz|nrm|zip)$/i.test(e.name));
      if (root === null && inner.length === 1) {
        blob = new Blob([await readZipEntry(file, inner[0]!)]);
        root = findRoot((await listZip(blob)).map((e) => e.name));
      }
      if (root === null) return `${file.name} has no rt64.json: not an RT64 texture pack.`;
      return await add({ kind: 'zip', blob, prefix: root }, file.name, file.name.replace(/\.[^.]*$/, ''));
    } catch (err) {
      return `${file.name}: ${errorMessage(err)}`;
    }
  });
}

// a folder picked with webkitdirectory: its rt64.json, mod.json and images
export function importFolder(files: FileList): Promise<string> {
  return serial(async () => {
    const all = [...files].map((f) => ({ path: f.webkitRelativePath.split('/').slice(1).join('/'), blob: f as Blob }));
    const folder = files[0]?.webkitRelativePath.split('/')[0] || 'The folder';
    const root = findRoot(all.map((f) => f.path));
    if (root === null) return `${folder} has no rt64.json: not an RT64 texture pack.`;
    const kept = all
      .filter((f) => f.path.startsWith(root))
      .map((f) => ({ path: f.path.slice(root.length), blob: f.blob }))
      .filter((f) => /^(rt64|mod)\.json$/i.test(f.path) || /\.(png|dds)$/i.test(f.path));
    try {
      return await add({ kind: 'dir', files: kept }, folder, folder);
    } catch (err) {
      return `${folder}: ${errorMessage(err)}`;
    }
  });
}

export function setEnabled(id: string, on: boolean): Promise<void> {
  return serial(async () => {
    const r = records.find((x) => x.id === id);
    if (!r || r.enabled === on) return;
    r.enabled = on;
    await putPacks([r]);
    await apply();
  });
}

// one place up (-1) or down (1)
export function move(id: string, dir: -1 | 1): Promise<void> {
  return serial(async () => {
    const i = records.findIndex((x) => x.id === id);
    const a = records[i];
    const b = records[i + dir];
    if (!a || !b) return;
    [a.order, b.order] = [b.order, a.order];
    records.sort((x, y) => x.order - y.order);
    await putPacks([a, b]);
    await apply();
  });
}

export function remove(id: string): Promise<void> {
  return serial(async () => {
    if (!records.some((x) => x.id === id)) return;
    await deletePack(id);
    records = records.filter((x) => x.id !== id);
    await apply();
  });
}
