// Which audio packs this browser plays, and the packs imported on this device.
// - enabled(): localStorage dk64.audiopacks, keys highest priority first: "s:<server id>" or "l:<sha1>".
// - Local packs: IndexedDB dk64-audiopacks (meta + bytes, keyed by the zip's SHA-1).
// - listServer(): the save server's shared packs (tools/packs.py); [] without one.
import type { ServerPack } from '../../shared/packs';
import { sha1Hex } from '../../shared/sha1';
import { openPack } from './zip';

const ENABLED_KEY = 'dk64.audiopacks';
const DB_NAME = 'dk64-audiopacks';
const META = 'meta';
const BYTES = 'bytes';

export interface LocalMeta {
  sha1: string;
  name: string;
  author: string;
  version: string;
  songs: number;
  sfx: number;
  size: number;
  added: string;
}

export function enabled(): string[] {
  try {
    const list: unknown = JSON.parse(localStorage.getItem(ENABLED_KEY) || '[]');
    return Array.isArray(list) ? list.filter((k): k is string => typeof k === 'string') : [];
  } catch {
    return [];
  }
}

function store(list: string[]): void {
  localStorage.setItem(ENABLED_KEY, JSON.stringify(list));
}

// enabling puts the pack first (highest priority); disabling removes it
export function setEnabled(key: string, on: boolean): void {
  const list = enabled().filter((k) => k !== key);
  store(on ? [key, ...list] : list);
}

// delta -1: one place higher in priority
export function move(key: string, delta: -1 | 1): void {
  const list = enabled();
  const i = list.indexOf(key);
  const j = i + delta;
  if (i < 0 || j < 0 || j >= list.length) return;
  [list[i], list[j]] = [list[j]!, list[i]!];
  store(list);
}

function idbOpen(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => {
      req.result.createObjectStore(META);
      req.result.createObjectStore(BYTES);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function idbGet<T>(name: string, read: (store: IDBObjectStore) => IDBRequest): Promise<T> {
  const db = await idbOpen();
  try {
    return await new Promise<T>((resolve, reject) => {
      const req = read(db.transaction(name, 'readonly').objectStore(name));
      req.onsuccess = () => resolve(req.result as T);
      req.onerror = () => reject(req.error);
    });
  } finally {
    db.close();
  }
}

async function idbWrite(write: (meta: IDBObjectStore, bytes: IDBObjectStore) => void): Promise<void> {
  const db = await idbOpen();
  try {
    await new Promise<void>((resolve, reject) => {
      const tx = db.transaction([META, BYTES], 'readwrite');
      write(tx.objectStore(META), tx.objectStore(BYTES));
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error);
    });
  } finally {
    db.close();
  }
}

export async function listLocal(): Promise<LocalMeta[]> {
  if (typeof indexedDB === 'undefined') return [];
  try {
    return await idbGet<LocalMeta[]>(META, (s) => s.getAll());
  } catch {
    return [];
  }
}

export async function getLocalBytes(sha1: string): Promise<Uint8Array<ArrayBuffer> | null> {
  if (typeof indexedDB === 'undefined') return null;
  const bytes = await idbGet<Uint8Array<ArrayBuffer> | undefined>(BYTES, (s) => s.get(sha1));
  return bytes ?? null;
}

export async function removeLocal(sha1: string): Promise<void> {
  await idbWrite((meta, bytes) => {
    meta.delete(sha1);
    bytes.delete(sha1);
  });
  setEnabled('l:' + sha1, false);
}

// checks the pack like the server does (PackError), stores it and turns it on at the top
export async function importLocal(file: File): Promise<LocalMeta> {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const { manifest } = await openPack(bytes);
  const sha1 = await sha1Hex(bytes);
  const meta: LocalMeta = {
    sha1,
    name: manifest.name,
    author: manifest.author,
    version: manifest.version,
    songs: manifest.songs.size,
    sfx: manifest.sfx.size,
    size: bytes.length,
    added: new Date().toISOString(),
  };
  await idbWrite((m, b) => {
    m.put(meta, sha1);
    b.put(bytes, sha1);
  });
  setEnabled('l:' + sha1, true);
  return meta;
}

export async function listServer(): Promise<ServerPack[]> {
  try {
    const r = await fetch('api/packs', { cache: 'no-store', credentials: 'same-origin' });
    if (!r.ok) return [];
    const body = (await r.json()) as { packs?: ServerPack[] };
    return Array.isArray(body.packs) ? body.packs : [];
  } catch {
    return [];
  }
}
