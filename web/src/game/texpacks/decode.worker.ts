// Pack image decoder, off the main thread: reads a pack file and sniffs it as RT64 does (the extension is ignored).
// PNG decodes to an unpremultiplied ImageBitmap; DDS is parsed into its mip levels.
import { parseDds } from './dds';
import type { DecodeRequest, DecodeResult } from './decode';
import { readLocator } from './source';

const scope = self as unknown as {
  onmessage: ((e: MessageEvent<DecodeRequest>) => void) | null;
  postMessage(m: DecodeResult, transfer: Transferable[]): void;
};

async function decode(req: DecodeRequest): Promise<void> {
  const { id } = req;
  try {
    const b = await readLocator(req);
    if (b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47) {
      const bitmap = await createImageBitmap(new Blob([b]), {
        premultiplyAlpha: 'none',
        colorSpaceConversion: 'none',
        imageOrientation: 'none',
      });
      scope.postMessage({ id, kind: 'bitmap', bitmap }, [bitmap]);
    } else if (b[0] === 0x44 && b[1] === 0x44 && b[2] === 0x53 && b[3] === 0x20) {
      const img = parseDds(b);
      const data = new Uint8Array(img.levels.reduce((n, l) => n + l.length, 0));
      let at = 0;
      for (const l of img.levels) {
        data.set(l, at);
        at += l.length;
      }
      const { format, width, height } = img;
      scope.postMessage({ id, kind: 'dds', format, width, height, levels: img.levels.length, data: data.buffer }, [
        data.buffer,
      ]);
    } else {
      scope.postMessage({ id, kind: 'error', message: 'Unknown image format (not PNG or DDS)' }, []);
    }
  } catch (err) {
    scope.postMessage({ id, kind: 'error', message: err instanceof Error ? err.message : String(err) }, []);
  }
}

scope.onmessage = (e) => void decode(e.data);
