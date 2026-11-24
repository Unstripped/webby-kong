// Save games on the save server (tools/serve.py, tools/saves.py); also window.dk64saves (port/src/save.c calls upload).
// - select(): before the module is created, picks this page's save game: ?save=<id>, else the most recently played
//   one (or a new one), written back into the URL. The EEPROM lives at path() (/saves/s<id>.eep in IDBFS), passed to
//   the game as --save-file; scope() keys quicksave.ts's per-save storage.
// - boot(FS): before main reads path(), uploads the browser copy (kept as a version), then restores the server's
//   newest version (highest client mtime) into IDBFS if it differs. Never rejects.
// - upload(FS, summary): called from port_save_sync (port/src/save.c) after each EEPROM flush, with the game's
//   summary JSON (or '') for the dashboard.
// - settle() / putResume() / takeResume(): the Esc menu's "Exit to dashboard" and quicksave.ts's resume point.
// - localBytes(FS): this save game's EEPROM file in IDBFS (the Esc menu's Export save).
// - replace(m, bytes): overwrites this save game's EEPROM file in place (report.ts load, the Esc menu's Import save);
//   the caller reloads, and boot() then uploads it as the newest version.
// Without the server (plain http.server answers 501/404) the game runs on /saves/dk64.eep in IDBFS alone and toasts a
// warning; there is no dashboard then.
import type { Dk64Module, EmscriptenFS } from './port';
import { toast as showToast } from './ui/toast';

const SIZE = 2048;
const API = 'api/saves';
const TIMEOUT_MS = 5000;
const JSON_HEADERS = { 'Content-Type': 'application/json' };

// the resume point fields the server stores (port_resume_capture without `ok`/`reason`)
export interface ResumePoint {
  slot: number;
  kong: number;
  map: number;
  exit: number;
  hasPos: number;
  x: number;
  y: number;
  z: number;
  yaw: number;
  time?: string;
}

export interface Save {
  id: number;
  name?: string;
  resume: ResumePoint | null;
  [key: string]: unknown;
}

interface Upload {
  time: number;
  ok: boolean;
}

const state: { server: 'unknown' | 'ok' | 'unavailable'; lastUpload: Upload | null } = {
  server: 'unknown',
  lastUpload: null,
};
let current: Save | null = null; // the server's save object (tools/saves.py save_json) or null: offline
let isDashboard = false;
const inflight = new Set<Promise<void>>();

class Fatal extends Error {}

function toast(msg: string): void {
  showToast(msg);
  console.warn(msg);
}

export function path(): string {
  return current ? '/saves/s' + current.id + '.eep' : '/saves/dk64.eep';
}

export function scope(): string {
  return current ? '.' + current.id : '';
}

function saveUrl(save: Save, suffix = ''): string {
  return API + '/' + save.id + suffix;
}

async function call(url: string, init?: RequestInit): Promise<Response> {
  const r = await fetch(url, { cache: 'no-store', signal: AbortSignal.timeout(TIMEOUT_MS), ...init });
  if (r.status === 401) {
    location.assign('/auth/?next=' + encodeURIComponent(location.pathname + location.search));
    throw new Fatal('Signing in…');
  }
  return r;
}

async function json<T>(r: Response): Promise<T> {
  let data: unknown = null;
  try {
    data = await r.json();
  } catch {
    data = null;
  }
  if (!r.ok || data === null || typeof data !== 'object') throw new Error('save server HTTP ' + r.status);
  return data as T;
}

export async function select(): Promise<Save | null> {
  const id = new URLSearchParams(location.search).get('save');
  try {
    let save: Save | undefined;
    if (id !== null) {
      const r = await call(API + '/' + encodeURIComponent(id));
      if (r.status === 404) {
        const body = (await r.json().catch(() => null)) as { error?: string } | null;
        if (body && body.error === 'not_found') {
          throw new Fatal("This save doesn't exist or isn't yours. Pick one on the dashboard.");
        }
      }
      const data = await json<{ save: Save; dashboard?: boolean }>(r);
      save = data.save;
      isDashboard = !!data.dashboard;
    } else {
      const data = await json<{ saves: Save[]; dashboard?: boolean }>(await call(API));
      isDashboard = !!data.dashboard;
      save = data.saves[0];
      if (!save) {
        save = (await json<{ save: Save }>(await call(API, { method: 'POST', headers: JSON_HEADERS, body: '{}' }))).save;
      }
      const u = new URL(location.href);
      u.searchParams.set('save', String(save.id));
      history.replaceState(null, '', u.href);
    }
    current = save;
    state.server = 'ok';
    return current;
  } catch (err) {
    if (err instanceof Fatal) throw err;
    current = null;
    isDashboard = false;
    state.server = 'unavailable';
    toast('Save server unavailable (' + (err as Error).message + '): progress is only in this browser');
    return null;
  }
}

