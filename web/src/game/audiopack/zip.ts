// A small zip reader for audio packs: stored and deflated entries, no zip64, with the limits and entry-name rules of
// docs/audio-packs.md (and the same messages as tools/packs.py).
import { type Manifest, PackError, parseManifest } from './manifest';

export const MAX_ENTRIES = 2000;
export const MAX_UNPACKED_BYTES = 512 * 1024 * 1024;

const EOCD = 0x06054b50;
const CENTRAL = 0x02014b50;
const LOCAL = 0x04034b50;
const EOCD_SEARCH = 22 + 0xffff;
// bytes 0x80..0xFF of code page 437: zip names without the UTF-8 flag (bit 11), as Python's zipfile reads them
const CP437_HIGH =
  'ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒáíóúñÑªº¿⌐¬½¼¡«»░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀' +
  'αßΓπΣσµτΦΘΩδ∞φε∩≡±≥≤⌠⌡÷≈°∙·√ⁿ²■\u00a0';

function decodeName(bytes: Uint8Array, utf8: boolean): string {
  if (utf8) return new TextDecoder('utf-8').decode(bytes);
  return Array.from(bytes, (b) => (b < 0x80 ? String.fromCharCode(b) : CP437_HIGH[b - 0x80]!)).join('');
}

export function unsafeName(name: string): boolean {
  return name.startsWith('/') || name.includes('\\') || name.split('/').includes('..');
}

async function inflate(data: Uint8Array<ArrayBuffer>, size: number, name: string): Promise<Uint8Array> {
  const corrupt = () => new PackError(`corrupt entry ${name}`);
  const input = new ReadableStream<BufferSource>({
    start(c) {
      c.enqueue(data);
      c.close();
    },
  });
  const reader = input.pipeThrough(new DecompressionStream('deflate-raw')).getReader();
  const out = new Uint8Array(size);
  let pos = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (pos + value.length > size) {
        void reader.cancel();
        throw corrupt();
      }
      out.set(value, pos);
      pos += value.length;
    }
  } catch (err) {
    throw err instanceof PackError ? err : corrupt();
  }
  if (pos !== size) throw corrupt();
  return out;
}

// name -> bytes of every file entry (directories are left out)
export async function readZip(bytes: Uint8Array<ArrayBuffer>): Promise<Map<string, Uint8Array>> {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const u16 = (at: number) => view.getUint16(at, true);
  const u32 = (at: number) => view.getUint32(at, true);
  const notZip = () => new PackError('not a zip file');

  let eocd = -1;
  for (let at = bytes.length - 22; at >= Math.max(0, bytes.length - EOCD_SEARCH); at--) {
    if (u32(at) === EOCD) {
      eocd = at;
      break;
    }
  }
  if (eocd < 0) throw notZip();
  const count = u16(eocd + 10);
  const cdSize = u32(eocd + 12);
  const cdOffset = u32(eocd + 16);
  if (count === 0xffff || cdSize === 0xffffffff || cdOffset === 0xffffffff) {
    throw new PackError('zip64 packs are not supported');
  }
  if (count > MAX_ENTRIES) throw new PackError(`too many entries (at most ${MAX_ENTRIES})`);
  if (cdOffset + cdSize > eocd) throw notZip();

  const entries: Array<{ name: string; method: number; comp: number; size: number; local: number }> = [];
  let total = 0;
  let at = cdOffset;
  for (let i = 0; i < count; i++) {
    if (at + 46 > eocd || u32(at) !== CENTRAL) throw notZip();
    const flags = u16(at + 8);
    const method = u16(at + 10);
    const comp = u32(at + 20);
    const size = u32(at + 24);
    const nameLen = u16(at + 28);
    const extraLen = u16(at + 30);
    const commentLen = u16(at + 32);
    const local = u32(at + 42);
    const name = decodeName(bytes.subarray(at + 46, at + 46 + nameLen), (flags & 0x800) !== 0);
    if (comp === 0xffffffff || size === 0xffffffff || local === 0xffffffff) {
      throw new PackError('zip64 packs are not supported');
    }
    if (unsafeName(name)) throw new PackError(`entry "${name}" has an unsafe name`);
    if (flags & 1) throw new PackError(`entry "${name}" is encrypted`);
    if (method !== 0 && method !== 8) throw new PackError(`entry "${name}" uses an unsupported compression method`);
    total += size;
    entries.push({ name, method, comp, size, local });
    at += 46 + nameLen + extraLen + commentLen;
  }
  if (total > MAX_UNPACKED_BYTES) throw new PackError('the pack unpacks to more than 512 MiB');

  const files = new Map<string, Uint8Array>();
  for (const e of entries) {
    if (e.local + 30 > bytes.length || u32(e.local) !== LOCAL) throw new PackError(`corrupt entry ${e.name}`);
    const start = e.local + 30 + u16(e.local + 26) + u16(e.local + 28);
    if (start + e.comp > bytes.length) throw new PackError(`corrupt entry ${e.name}`);
    const data = bytes.subarray(start, start + e.comp);
    if (e.name.endsWith('/')) continue;
    if (e.method === 0) {
      if (e.comp !== e.size) throw new PackError(`corrupt entry ${e.name}`);
      files.set(e.name, data.slice());
    } else {
      files.set(e.name, await inflate(data, e.size, e.name));
    }
  }
  return files;
}

// a pack zip -> its files and checked manifest (PackError with tools/packs.py's messages otherwise)
export async function openPack(
  bytes: Uint8Array<ArrayBuffer>,
): Promise<{ files: Map<string, Uint8Array>; manifest: Manifest }> {
  const files = await readZip(bytes);
  const raw = files.get('manifest.json');
  if (!raw) throw new PackError('no manifest.json at the root of the pack');
  if (raw.length > 64 * 1024) throw new PackError('manifest.json is larger than 64 KiB');
  let json: unknown;
  try {
    json = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(raw));
  } catch {
    throw new PackError('manifest.json is not valid UTF-8 JSON');
  }
  return { files, manifest: parseManifest(json, new Set(files.keys())) };
}
