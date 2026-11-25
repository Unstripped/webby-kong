// rt64.json, the RT64 texture pack database: parsed and resolved against a pack's files exactly as RT64 does
// (src/common/rt64_replacement_database.cpp at d9817c68, MIT): nlohmann defaults, legacy keys, auto paths, filters.
export type Operation = 'preload' | 'stream' | 'stall';
export type Shift = 'none' | 'half';
type AutoPath = 'rt64' | 'rice';

export interface Rt64Config {
  autoPath: AutoPath;
  defaultOperation: Operation;
  defaultShift: Shift;
  configurationVersion: number;
  hashVersion: number;
}

export interface Rt64Texture {
  rt64: string;
  rice: string;
  path: string;
  operation: Operation | 'auto';
  shift: Shift | 'auto';
}

export interface Rt64Database {
  config: Rt64Config;
  textures: Rt64Texture[];
  operationFilters: Array<{ wildcard: string; operation: Operation | 'auto' }>;
  shiftFilters: Array<{ wildcard: string; shift: Shift | 'auto' }>;
}

export interface ResolvedTexture {
  hash: bigint;
  path: string;
  operation: Operation;
  shift: Shift;
}

type Obj = Record<string, unknown>;

const isObj = (v: unknown): v is Obj => typeof v === 'object' && v !== null && !Array.isArray(v);

// nlohmann's j.value(key, default): the default only when the key is absent
function value<T>(j: Obj, key: string, def: T, conv: (v: unknown) => T): T {
  return key in j ? conv(j[key]) : def;
}

// NLOHMANN_JSON_SERIALIZE_ENUM: a string the enum doesn't list maps to its first member
const enumOf =
  <T extends string>(members: readonly T[]) =>
  (v: unknown): T =>
    members.find((m) => m === v) ?? members[0]!;

const toOperation = enumOf(['preload', 'stream', 'stall', 'auto'] as const);
const toShift = enumOf(['none', 'half', 'auto'] as const);
const toAutoPath = enumOf(['rt64', 'rice'] as const);
const toStr = (v: unknown) => (typeof v === 'string' ? v : '');
const toUint = (v: unknown) => (typeof v === 'number' && v >= 0 ? Math.floor(v) : 0);
const toArray = (v: unknown) => (Array.isArray(v) ? v.filter(isObj) : []);
const forwardSlashes = (s: string) => s.replace(/\\/g, '/');

// RT64 never resolves 'auto' from a configuration; it only appears in a malformed one
const concrete = <T extends string>(v: T | 'auto', def: Exclude<T, 'auto'>): Exclude<T, 'auto'> =>
  v === 'auto' ? def : (v as Exclude<T, 'auto'>);

export function parseDatabase(text: string): Rt64Database {
  let j: unknown;
  try {
    j = JSON.parse(text);
  } catch (err) {
    throw new Error('rt64.json: ' + (err as Error).message);
  }
  if (!isObj(j)) throw new Error('rt64.json: not a JSON object');
  let config: Rt64Config = {
    autoPath: 'rt64',
    defaultOperation: 'stream',
    defaultShift: 'half',
    configurationVersion: 3,
    hashVersion: 5,
  };
  if ('configuration' in j) {
    const c = isObj(j.configuration) ? j.configuration : {};
    const configurationVersion = value(c, 'configurationVersion', 1, toUint);
    // packs made before configuration version 3 default to no shift
    const defaultShift = configurationVersion < 3 ? 'none' : 'half';
    config = {
      configurationVersion,
      autoPath: value(c, 'autoPath', 'rt64', toAutoPath),
      defaultOperation: concrete(value(c, 'defaultOperation', 'stream', toOperation), 'preload'),
      defaultShift: concrete(value(c, 'defaultShift', defaultShift, toShift), 'none'),
      hashVersion: value(c, 'hashVersion', 1, toUint),
    };
  }
  const textures = toArray(j.textures).map((t): Rt64Texture => {
    const h = isObj(t.hashes) ? t.hashes : {};
    return {
      // the first database version named the hash version in the key
      rt64: value(h, 'rt64', value(h, 'rt64v1', '', toStr), toStr),
      rice: value(h, 'rice', '', toStr),
      path: value(t, 'path', '', toStr),
      operation: value(t, 'operation', 'auto', toOperation),
      shift: value(t, 'shift', 'auto', toShift),
    };
  });
  return {
    config,
    textures,
    operationFilters: toArray(j.operationFilters).map((f) => ({
      wildcard: forwardSlashes(value(f, 'wildcard', '', toStr)),
      operation: value(f, 'operation', 'stream', toOperation),
    })),
    shiftFilters: toArray(j.shiftFilters).map((f) => ({
      wildcard: forwardSlashes(value(f, 'wildcard', '', toStr)),
      shift: value(f, 'shift', 'half', toShift),
    })),
  };
}