function readLocal(FS: EmscriptenFS): { bytes: Uint8Array<ArrayBuffer>; mtime: number } | null {
  try {
    return { bytes: FS.readFile(path()), mtime: Math.round(+FS.stat(path()).mtime) };
  } catch {
    return null;
  }
}

// Export save: null when there is no file yet (the game never saved this save game)
export function localBytes(FS: EmscriptenFS): Uint8Array<ArrayBuffer> | null {
  const local = readLocal(FS);
  return local ? local.bytes : null;
}

// Pauses the game and writes `bytes` as this save game's EEPROM file with the newest mtime, so boot() uploads it
// after the reload and the server serves it as latest. Flushing first leaves the save clean, so neither a pending
// write nor the visibilitychange flush (port/src/save.c) can overwrite it.
export async function replace(m: Dk64Module, bytes: Uint8Array): Promise<void> {
  m.ccall('port_set_paused', null, ['number'], [1]);
  m.ccall('port_save_flush_now', null);
  m.FS.writeFile(path(), bytes);
  const t = Date.now();
  m.FS.utime(path(), t, t);
  await new Promise<void>((resolve, reject) => m.FS.syncfs(false, (err) => (err ? reject(err) : resolve())));
}

async function put(save: Save, bytes: Uint8Array<ArrayBuffer>, mtime: number, keepalive: boolean, summary?: string) {
  const headers: Record<string, string> = { 'X-Save-Mtime': String(mtime) };
  if (summary) headers['X-Save-Summary'] = summary;
  const r = await fetch(saveUrl(save, '/eeprom'), {
    method: 'PUT',
    body: bytes,
    headers,
    keepalive,
    signal: AbortSignal.timeout(TIMEOUT_MS),
  });
  if (!r.ok) throw new Error('save server HTTP ' + r.status);
}

function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}

export async function boot(FS: EmscriptenFS): Promise<void> {
  const save = current;
  if (!save) return;
  try {
    const local = readLocal(FS);
    if (local) await put(save, local.bytes, local.mtime, false);
    const r = await fetch(saveUrl(save, '/eeprom'), { cache: 'no-store', signal: AbortSignal.timeout(TIMEOUT_MS) });
    if (r.status === 404) {
      state.server = 'ok';
      return;
    }
    if (!r.ok) throw new Error('save server HTTP ' + r.status);
    const bytes = new Uint8Array(await r.arrayBuffer());
    const mtime = Number(r.headers.get('X-Save-Mtime'));
    if (bytes.length !== SIZE) throw new Error('save server sent ' + bytes.length + ' bytes');
    if (!local || !sameBytes(local.bytes, bytes)) {
      FS.writeFile(path(), bytes);
      FS.utime(path(), mtime, mtime);
      FS.syncfs(false, (err) => err && console.warn('save sync failed', err));
      console.log('PORT: save restored from server (' + r.headers.get('X-Save-Sha1') + ')');
    }
    state.server = 'ok';
  } catch (err) {
    state.server = 'unavailable';
    toast('Save server unavailable (' + (err as Error).message + '): progress is only in this browser');
  }
}

export function upload(FS: EmscriptenFS, summary: string): Promise<void> {
  const save = current;
  if (!save) return Promise.resolve();
  const local = readLocal(FS);
  if (!local) return Promise.resolve();
  const p = put(save, local.bytes, local.mtime, true, summary).then(
    () => {
      state.lastUpload = { time: Date.now(), ok: true };
    },
    (err: Error) => {
      state.lastUpload = { time: Date.now(), ok: false };
      state.server = 'unavailable';
      toast('Save upload failed (' + err.message + '): progress is only in this browser');
    },
  );
  inflight.add(p);
  p.finally(() => inflight.delete(p));
  return p;
}

// true when the server holds this browser's latest save: every upload settled and the last one succeeded
export async function settle(): Promise<boolean> {
  await Promise.allSettled([...inflight]);
  if (current === null) return false;
  return state.lastUpload !== null ? state.lastUpload.ok : state.server === 'ok';
}

export async function putResume(resume: ResumePoint | null): Promise<void> {
  const save = current;
  if (!save) return;
  const r = await fetch(saveUrl(save, '/resume'), {
    method: 'PUT',
    headers: JSON_HEADERS,
    body: JSON.stringify({ resume }),
    signal: AbortSignal.timeout(TIMEOUT_MS),
  });
  if (!r.ok) throw new Error('save server HTTP ' + r.status);
}

// the server's resume point from "Exit to dashboard", once: it is cleared on the server as it is used
export function takeResume(): ResumePoint | null {
  const resume = current && current.resume;
  if (!current || !resume) return null;
  current.resume = null;
  putResume(null).catch(() => {});
  return resume;
}

export function status(): { server: string; save: number | null; lastUpload: Upload | null } {
  return { server: state.server, save: current && current.id, lastUpload: state.lastUpload && { ...state.lastUpload } };
}

export function currentSave(): Save | null {
  return current;
}

export function dashboard(): boolean {
  return !!(current && isDashboard);
}
