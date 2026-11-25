// Messages of the pack image decoder (decode.worker.ts): a request names a file of a pack (source.ts Locator); the
// result is a PNG as an ImageBitmap, a DDS's mip levels back to back, or why it could not be read.
import type { Locator } from './source';

export interface DecodeRequest extends Locator {
  id: number;
}

export type DecodeResult =
  | { id: number; kind: 'bitmap'; bitmap: ImageBitmap }
  | { id: number; kind: 'dds'; format: number; width: number; height: number; levels: number; data: ArrayBuffer }
  | { id: number; kind: 'error'; message: string };