// RT64's matcher: '*' any run, '?' any one character, case-sensitive
export function checkWildcard(str: string, pat: string): boolean {
  let s = 0;
  let p = 0;
  let star = -1;
  let match = -1;
  while (s < str.length) {
    if (p < pat.length && (pat[p] === '?' || str[s] === pat[p])) {
      s++;
      p++;
    } else if (p < pat.length && pat[p] === '*') {
      star = p;
      match = s;
      p++;
    } else if (star >= 0) {
      match++;
      p = star + 1;
      s = match;
    } else {
      return false;
    }
  }
  while (p < pat.length && pat[p] === '*') p++;
  return p === pat.length;
}

// strtoull(s, nullptr, 16)
export function hashFromString(s: string): bigint {
  const m = /^\s*(?:0[xX](?=[0-9a-fA-F]))?([0-9a-fA-F]*)/.exec(s)!;
  if (!m[1]) return 0n;
  const v = BigInt('0x' + m[1]);
  return v > 0xffffffffffffffffn ? 0xffffffffffffffffn : v;
}

const KNOWN = ['.dds', '.png'];

function removeKnownExtension(path: string): string {
  const lower = path.toLowerCase();
  const ext = KNOWN.find((e) => lower.endsWith(e));
  return ext ? path.slice(0, path.length - ext.length) : path;
}

function resolveOperation(db: Rt64Database, path: string, op: Operation | 'auto'): Operation {
  if (op !== 'auto') return op;
  let r: Operation | 'auto' = db.config.defaultOperation;
  for (const f of db.operationFilters) if (checkWildcard(path, f.wildcard)) r = f.operation;
  return concrete(r, 'preload');
}

function resolveShift(db: Rt64Database, path: string, shift: Shift | 'auto'): Shift {
  if (shift !== 'auto') return shift;
  let r: Shift | 'auto' = db.config.defaultShift;
  for (const f of db.shiftFilters) if (checkWildcard(path, f.wildcard)) r = f.shift;
  return concrete(r, 'none');
}

// the pack's textures: each entry's file (an explicit path tried as .dds then .png, else the auto path map), the
// first per hash; total counts entries with a hash, missing those whose file isn't there
export function resolve(
  db: Rt64Database,
  paths: string[],
): { hashVersion: number; textures: ResolvedTexture[]; total: number; missing: number } {
  const byLower = new Map<string, string>();
  const auto = new Map<string, string>();
  for (const p of paths) {
    const lower = p.toLowerCase();
    if (!byLower.has(lower)) byLower.set(lower, p);
    const ext = KNOWN.find((e) => lower.endsWith(e));
    if (!ext) continue;
    const name = p.slice(p.lastIndexOf('/') + 1);
    let key: string | null = null;
    if (db.config.autoPath === 'rt64') {
      const dot = name.indexOf('.');
      key = name.slice(0, dot).toLowerCase();
    } else {
      const hash = name.indexOf('#');
      const under = name.lastIndexOf('_');
      if (hash >= 0 && under > hash) key = name.slice(hash + 1, under).toLowerCase();
    }
    if (key === null) continue;
    const prev = auto.get(key);
    if (prev === undefined || (ext === '.dds' && !prev.toLowerCase().endsWith('.dds'))) auto.set(key, p);
  }
  const seen = new Set<bigint>();
  const textures: ResolvedTexture[] = [];
  let total = 0;
  let missing = 0;
  for (const t of db.textures) {
    if (t.rt64 === '') continue;
    total++;
    let path: string | undefined;
    if (t.path !== '') {
      const base = removeKnownExtension(forwardSlashes(t.path));
      for (const ext of KNOWN) {
        path = byLower.get((base + ext).toLowerCase());
        if (path !== undefined) break;
      }
    } else {
      path = auto.get(db.config.autoPath === 'rt64' ? t.rt64 : t.rice);
    }
    if (path === undefined) {
      missing++;
      continue;
    }
    const hash = hashFromString(t.rt64);
    if (seen.has(hash)) continue;
    seen.add(hash);
    textures.push({
      hash,
      path,
      operation: resolveOperation(db, path, t.operation),
      shift: resolveShift(db, path, t.shift),
    });
  }
  return { hashVersion: db.config.hashVersion, textures, total, missing };
}
