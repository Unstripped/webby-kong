// Live-debug layer: the window.dk64dbg API (used by the dk64-live-debug skill), F8 marks, Alt+click probes and the
// F2 / F5 / F9 keys. The F2 panel is DebugPanel.tsx; quicksave is ../quicksave.ts.
import { signal } from '@preact/signals';
import { requireModule } from '../port';
import * as qs from '../quicksave';
import * as report from '../report';
import { toast } from '../ui/toast';

const MAX_MARKS = 20;
const MASK_KEY = 'dk64dbg.traceMask';

export interface DebugStats {
  map: number;
  mode: number;
  overlay: number;
  vi: number;
  frames: number;
  actors: number;
  player: { x: number; y: number; z: number; yaw: number; chunk: number } | null;
  lastError?: string | null;
}

export interface TraceEvent {
  vi: number;
  text: string;
}

export interface Trace {
  next: number;
  events: TraceEvent[];
}

export interface Mark {
  id: number;
  time: string;
  note: string;
  state: DebugStats;
  trace: TraceEvent[];
  image: string | null;
  probe?: string;
}

const marksList: Mark[] = [];
let nextMarkId = 1;

// F2 panel visibility; refreshNonce re-renders the open panel after an action (refreshIfVisible)
export const overlayVisible = signal(false);
export const refreshNonce = signal(0);

function refreshIfVisible(): void {
  if (overlayVisible.value) refreshNonce.value++;
}

export function state(): DebugStats {
  return JSON.parse(requireModule().ccall('port_debug_stats', 'string') as string);
}

// sequence id per sequence player (-1: stopped) and the last sound ids played, newest first (port/src/audio/pack.c)
export function audio(): { seq: number[]; sfx: number[] } {
  return JSON.parse(requireModule().ccall('port_debug_audio', 'string') as string);
}

export function trace(since = 0): Trace {
  return JSON.parse(requireModule().ccall('port_debug_trace', 'string', ['number'], [since >>> 0]) as string);
}

// setting a mask also remembers it, so it applies from boot on the next load
export function traceMask(mask?: number): number {
  const set = mask === undefined ? -1 : mask | 0;
  const cur = (requireModule().ccall('port_debug_trace_mask', 'number', ['number'], [set]) as number) >>> 0;
  if (mask !== undefined) {
    try {
      localStorage.setItem(MASK_KEY, String(cur));
    } catch {
      // storage unavailable: the mask still applies to this session
    }
  }
  return cur;
}

export function capture(): Promise<string> {
  const m = requireModule();
  const seq0 = m.ccall('port_debug_capture_seq', 'number');
  m.ccall('port_debug_capture_request', null);
  const start = performance.now();
  return new Promise((resolve, reject) => {
    function poll() {
      if (m.ccall('port_debug_capture_seq', 'number') === seq0) {
        if (performance.now() - start > 1000) {
          reject(new Error('dk64dbg: capture timed out'));
        } else {
          requestAnimationFrame(poll);
        }
        return;
      }
      const w = m.ccall('port_debug_capture_width', 'number') as number;
      const h = m.ccall('port_debug_capture_height', 'number') as number;
      const p = (m.ccall('port_debug_capture_pixels', 'number') as number) >>> 0;
      const src = m.HEAPU8.subarray(p, p + w * h * 4);
      const img = new ImageData(w, h);
      const row = w * 4;
      for (let y = 0; y < h; y++) {
        // GL rows are bottom-up
        img.data.set(src.subarray((h - 1 - y) * row, (h - y) * row), y * row);
      }
      const full = document.createElement('canvas');
      full.width = w;
      full.height = h;
      full.getContext('2d')!.putImageData(img, 0, 0);
      const out = document.createElement('canvas');
      out.width = Math.min(w, 960);
      out.height = Math.round((h * out.width) / w);
      out.getContext('2d')!.drawImage(full, 0, 0, out.width, out.height);
      resolve(out.toDataURL('image/jpeg', 0.8));
    }
    requestAnimationFrame(poll);
  });
}

// RDP state of every triangle drawn through a point of the canvas (fractions of its size, top-left origin), recorded
// during the next frame: display-list path, modes, combiner, tiles, recent texture loads, vertices
export function probe(fx: number, fy: number): Promise<string> {
  const m = requireModule();
  if (!m.ccall('port_debug_probe', 'number', ['number', 'number'], [fx, fy])) {
    return Promise.reject(new Error('dk64dbg: point is off the game image'));
  }
  const start = performance.now();
  return new Promise((resolve, reject) => {
    function poll() {
      const r = m.ccall('port_debug_probe_result', 'string') as string;
      if (r) {
        resolve(r);
      } else if (performance.now() - start > 2000) {
        reject(new Error('dk64dbg: probe timed out'));
      } else {
        requestAnimationFrame(poll);
      }
    }
    requestAnimationFrame(poll);
  });
}

