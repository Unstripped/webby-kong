// dk64.js is the unbundled Emscripten glue (a classic script defining the global createDk64); it is loaded on start.
import type { Dk64Module } from './port';

export interface CreateDk64Options {
  canvas: HTMLCanvasElement;
  instantiateWasm: (imports: WebAssembly.Imports, done: (instance: WebAssembly.Instance) => void) => object;
  preRun: Array<(m: Dk64Module) => void>;
  print: (text: string) => void;
  printErr: (text: string) => void;
  onAbort: (what: unknown) => void;
}

export type CreateDk64 = (opts: CreateDk64Options) => Promise<Dk64Module>;

export function loadDk64(): Promise<CreateDk64> {
  if (window.createDk64) return Promise.resolve(window.createDk64);
  return new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = 'dk64.js';
    script.onload = () => {
      if (window.createDk64) resolve(window.createDk64);
      else reject(new Error('dk64.js did not define createDk64'));
    };
    script.onerror = () => reject(new Error('dk64.js failed to load'));
    document.head.append(script);
  });
}
