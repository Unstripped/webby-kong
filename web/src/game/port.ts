import type { AudioPackRuntime } from './audiopack/runtime';

// The Emscripten module (dk64.js, MODULARIZE createDk64) and typed ccall wrappers for the C exports the page calls.

// -sEXPORTED_FUNCTIONS (CMakeLists.txt) without the leading _, plus the EMSCRIPTEN_KEEPALIVE-only exports
export type PortExport =
  | 'port_debug_stats'
  | 'port_debug_audio'
  | 'port_save_flush_now'
  | 'port_debug_trace'
  | 'port_debug_trace_mask'
  | 'port_debug_capture_request'
  | 'port_debug_capture_seq'
  | 'port_debug_capture_pixels'
  | 'port_debug_capture_width'
  | 'port_debug_capture_height'
  | 'port_qs_table'
  | 'port_qs_size'
  | 'port_qs_save_begin'
  | 'port_qs_save_end'
  | 'port_qs_load_begin'
  | 'port_qs_load_end'
  | 'port_resume_request'
  | 'port_resume_capture'
  | 'port_touch_pad'
  | 'port_level_progress'
  | 'port_set_paused'
  | 'port_set_drawdist'
  | 'port_set_scale'
  | 'port_set_msaa'
  | 'port_gfx_max_samples'
  | 'port_set_audio_hrtf'
  | 'port_audio_rate'
  | 'port_set_interp'
  | 'port_frame_step'
  | 'port_save_request'
  | 'port_save_busy'
  | 'port_set_stick_camera'
  | 'port_set_camera_speed'
  | 'port_set_camera_invert'
  | 'port_set_kong_swap'
  | 'port_set_disco_chunky'
  | 'port_debug_probe'
  | 'port_debug_probe_result';

type CType = 'number' | 'string' | null;

export interface EmscriptenFS {
  mkdir(path: string): void;
  mount(type: unknown, opts: object, mountpoint: string): void;
  syncfs(populate: boolean, callback: (err: unknown) => void): void;
  readFile(path: string): Uint8Array<ArrayBuffer>;
  writeFile(path: string, data: Uint8Array | string): void;
  stat(path: string): { mtime: Date };
  utime(path: string, atime: number, mtime: number): void;
  analyzePath(path: string): { exists: boolean };
}

export interface Dk64Module {
  ccall(name: PortExport, returnType: CType, argTypes?: CType[], args?: unknown[]): unknown;
  callMain(args: string[]): void;
  FS: EmscriptenFS;
  IDBFS: unknown;
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  addRunDependency(id: string): void;
  removeRunDependency(id: string): void;
  SDL2?: { audioContext?: AudioContext };
  portBuildId: string;
  portRom: Uint8Array;
  portAudioPack?: AudioPackRuntime; // audiopack/runtime.ts, read by port/src/audio/pack.c
}

let current: Dk64Module | null = null;

export function setModule(m: Dk64Module | null): void {
  current = m;
}

export function currentModule(): Dk64Module | null {
  return current;
}

// the running module; `who` names the API in the error (the messages debug.js / quicksave.js threw)
export function requireModule(who = 'dk64dbg'): Dk64Module {
  if (!current) throw new Error(`${who}: game not started`);
  return current;
}

const numbers = (args: number[]): CType[] => args.map(() => 'number');

export function callNumber(name: PortExport, ...args: number[]): number {
  return requireModule().ccall(name, 'number', numbers(args), args) as number;
}

export function callString(name: PortExport, ...args: number[]): string {
  return requireModule().ccall(name, 'string', numbers(args), args) as string;
}

export function callVoid(name: PortExport, ...args: number[]): void {
  requireModule().ccall(name, null, numbers(args), args);
}
