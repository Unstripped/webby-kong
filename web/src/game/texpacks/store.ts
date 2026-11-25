// Imported texture packs, kept in IndexedDB (dk64packs; quicksave.ts's dk64dbg schema stays as it is). The archive
// Blob or folder Files are stored as they are: the browser keeps them on disk.
import type { StoredSource } from './source';

export interface PackRecord {
  id: string;
  name: string;
  enabled: boolean;
  order: number; // ascending order = descending priority: the top pack wins
  added: string;
  source: StoredSource;
}

const DB_NAME = 'dk64packs';
const STORE = 'packs';

function idbOpen(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(STORE, { keyPath: 'id' });
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
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

export async function listPacks(): Promise<PackRecord[]> {
  const db = await idbOpen();
  try {
    const all = await new Promise<PackRecord[]>((resolve, reject) => {
      const req = db.transaction(STORE, 'readonly').objectStore(STORE).getAll();
      req.onsuccess = () => resolve(req.result as PackRecord[]);
      req.onerror = () => reject(req.error);
    });
    return all.sort((a, b) => a.order - b.order);
  } finally {
    db.close();
  }
}

export function putPacks(records: PackRecord[]): Promise<void> {
  return idbWrite((s) => {
    for (const r of records) s.put(r);
  });
}

export function deletePack(id: string): Promise<void> {
  return idbWrite((s) => s.delete(id));
}
