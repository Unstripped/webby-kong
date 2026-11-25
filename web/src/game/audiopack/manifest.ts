// Audio pack manifest.json (format 1, docs/audio-packs.md). The rules and messages match tools/packs.py exactly, so
// a pack the server accepted always loads here and a local import fails with the server's words.

export class PackError extends Error {
  override name = 'PackError';
}

export interface ManifestEntry {
  file: string;
  loop: boolean | undefined;
  loopStart: number;
  loopEnd: number | undefined;
  gain: number;
}

export interface Manifest {
  name: string;
  author: string;
  version: string;
  description: string;
  songs: Map<number, ManifestEntry>;
  sfx: Map<number, ManifestEntry>;
}

export const AUDIO_EXT = ['.ogg', '.oga', '.opus', '.mp3', '.wav', '.flac'];
const TEXT_FIELDS: Array<['author' | 'version' | 'description', number]> = [
  ['author', 60],
  ['version', 20],
  ['description', 500],
];

const bad = (message: string) => new PackError(message);
const isObject = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v);
const isNumber = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
// code points, like Python's len()
const chars = (s: string) => [...s].length;
export const hexId = (id: number) => '0x' + id.toString(16).toUpperCase();

function parseId(key: string, min: number, max: number): number | null {
  let value: number;
  if (/^[0-9]+$/.test(key)) value = parseInt(key, 10);
  else if (/^0[xX][0-9a-fA-F]+$/.test(key)) value = parseInt(key.slice(2), 16);
  else return null;
  return value >= min && value <= max ? value : null;
}

function parseEntry(kind: string, id: number, value: unknown, files: Set<string>): ManifestEntry {
  const where = `${kind} ${hexId(id)}`;
  if (!isObject(value)) throw bad(`${where}: entry is invalid`);
  const file = value.file;
  if (typeof file !== 'string' || !file) throw bad(`${where}: file is invalid`);
  if (!files.has(file)) throw bad(`${where}: file "${file}" is not in the pack`);
  const lower = file.toLowerCase();
  if (!AUDIO_EXT.some((ext) => lower.endsWith(ext))) {
    throw bad(`${where}: file must be .ogg, .oga, .opus, .mp3, .wav or .flac`);
  }
  if ('loop' in value && typeof value.loop !== 'boolean') throw bad(`${where}: loop is invalid`);
  const loopStart = 'loopStart' in value ? value.loopStart : 0;
  if (!isNumber(loopStart) || loopStart < 0) throw bad(`${where}: loopStart is invalid`);
  const loopEnd = value.loopEnd;
  if ('loopEnd' in value && (!isNumber(loopEnd) || loopEnd <= loopStart)) throw bad(`${where}: loopEnd is invalid`);
  const gain = 'gain' in value ? value.gain : 1;
  if (!isNumber(gain) || !(gain > 0 && gain <= 4)) throw bad(`${where}: gain is invalid`);
  return {
    file,
    loop: value.loop as boolean | undefined,
    loopStart,
    loopEnd: loopEnd as number | undefined,
    gain,
  };
}

function parseTable(
  manifest: Record<string, unknown>,
  kind: 'songs' | 'sfx',
  min: number,
  max: number,
  what: string,
  files: Set<string>,
): Map<number, ManifestEntry> {
  const table = kind in manifest ? manifest[kind] : {};
  if (!isObject(table)) throw bad(`${kind} must be an object`);
  const out = new Map<number, ManifestEntry>();
  for (const [key, value] of Object.entries(table)) {
    const id = parseId(key, min, max);
    if (id === null) throw bad(`${kind} key "${key}" is not ${what}`);
    if (out.has(id)) throw bad(`${kind} id ${hexId(id)} is listed twice`);
    out.set(id, parseEntry(kind, id, value, files));
  }
  return out;
}

// `files`: the zip's file entry names
export function parseManifest(json: unknown, files: Set<string>): Manifest {
  if (!isObject(json)) throw bad('manifest.json must be a JSON object');
  if (json.format !== 1) throw bad('format must be 1');
  const name = json.name;
  if (typeof name !== 'string' || chars(name.trim()) < 1 || chars(name.trim()) > 60) {
    throw bad('name must be 1-60 characters');
  }
  const text: Record<string, string> = {};
  for (const [field, limit] of TEXT_FIELDS) {
    const value = field in json ? json[field] : '';
    if (typeof value !== 'string' || chars(value.trim()) > limit) {
      throw bad(`${field} must be a string of at most ${limit} characters`);
    }
    text[field] = value.trim();
  }
  const songs = parseTable(json, 'songs', 0, 175, 'a song id (0-175 or 0x0-0xAF)', files);
  const sfx = parseTable(json, 'sfx', 1, 0x1fff, 'a sound id (1-8191 or 0x1-0x1FFF)', files);
  if (songs.size + sfx.size === 0) throw bad('the pack replaces nothing (no songs or sfx)');
  return {
    name: name.trim(),
    author: text.author!,
    version: text.version!,
    description: text.description!,
    songs,
    sfx,
  };
}
