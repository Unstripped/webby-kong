// Quicksave / quickload (F5 / F9, wired in debug/api.ts); also window.dk64qs.
// - Snapshot: RDRAM + the port/game .data/.bss (port/src/qs.c), exact, valid only for the identical dk64.wasm
//   (Module.portBuildId = SHA-1 of the wasm boot.ts instantiated). Kept in IndexedDB, so it survives a reload.
// - Resume point: save slot, kong, map, exit, position. After a reload of any build the game boots straight
//   into it once the EEPROM is read (src/global/450.c), skipping logos, title and file select.
// Both are kept per save game (saves.scope()). The server's resume point from the Esc menu's "Exit to dashboard"
// (saves.takeResume()) applies regardless of the auto-resume toggle; the newer of it and the local one wins,
// and a same-build snapshot only when it is newer than the server's point.
import { currentModule, requireModule, type Dk64Module } from './port';
import * as saves from './saves';
import type { ResumePoint } from './saves';
import { toast } from './ui/toast';

const DB_NAME = 'dk64dbg';
const STORE = 'snapshots';
const key = () => 'quick' + saves.scope();
const resumeKey = () => 'dk64dbg.resume' + saves.scope();
const AUTO_KEY = 'dk64dbg.autoResume';
const RESUME_ONCE = 'dk64dbg.resumeOnce';
const SKIP_SNAPSHOT = 'dk64dbg.skipSnapshot';

export interface Snapshot {
  buildId: string;
  time: string;
  map: number;
  bytes: Uint8Array;
}

// a resume point with the time it was taken (localStorage, or the server's from "Exit to dashboard")
export type ResumeRecord = ResumePoint & { ok?: boolean; time?: string };

let snap: Snapshot | null = null;

const mod = (): Dk64Module => requireModule('dk64qs');

function hex(n: number): string {
  return (n >>> 0).toString(16);
}

function idbOpen(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(STORE);
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function idbGet(): Promise<Snapshot | null> {
  const db = await idbOpen();
  try {
    return await new Promise<Snapshot | null>((resolve, reject) => {
      const req = db.transaction(STORE, 'readonly').objectStore(STORE).get(key());
      req.onsuccess = () => resolve((req.result as Snapshot | undefined) || null);
      req.onerror = () => reject(req.error);
    });
  } finally {
    db.close();
  }
}

async function idbWrite(write: (store: IDBObjectStore) => void): Promise<void> {
  const db = await idbOpen();
  try {
    await new Promise<void>((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      write(tx.objectStore(STORE));
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error);
    });
  } finally {
    db.close();
  }
}

// [[start, length] x3]: RDRAM, .data, .bss (port_qs_table)
function ranges(m: Dk64Module): Array<[number, number]> {
  const t = (m.ccall('port_qs_table', 'number') as number) >>> 2;
  return [0, 2, 4].map((k) => [m.HEAPU32[t + k]!, m.HEAPU32[t + k + 1]!]);
}

function currentMap(m: Dk64Module): number {
  const stats: { map: number } = JSON.parse(m.ccall('port_debug_stats', 'string') as string);
  return stats.map;
}

function readResume(): ResumeRecord | null {
  try {
    const s = localStorage.getItem(resumeKey());
    return s ? (JSON.parse(s) as ResumeRecord) : null;
  } catch {
    return null;
  }
}

export function autoResume(on?: boolean): boolean {
  if (on === undefined) {
    try {
      return localStorage.getItem(AUTO_KEY) !== '0';
    } catch {
      return true;
    }
  }
  localStorage.setItem(AUTO_KEY, on ? '1' : '0');
  return !!on;
}

export async function save(): Promise<void> {
  const m = mod();
  const res = JSON.parse(m.ccall('port_resume_capture', 'string') as string) as ResumeRecord & { reason?: string };
  if (!m.ccall('port_qs_save_begin', 'number')) {
    throw new Error('not at a frame boundary');
  }
  let bytes: Uint8Array;
  try {
    const r = ranges(m);
    bytes = new Uint8Array(r.reduce((a, [, n]) => a + n, 0));
    let o = 0;
    for (const [s, n] of r) {
      bytes.set(m.HEAPU8.subarray(s, s + n), o);
      o += n;
    }
  } finally {
    m.ccall('port_qs_save_end', null);
  }
  const taken: Snapshot = { buildId: m.portBuildId, time: new Date().toISOString(), map: currentMap(m), bytes };
  snap = taken;
  if (res.ok) {
    localStorage.setItem(resumeKey(), JSON.stringify({ ...res, time: taken.time }));
  }
  const noResume = res.ok ? '' : ` · no resume point: ${res.reason}`;
  try {
    await idbWrite((store) => store.put(taken, key()));
  } catch (err) {
    toast(`Quicksaved (not persisted: ${err && (err as Error).message})${noResume}`);
    return;
  }
  toast(`Quicksaved · map 0x${hex(taken.map)}${noResume}`);
}

