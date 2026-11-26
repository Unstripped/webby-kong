// Zip archives for texture packs: a reader (RT64 .rtz packs and DK64 Rekongpiled .nrm mods are zips; entries stored,
// deflated or zstd-compressed, zip64 included) and a store-only writer for the texture dump.
import { decompress as zstdDecompress } from 'fzstd';

export interface ZipEntry {
  name: string;
  method: number;
  compressedSize: number;
  size: number;
  headerOffset: number;
  flags: number;
}

const EOCD = 0x06054b50;
const ZIP64_LOCATOR = 0x07064b50;
const ZIP64_EOCD = 0x06064b50;
const CENTRAL = 0x02014b50;
const LOCAL = 0x04034b50;

async function bytesOf(blob: Blob, start: number, end: number): Promise<Uint8Array<ArrayBuffer>> {
  return new Uint8Array(await blob.slice(start, end).arrayBuffer());
}

const viewOf = (b: Uint8Array) => new DataView(b.buffer, b.byteOffset, b.byteLength);

// '\' → '/', no leading '/' or './'; null for directories and names that climb out with '..'
function normalizeName(raw: string): string | null {
  let name = raw.replace(/\\/g, '/');
  for (;;) {
    if (name.startsWith('/')) name = name.slice(1);
    else if (name.startsWith('./')) name = name.slice(2);
    else break;
  }
  if (name === '' || name.endsWith('/')) return null;
  if (name.split('/').includes('..')) return null;
  return name;
}

