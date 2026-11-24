// The window globals with consumers outside web/: Module and dk64dbg / dk64qs (the dk64-live-debug skill),
// dk64saves (port/src/save.c calls dk64saves.upload) and createDk64 (defined by dk64.js).
import * as dbg from './debug/api';
import type { CreateDk64 } from './emscripten';
import type { Dk64Module } from './port';
import * as qs from './quicksave';
import * as saves from './saves';

const savesApi = {
  select: saves.select,
  boot: saves.boot,
  upload: saves.upload,
  settle: saves.settle,
  putResume: saves.putResume,
  takeResume: saves.takeResume,
  localBytes: saves.localBytes,
  replace: saves.replace,
  path: saves.path,
  scope: saves.scope,
  current: saves.currentSave,
  dashboard: saves.dashboard,
  status: saves.status,
};

const qsApi = {
  save: qs.save,
  load: qs.load,
  bootInto: qs.bootInto,
  started: qs.started,
  status: qs.status,
  autoResume: qs.autoResume,
  forget: qs.forget,
};

declare global {
  interface Window {
    Module?: Dk64Module;
    createDk64?: CreateDk64;
    dk64saves: typeof savesApi;
    dk64dbg: typeof dbg.api;
    dk64qs: typeof qsApi;
  }
}

export function exposeGlobals(): void {
  window.dk64saves = savesApi;
  window.dk64dbg = dbg.api;
  window.dk64qs = qsApi;
}