// extra: fields merged into the mark (Alt+click adds `probe`)
export async function mark(note = '', extra: Partial<Mark> = {}): Promise<Mark> {
  const cur = trace(0);
  const m: Mark = {
    id: nextMarkId++,
    time: new Date().toISOString(),
    note,
    state: state(),
    trace: trace(Math.max(0, cur.next - 50)).events,
    image: null,
    ...extra,
  };
  try {
    m.image = await capture();
  } catch {
    m.image = null;
  }
  marksList.push(m);
  if (marksList.length > MAX_MARKS) {
    marksList.shift();
  }
  toast(`Mark #${m.id} saved`);
  refreshIfVisible();
  return m;
}

// Alt+click on the game: a mark carrying the probe of the clicked point
export async function probeMark(fx: number, fy: number): Promise<Mark> {
  const p = await probe(fx, fy);
  return mark(`probe ${fx.toFixed(3)},${fy.toFixed(3)}`, { probe: p });
}

export function marks(): Mark[] {
  return marksList;
}

// downloads every mark plus the full trace ring as dk64-marks-<local time>.json; returns the file name
export function exportMarks(): string {
  const now = new Date();
  const pad = (n: number) => String(n).padStart(2, '0');
  const name = `dk64-marks-${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}.json`;
  const data: { when: string; state: DebugStats | null; mask: number | null; marks: Mark[]; trace: Trace | null } = {
    when: now.toISOString(),
    state: null,
    mask: null,
    marks: marksList,
    trace: null,
  };
  try {
    data.state = state();
    data.mask = traceMask();
    data.trace = trace(0);
  } catch {
    // game not started: export the marks alone
  }
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([JSON.stringify(data)], { type: 'application/json' }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  toast(`Exported ${marksList.length} mark${marksList.length === 1 ? '' : 's'} → ${name}`);
  return name;
}

export function clearMarks(): void {
  marksList.length = 0;
  refreshIfVisible();
}

// called by boot.ts after the module is set and before callMain, so the remembered mask covers boot
export function moduleReady(): void {
  let saved: string | null = null;
  try {
    saved = localStorage.getItem(MASK_KEY);
  } catch {
    return;
  }
  if (saved !== null) {
    traceMask(Number(saved) >>> 0);
  }
}

// F2: toggles without an argument; returns whether the panel shows
export function overlay(on?: boolean): boolean {
  const show = on === undefined ? !overlayVisible.value : !!on;
  overlayVisible.value = show;
  return show;
}

export function quicksave(): void {
  qs.save()
    .catch((err: Error) => toast('Quicksave failed: ' + err.message))
    .finally(refreshIfVisible);
}

export function quickload(): void {
  qs.load()
    .catch((err: Error) => toast('Quickload failed: ' + err.message))
    .finally(refreshIfVisible);
}

export function toggleAutoResume(): void {
  qs.autoResume(!qs.autoResume());
  refreshIfVisible();
}

export function toggleTraceBit(bit: number): void {
  try {
    traceMask(traceMask() ^ bit);
  } catch (e) {
    toast((e as Error).message);
  }
  refreshIfVisible();
}

// a bug report from GitHub (report.ts): the reporter's save, location and settings
export function loadReport(): void {
  const t = prompt('Paste a bug report: the whole issue, or its DK64R1… line');
  if (!t) return;
  report.load(t).catch((err: Error) => toast('Load report failed: ' + err.message));
}

// F8 and the panel's ● Mark
export function markOrToast(): void {
  mark().catch((err: Error) => toast('Mark failed: ' + err.message));
}

// the window.dk64dbg object (the dk64-live-debug skill depends on these names)
export const api = {
  state,
  trace,
  traceMask,
  capture,
  mark,
  probe,
  marks,
  clearMarks,
  exportMarks,
  moduleReady,
  overlay,
  toast,
};

export function installDebugKeys(): void {
  // capture phase: runs before SDL's keydown listener
  window.addEventListener(
    'keydown',
    (e) => {
      if (e.key !== 'F2' && e.key !== 'F8' && e.key !== 'F5' && e.key !== 'F9') {
        return;
      }
      e.preventDefault();
      if (e.repeat) {
        return;
      }
      if (e.key === 'F2') {
        overlay();
      } else if (e.key === 'F5') {
        quicksave();
      } else if (e.key === 'F9') {
        quickload();
      } else {
        markOrToast();
      }
    },
    true,
  );

  // Alt+click on the game canvas: probe mark of that point (capture phase, so SDL never sees the click)
  window.addEventListener(
    'mousedown',
    (e) => {
      const c = document.getElementById('canvas');
      if (!c || !e.altKey || e.button !== 0 || e.target !== c) {
        return;
      }
      e.preventDefault();
      e.stopPropagation();
      const r = c.getBoundingClientRect();
      probeMark((e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height).catch((err: Error) =>
        toast('Probe failed: ' + err.message),
      );
    },
    true,
  );
}