export async function listZip(blob: Blob): Promise<ZipEntry[]> {
  const notZip = () => new Error('Not a zip archive');
  const tailLen = Math.min(blob.size, 22 + 65535);
  const tailStart = blob.size - tailLen;
  const tail = await bytesOf(blob, tailStart, blob.size);
  const tv = viewOf(tail);
  let eocd = -1;
  for (let i = tail.length - 22; i >= 0; i--) {
    if (tv.getUint32(i, true) === EOCD) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw notZip();
  let count = tv.getUint16(eocd + 10, true);
  let cdSize = tv.getUint32(eocd + 12, true);
  let cdOffset = tv.getUint32(eocd + 16, true);
  if (count === 0xffff || cdSize === 0xffffffff || cdOffset === 0xffffffff) {
    const loc = eocd - 20;
    if (loc < 0 || tv.getUint32(loc, true) !== ZIP64_LOCATOR) throw notZip();
    const at = Number(tv.getBigUint64(loc + 8, true));
    const z = await bytesOf(blob, at, at + 56);
    const zv = viewOf(z);
    if (z.length < 56 || zv.getUint32(0, true) !== ZIP64_EOCD) throw notZip();
    count = Number(zv.getBigUint64(32, true));
    cdSize = Number(zv.getBigUint64(40, true));
    cdOffset = Number(zv.getBigUint64(48, true));
  }
  const cd = await bytesOf(blob, cdOffset, cdOffset + cdSize);
  if (cd.length < cdSize) throw notZip();
  const cv = viewOf(cd);
  const utf8 = new TextDecoder();
  const out: ZipEntry[] = [];
  let p = 0;
  for (let i = 0; i < count; i++) {
    if (p + 46 > cd.length || cv.getUint32(p, true) !== CENTRAL) throw notZip();
    const flags = cv.getUint16(p + 8, true);
    const method = cv.getUint16(p + 10, true);
    let compressedSize = cv.getUint32(p + 20, true);
    let size = cv.getUint32(p + 24, true);
    const nameLen = cv.getUint16(p + 28, true);
    const extraLen = cv.getUint16(p + 30, true);
    const commentLen = cv.getUint16(p + 32, true);
    let headerOffset = cv.getUint32(p + 42, true);
    const name = utf8.decode(cd.subarray(p + 46, p + 46 + nameLen));
    // zip64 extra field: only the saturated values, in this order
    for (let q = p + 46 + nameLen, end = q + extraLen; q + 4 <= end; ) {
      const id = cv.getUint16(q, true);
      const len = cv.getUint16(q + 2, true);
      if (id === 0x0001) {
        let r = q + 4;
        if (size === 0xffffffff) {
          size = Number(cv.getBigUint64(r, true));
          r += 8;
        }
        if (compressedSize === 0xffffffff) {
          compressedSize = Number(cv.getBigUint64(r, true));
          r += 8;
        }
        if (headerOffset === 0xffffffff) headerOffset = Number(cv.getBigUint64(r, true));
      }
      q += 4 + len;
    }
    p += 46 + nameLen + extraLen + commentLen;
    const norm = normalizeName(name);
    if (norm !== null) out.push({ name: norm, method, compressedSize, size, headerOffset, flags });
  }
  return out;
}

async function inflateRaw(data: Uint8Array<ArrayBuffer>): Promise<Uint8Array<ArrayBuffer>> {
  const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

export async function readZipEntry(blob: Blob, e: ZipEntry): Promise<Uint8Array<ArrayBuffer>> {
  if (e.flags & 1 || (e.method !== 0 && e.method !== 8 && e.method !== 93)) {
    throw new Error(`Unsupported zip compression (method ${e.method})`);
  }
  const h = await bytesOf(blob, e.headerOffset, e.headerOffset + 30);
  const hv = viewOf(h);
  if (h.length < 30 || hv.getUint32(0, true) !== LOCAL) throw new Error(`${e.name}: corrupt zip entry`);
  const start = e.headerOffset + 30 + hv.getUint16(26, true) + hv.getUint16(28, true);
  const data = await bytesOf(blob, start, start + e.compressedSize);
  if (data.length < e.compressedSize) throw new Error(`${e.name}: corrupt zip entry`);
  if (e.method === 8) return inflateRaw(data);
  if (e.method === 93) return zstdDecompress(data) as Uint8Array<ArrayBuffer>;
  return data;
}

let crcTable: Uint32Array | null = null;

export function crc32(data: Uint8Array, crc = 0): number {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let c = ~crc >>> 0;
  for (let i = 0; i < data.length; i++) c = crcTable[(c ^ data[i]!) & 0xff]! ^ (c >>> 8);
  return ~c >>> 0;
}

// store-only zip (method 0, UTF-8 names)
export function writeZip(files: Array<{ name: string; data: Uint8Array<ArrayBuffer> }>): Blob {
  const tooMany = () => new Error('Too many textures for one zip');
  if (files.length > 0xffff) throw tooMany();
  const now = new Date();
  const time = (now.getHours() << 11) | (now.getMinutes() << 5) | (now.getSeconds() >> 1);
  const date = ((now.getFullYear() - 1980) << 9) | ((now.getMonth() + 1) << 5) | now.getDate();
  const utf8 = new TextEncoder();
  const parts: Uint8Array<ArrayBuffer>[] = [];
  const central: Uint8Array<ArrayBuffer>[] = [];
  let offset = 0;
  for (const f of files) {
    const name = utf8.encode(f.name);
    const crc = crc32(f.data);
    const size = f.data.length;
    if (offset + 30 + name.length + size > 0xffffffff) throw tooMany();
    const local = new Uint8Array(30 + name.length);
    const lv = viewOf(local);
    lv.setUint32(0, LOCAL, true);
    lv.setUint16(4, 20, true);
    lv.setUint16(6, 0x0800, true);
    lv.setUint16(8, 0, true);
    lv.setUint16(10, time, true);
    lv.setUint16(12, date, true);
    lv.setUint32(14, crc, true);
    lv.setUint32(18, size, true);
    lv.setUint32(22, size, true);
    lv.setUint16(26, name.length, true);
    local.set(name, 30);
    const cen = new Uint8Array(46 + name.length);
    const cv = viewOf(cen);
    cv.setUint32(0, CENTRAL, true);
    cv.setUint16(4, 20, true);
    cv.setUint16(6, 20, true);
    cv.setUint16(8, 0x0800, true);
    cv.setUint16(12, time, true);
    cv.setUint16(14, date, true);
    cv.setUint32(16, crc, true);
    cv.setUint32(20, size, true);
    cv.setUint32(24, size, true);
    cv.setUint16(28, name.length, true);
    cv.setUint32(42, offset, true);
    cen.set(name, 46);
    parts.push(local, f.data);
    central.push(cen);
    offset += local.length + size;
  }
  const cdSize = central.reduce((n, c) => n + c.length, 0);
  if (offset + cdSize > 0xffffffff) throw tooMany();
  const end = new Uint8Array(22);
  const ev = viewOf(end);
  ev.setUint32(0, EOCD, true);
  ev.setUint16(8, files.length, true);
  ev.setUint16(10, files.length, true);
  ev.setUint32(12, cdSize, true);
  ev.setUint32(16, offset, true);
  return new Blob([...parts, ...central, end], { type: 'application/zip' });
}