function restore(rec: Snapshot): boolean {
  const m = mod();
  if (rec.bytes.length !== (m.ccall('port_qs_size', 'number') as number) >>> 0) {
    throw new Error('snapshot size mismatch');
  }
  const stash = m.ccall('port_qs_load_begin', 'number') as number;
  if (!stash) {
    throw new Error('not at a frame boundary');
  }
  let o = 0;
  for (const [s, n] of ranges(m)) {
    m.HEAPU8.set(rec.bytes.subarray(o, o + n), s);
    o += n;
  }
  if (!m.ccall('port_qs_load_end', 'number', ['number'], [stash])) {
    // memory is already overwritten: the snapshot's threads cannot resume in this page
    sessionStorage.setItem(SKIP_SNAPSHOT, '1');
    sessionStorage.setItem(RESUME_ONCE, '1');
    toast('Quickload failed; reloading');
    setTimeout(() => location.reload(), 800);
    return false;
  }
  return true;
}

export async function load(): Promise<void> {
  const m = mod();
  if (!snap) snap = await idbGet().catch(() => null);
  if (snap && snap.buildId === m.portBuildId) {
    if (restore(snap)) toast(`Quickloaded · map 0x${hex(snap.map)}`);
  } else if (readResume()) {
    sessionStorage.setItem(RESUME_ONCE, '1');
    toast('Snapshot is from another build; reloading to resume');
    setTimeout(() => location.reload(), 600);
  } else {
    toast('Nothing to load');
  }
}

// report.ts load(): reload into a resume point (or none) with settings URL params for that load only. Skips the
// same-build snapshot so it cannot override the imported state.
export function bootInto(resume: ResumePoint | null, params?: Record<string, unknown>): void {
  if (resume) {
    localStorage.setItem(resumeKey(), JSON.stringify({ ok: true, ...resume, time: new Date().toISOString() }));
  } else {
    localStorage.removeItem(resumeKey());
  }
  sessionStorage.setItem(SKIP_SNAPSHOT, '1');
  sessionStorage.setItem(RESUME_ONCE, '1');
  const u = new URL(location.href);
  u.searchParams.delete('noresume');
  for (const [k, v] of Object.entries(params || {})) u.searchParams.set(k, String(v));
  location.replace(u.href);
}

// The Esc menu's Import save: drops this save game's snapshot. It holds the replaced save in RDRAM (sEepromImage)
// and port .bss (gPortEeprom), so restoring it later would bring the old progress back.
export async function forget(): Promise<void> {
  snap = null;
  await idbWrite((store) => store.delete(key()));
}

function newest(a: ResumeRecord | null, b: ResumeRecord | null): ResumeRecord | null {
  if (!a) return b;
  if (!b) return a;
  return String(b.time || '') > String(a.time || '') ? b : a;
}

export type ResumeChoice = { kind: 'snapshot' } | { kind: 'resume'; record: ResumeRecord } | { kind: 'none' };

// started()'s decision: a same-build snapshot unless the server's point is newer, else the newest resume point
export function pickResume(opts: {
  auto: boolean;
  local: ResumeRecord | null;
  server: ResumeRecord | null;
  snapshot: Snapshot | null;
  buildId: string;
}): ResumeChoice {
  const { auto, local, server, snapshot, buildId } = opts;
  if (auto && snapshot && snapshot.buildId === buildId && !(server && String(server.time) > String(snapshot.time))) {
    return { kind: 'snapshot' };
  }
  const record = newest(auto ? local : null, server);
  return record ? { kind: 'resume', record } : { kind: 'none' };
}

// boot.ts, right after callMain
export async function started(): Promise<void> {
  const once = sessionStorage.getItem(RESUME_ONCE) === '1';
  const skipSnapshot = sessionStorage.getItem(SKIP_SNAPSHOT) === '1';
  sessionStorage.removeItem(RESUME_ONCE);
  sessionStorage.removeItem(SKIP_SNAPSHOT);
  const m = mod();
  const rec = skipSnapshot ? null : await idbGet().catch(() => null);
  if (new URLSearchParams(location.search).has('noresume')) {
    snap = rec;
    return;
  }
  const server = skipSnapshot ? null : saves.takeResume();
  const auto = once || autoResume();
  const choice = pickResume({
    auto,
    local: auto ? readResume() : null,
    server,
    snapshot: rec,
    buildId: m.portBuildId,
  });
  snap = rec;
  if (choice.kind === 'snapshot' && rec) {
    if (restore(rec)) toast(`Resumed snapshot · map 0x${hex(rec.map)}`);
    return;
  }
  if (choice.kind === 'resume') {
    const r = choice.record;
    m.ccall('port_resume_request', null, Array<'number'>(9).fill('number'),
      [r.slot, r.kong, r.map, r.exit, r.hasPos, r.x, r.y, r.z, r.yaw]);
    toast(`Resuming at map 0x${hex(r.map)} after boot`);
  }
}

export function status(): {
  snapshot: { time: string; map: number; sameBuild: boolean } | null;
  resume: ResumeRecord | null;
  auto: boolean;
} {
  const id = currentModule()?.portBuildId;
  return {
    snapshot: snap ? { time: snap.time, map: snap.map, sameBuild: snap.buildId === id } : null,
    resume: readResume(),
    auto: autoResume(),
  };
}
