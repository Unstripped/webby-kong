// A stored texture pack's files: a zip (entries under a prefix: the pack root) or an imported folder. Paths keep their
// stored case and match case-insensitively, as RT64 resolves them.
import { listZip, readZipEntry, type ZipEntry } from './zip';

export type StoredSource =
  | { kind: 'zip'; blob: Blob; prefix: string }
  | { kind: 'dir'; files: Array<{ path: string; blob: Blob }> };

export interface Locator {
  blob: Blob;
  entry: ZipEntry | null; // null: the blob is the whole file
}

export interface PackSource {
  paths(): string[];
  locate(path: string): Locator | null;
  read(path: string): Promise<Uint8Array<ArrayBuffer>>;
}

export async function readLocator(loc: Locator): Promise<Uint8Array<ArrayBuffer>> {
  return loc.entry ? readZipEntry(loc.blob, loc.entry) : new Uint8Array(await loc.blob.arrayBuffer());
}

export async function openSource(s: StoredSource): Promise<PackSource> {
  const files = new Map<string, { path: string; loc: Locator }>();
  const add = (path: string, loc: Locator) => {
    const key = path.toLowerCase();
    if (!files.has(key)) files.set(key, { path, loc });
  };
  if (s.kind === 'zip') {
    for (const e of await listZip(s.blob)) {
      if (e.name.startsWith(s.prefix)) add(e.name.slice(s.prefix.length), { blob: s.blob, entry: e });
    }
  } else {
    for (const f of s.files) add(f.path, { blob: f.blob, entry: null });
  }
  const paths = [...files.values()].map((f) => f.path);
  const locate = (path: string) => files.get(path.toLowerCase())?.loc ?? null;
  return {
    paths: () => paths,
    locate,
    read: async (path) => {
      const loc = locate(path);
      if (!loc) throw new Error(`${path} missing`);
      return readLocator(loc);
    },
  };
}

// the pack root among an archive's or folder's paths: '' when rt64.json is at the top, '<dir>/' when exactly one
// top-level directory holds one, else null
export function findRoot(paths: string[]): string | null {
  if (paths.some((p) => p.toLowerCase() === 'rt64.json')) return '';
  const dirs = new Set<string>();
  for (const p of paths) {
    const m = /^([^/]+)\/rt64\.json$/i.exec(p);
    if (m) dirs.add(m[1] + '/');
  }
  return dirs.size === 1 ? [...dirs][0]! : null;
}
