// Tests only: builds pack zips in memory (zip.test.ts, load.test.ts).
const enc = new TextEncoder();

export async function deflateRaw(data: Uint8Array<ArrayBuffer>): Promise<Uint8Array<ArrayBuffer>> {
  const stream = new Blob([data]).stream().pipeThrough(new CompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

export interface Entry {
  name: string;
  method: 0 | 8;
  data: Uint8Array<ArrayBuffer>; // as stored
  size: number; // declared uncompressed size
}

// a minimal zip: local headers + data, central directory, end record (CRCs left 0: the reader does not check them)
export function buildZip(entries: Entry[]): Uint8Array<ArrayBuffer> {
  const parts: number[] = [];
  const central: number[] = [];
  const u16 = (a: number[], v: number) => a.push(v & 0xff, (v >> 8) & 0xff);
  const u32 = (a: number[], v: number) => a.push(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff);
  for (const e of entries) {
    const name = enc.encode(e.name);
    const offset = parts.length;
    u32(parts, 0x04034b50);
    u16(parts, 20);
    u16(parts, 0x800);
    u16(parts, e.method);
    u32(parts, 0);
    u32(parts, 0);
    u32(parts, e.data.length);
    u32(parts, e.size);
    u16(parts, name.length);
    u16(parts, 0);
    parts.push(...name, ...e.data);
    u32(central, 0x02014b50);
    u16(central, 20);
    u16(central, 20);
    u16(central, 0x800);
    u16(central, e.method);
    u32(central, 0);
    u32(central, 0);
    u32(central, e.data.length);
    u32(central, e.size);
    u16(central, name.length);
    u16(central, 0);
    u16(central, 0);
    u16(central, 0);
    u16(central, 0);
    u32(central, 0);
    u32(central, offset);
    central.push(...name);
  }
  const cdOffset = parts.length;
  const end: number[] = [];
  u32(end, 0x06054b50);
  u16(end, 0);
  u16(end, 0);
  u16(end, entries.length);
  u16(end, entries.length);
  u32(end, central.length);
  u32(end, cdOffset);
  u16(end, 0);
  return new Uint8Array([...parts, ...central, ...end]);
}

// a stored zip of manifest.json plus the given files
export function packZip(manifest: unknown, files: Record<string, string>): Uint8Array<ArrayBuffer> {
  const entry = (name: string, text: string): Entry => {
    const data = enc.encode(text);
    return { name, method: 0, data, size: data.length };
  };
  return buildZip([
    entry('manifest.json', JSON.stringify(manifest)),
    ...Object.entries(files).map(([name, text]) => entry(name, text)),
  ]);
}
