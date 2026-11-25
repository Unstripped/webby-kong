// DDS textures in packs: BC1-BC3 and BC7 (uploaded compressed, through WebGL extensions) and 32-bit RGBA/BGRA,
// with their mip chains. Format codes are port_texpack_upload's (port/src/gfx/gfx_texpack.c).
export const TEX_RGBA8 = 0;
export const TEX_BC1 = 1;
export const TEX_BC2 = 2;
export const TEX_BC3 = 3;
export const TEX_BC7 = 4;

export interface DdsImage {
  format: number;
  width: number;
  height: number;
  levels: Uint8Array<ArrayBuffer>[];
}

const fourCC = (s: string) => s.charCodeAt(0) | (s.charCodeAt(1) << 8) | (s.charCodeAt(2) << 16) | (s.charCodeAt(3) << 24);

const DDSD_MIPMAPCOUNT = 0x20000;
const DDPF_FOURCC = 0x4;
const DDPF_RGB = 0x40;
const DDSCAPS2_CUBEMAP = 0x200;
const DDSCAPS2_VOLUME = 0x200000;

function levelBytes(format: number, w: number, h: number): number {
  return format === TEX_RGBA8 ? w * h * 4 : Math.ceil(w / 4) * Math.ceil(h / 4) * (format === TEX_BC1 ? 8 : 16);
}

export function parseDds(bytes: Uint8Array<ArrayBuffer>): DdsImage {
  const fail = (why: string) => new Error('DDS: ' + why);
  if (bytes.length < 128) throw fail('truncated header');
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (v.getUint32(0, true) !== fourCC('DDS ') || v.getUint32(4, true) !== 124) throw fail('not a DDS file');
  const flags = v.getUint32(8, true);
  const height = v.getUint32(12, true);
  const width = v.getUint32(16, true);
  const mipCount = v.getUint32(28, true);
  const pfFlags = v.getUint32(80, true);
  const code = v.getUint32(84, true);
  const caps2 = v.getUint32(112, true);
  if (caps2 & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME)) throw fail('cubemaps and volume textures are not supported');
  if (width === 0 || height === 0) throw fail('empty image');
  let format = -1;
  let swizzle = false; // BGRA in the file
  let opaque = false; // no alpha channel: alpha 255
  let offset = 128;
  if (pfFlags & DDPF_FOURCC && code === fourCC('DX10')) {
    if (bytes.length < 148) throw fail('truncated header');
    const dxgi = v.getUint32(128, true);
    const dimension = v.getUint32(132, true);
    const misc = v.getUint32(136, true);
    const arraySize = v.getUint32(140, true);
    if (arraySize !== 1) throw fail('texture arrays are not supported');
    if (dimension === 4 || misc & 0x4) throw fail('cubemaps and volume textures are not supported');
    offset = 148;
    // sRGB variants load as UNORM
    if (dxgi === 71 || dxgi === 72) format = TEX_BC1;
    else if (dxgi === 74 || dxgi === 75) format = TEX_BC2;
    else if (dxgi === 77 || dxgi === 78) format = TEX_BC3;
    else if (dxgi === 98 || dxgi === 99) format = TEX_BC7;
    else if (dxgi === 28 || dxgi === 29) format = TEX_RGBA8;
    else if (dxgi === 87 || dxgi === 91) {
      format = TEX_RGBA8;
      swizzle = true;
    } else throw fail(`unsupported DXGI format ${dxgi}`);
  } else if (pfFlags & DDPF_FOURCC) {
    if (code === fourCC('DXT1')) format = TEX_BC1;
    else if (code === fourCC('DXT2') || code === fourCC('DXT3')) format = TEX_BC2;
    else if (code === fourCC('DXT4') || code === fourCC('DXT5')) format = TEX_BC3;
    else throw fail('unsupported format ' + String.fromCharCode(code & 0xff, (code >> 8) & 0xff, (code >> 16) & 0xff, code >>> 24));
  } else if (pfFlags & DDPF_RGB && v.getUint32(88, true) === 32) {
    const r = v.getUint32(92, true);
    const g = v.getUint32(96, true);
    const b = v.getUint32(100, true);
    opaque = v.getUint32(104, true) === 0;
    if (r === 0xff && g === 0xff00 && b === 0xff0000) format = TEX_RGBA8;
    else if (r === 0xff0000 && g === 0xff00 && b === 0xff) {
      format = TEX_RGBA8;
      swizzle = true;
    } else throw fail('unsupported RGB channel layout');
  } else {
    throw fail('unsupported pixel format');
  }
  const compressed = format !== TEX_RGBA8;
  if (compressed && (width % 4 !== 0 || height % 4 !== 0)) throw fail(`compressed size ${width}x${height} is not a multiple of 4`);
  const declared = flags & DDSD_MIPMAPCOUNT && mipCount > 0 ? mipCount : 1;
  const blockable = (n: number) => n === 1 || n === 2 || n % 4 === 0;
  const levels: Uint8Array<ArrayBuffer>[] = [];
  for (let l = 0; l < declared; l++) {
    const w = Math.max(1, width >> l);
    const h = Math.max(1, height >> l);
    // WebGL takes compressed levels only at block-friendly sizes
    if (compressed && !(blockable(w) && blockable(h))) break;
    const n = levelBytes(format, w, h);
    if (offset + n > bytes.length) throw fail('truncated data');
    let level = bytes.subarray(offset, offset + n);
    if (swizzle || opaque) {
      level = level.slice();
      for (let i = 0; i < n; i += 4) {
        if (swizzle) {
          const t = level[i]!;
          level[i] = level[i + 2]!;
          level[i + 2] = t;
        }
        if (opaque) level[i + 3] = 255;
      }
    }
    levels.push(level);
    offset += n;
    if (w === 1 && h === 1) break;
  }
  return { format, width, height, levels };
}
