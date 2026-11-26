// Bug reports and ideas as prefilled GitHub issue forms (.github/ISSUE_TEMPLATE).
// - collect(): ROM-free game state (EEPROM save, resume point, level progress, version, settings, browser, recent
//   trace and console lines) as a markdown table plus one encoded blob, DK64R1.<base64url(deflate-raw(JSON))>.
//   The RDRAM quicksave is never included: it is ROM-derived memory and issues are public.
// - withMark(): adds the spot the player marked on the screenshot and the renderer's pixel probe there.
// - issueUrl(): the issue form URL, fitted under MAX_URL by dropping trace/log, then probe detail, then text.
// - Crash detection: wasm traps and abort() set `crash`, which opens the Esc menu's crash view (menu/control.ts).
// - load(): developer import (F2 → Load report…): writes the reporter's save and boots into their location.
// tools/report.py decodes the same blob outside the browser.
import { signal } from '@preact/signals';
import * as dbg from './debug/api';
import { currentModule } from './port';
import * as qs from './quicksave';
import * as saves from './saves';
import type { ResumePoint } from './saves';
import * as settings from './settings';
import { toast } from './ui/toast';

const REPO = 'https://github.com/Unstripped/webby-kong';
const MAX_URL = 7500;
const PREFIX = 'DK64R1.';
const SETTING_KEYS = ['drawdist', 'scale', 'msaa', 'a2c', 'mips', 'aniso', 'downsample', 'fxaa', 'sharpen',
  'dither', 'interp', 'audio3d', 'audiorate', 'stickcam', 'camspeed', 'caminvert', 'perpixel', 'kongsmooth'] as const;
const PROBE_CAPS = [6000, 3000, 1500, 0]; // probe chars per blob variant, largest first
const LOG_MAX = 60;
const HIT_RE = /^#\d+ dl=/m;

interface VersionInfo {
  version?: string;
  revision?: string;
}

export interface ReportSettings {
  drawdist?: number;
  scale?: number;
  msaa?: number;
  a2c?: number;
  mips?: number;
  aniso?: number;
  downsample?: number;
  fxaa?: number;
  sharpen?: number;
  dither?: number;
  interp?: number;
  audio3d?: number;
  audiorate?: number;
  stickcam?: number;
  camspeed?: number;
  caminvert?: number;
  touch?: string;
  perpixel?: number;
  kongsmooth?: number;
}

export interface ReportMark {
  fx: number;
  fy: number;
  probe: string | null;
}

// the encoded blob (v 1); tools/report.py reads the same fields
export interface ReportData {
  v: 1;
  version: string | null;
  revision: string | null;
  buildId: string | null;
  save: string | null;
  resume: ResumePoint | null;
  progress: unknown;
  settings: ReportSettings;
  crash: string | null;
  stack: string[] | null;
  mark: ReportMark | null;
}

export interface Report {
  data: ReportData;
  table: Array<[string, string]>;
  trace: string[];
  log: string[];
  blobs: string[];
  place: string | null;
}

let versionInfo: VersionInfo | null | undefined; // undefined until fetched, then {version, revision} or null (source build)
let versionPromise: Promise<VersionInfo | null> = Promise.resolve(null);

let gpu: string | null = null;
function gpuName(): string {
  if (gpu !== null) return gpu;
  gpu = 'unknown';
  try {
    const gl = document.createElement('canvas').getContext('webgl2');
    if (gl) {
      let name: unknown = gl.getParameter(gl.RENDERER);
      const dbgInfo = gl.getExtension('WEBGL_debug_renderer_info');
      if (String(name).includes('WebKit WebGL') && dbgInfo) name = gl.getParameter(dbgInfo.UNMASKED_RENDERER_WEBGL);
      gpu = String(name || 'unknown');
      const lose = gl.getExtension('WEBGL_lose_context');
      if (lose) lose.loseContext();
    }
  } catch {
    gpu = 'unknown';
  }
  return gpu;
}

// ---- console ring
const ring: Array<{ text: string; n: number }> = [];
export function log(text: unknown): void {
  const t = String(text);
  const last = ring[ring.length - 1];
  if (last && last.text === t) {
    last.n++;
    return;
  }
  ring.push({ text: t, n: 1 });
  if (ring.length > LOG_MAX) ring.shift();
}
function ringLines(n: number): string[] {
  return ring.slice(-n).map((e) => (e.n > 1 ? `${e.text} (×${e.n})` : e.text));
}

// ---- crash detection
// the crash the Esc menu shows (menu/control.ts subscribes); set once
export const crash = signal<{ message: string; stack: string[] } | null>(null);

// the trap's call stack, innermost first: wasm frames as `name [function index]+module offset` (the name section
// is in every build; the offset resolves against the same dk64.wasm, e.g. with tools/wasm_funcs.py)
export function stackFrames(stack: unknown): string[] {
  const lines = String(stack || '').split('\n').map((l) => l.trim()).filter((l) => /^at |@/.test(l));
  const wasm: string[] = [];
  for (const l of lines) {
    const at = /wasm-function\[(\d+)\]:(0x[0-9a-f]+)/i.exec(l);
    if (!at) continue;
    const name = (/^at ([^\s(]+) \(/.exec(l) || /^([^@\s]+)@/.exec(l) || [])[1] || '?';
    wasm.push(`${name.replace(/^.*\.wasm\./, '')} [${at[1]}]+${at[2]}`);
  }
  return (wasm.length ? wasm : lines).slice(0, 24);
}

export function crashed(msg: unknown, stack: unknown): void {
  if (!currentModule() || crash.value !== null) return;
  const message = String(msg);
  log('crash: ' + message);
  crash.value = { message, stack: stackFrames(stack) };
}

export function isCrashed(): boolean {
  return crash.value !== null;
}

// version.json and the crash listeners (main.tsx)
export function installReport(): void {
  versionPromise = fetch('version.json', { cache: 'no-store' })
    .then((r) => (r.ok ? (r.json() as Promise<unknown>) : null))
    .catch(() => null)
    .then((v) => {
      versionInfo = v && typeof v === 'object' ? (v as VersionInfo) : null;
      return versionInfo;
    });
  window.addEventListener('error', (e) => {
    log('error: ' + e.message);
    if (e.error instanceof WebAssembly.RuntimeError) crashed(e.error.message, e.error.stack);
  });
  window.addEventListener('unhandledrejection', (e) => {
    const r: unknown = e.reason;
    log('error: ' + (r instanceof Error && r.message ? r.message : String(r)));
    if (r instanceof WebAssembly.RuntimeError) crashed(r.message, r.stack);
  });
}

// ---- encoding
export function b64url(bytes: Uint8Array): string {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
export function unb64url(text: string): Uint8Array<ArrayBuffer> {
  const s = atob(text.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - (text.length % 4)) % 4));
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}
export async function encode(obj: unknown): Promise<string> {
  const bytes = new TextEncoder().encode(JSON.stringify(obj));
  const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));
  return PREFIX + b64url(new Uint8Array(await new Response(stream).arrayBuffer()));
}
export async function decode(text: unknown): Promise<ReportData> {
  const m = /DK64R1\.([A-Za-z0-9_-]+)/.exec(String(text));
  if (!m) throw new Error('no DK64R1 report data found');
  const stream = new Blob([unb64url(m[1]!)]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  const obj: unknown = JSON.parse(await new Response(stream).text());
  if (!obj || typeof obj !== 'object' || !('v' in obj) || obj.v !== 1) throw new Error('unsupported report version');
  return obj as ReportData;
}

function hex(n: number): string {
  return (n >>> 0).toString(16);
}
function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, (x) => x.toString(16).padStart(2, '0')).join('');
}
function fromHex(h: string): Uint8Array {
  const out = new Uint8Array(h.length >> 1);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.substr(i * 2, 2), 16);
  return out;
}

function versionText(): string {
  const v = versionInfo;
  if (!v || !v.version) return 'source build';
  return v.revision && v.revision !== 'unknown' ? `${v.version} (${String(v.revision).slice(0, 7)})` : v.version;
}

export function settingsText(s: ReportSettings): string {
  const parts: string[] = [];
  if (s.drawdist != null) parts.push(`draw distance ${s.drawdist}×`);
  if (s.scale != null) parts.push(`render scale ${s.scale === 0 ? 'auto' : s.scale + '×'}`);
  if (s.msaa != null) parts.push(`MSAA ${s.msaa > 1 ? s.msaa + '×' : 'off'}`);
  if (s.a2c != null) parts.push(`cutout edges ${s.a2c ? 'smooth' : 'hard'}`);
  if (s.mips != null) parts.push(`texture mips ${s.mips ? 'on' : 'off'}`);
  if (s.aniso != null) parts.push(`anisotropy ${s.aniso > 1 ? s.aniso + '×' : 'off'}`);
  if (s.downsample != null) parts.push(`downsample ${s.downsample ? 'area' : 'bilinear'}`);
  if (s.fxaa) parts.push('FXAA');
  if (s.sharpen) parts.push(`sharpening ${s.sharpen}`);
  if (s.dither) parts.push('dither');
  if (s.interp != null) parts.push(`${30 << s.interp} fps`);
  if (s.perpixel) parts.push('per-pixel lighting');
  if (s.kongsmooth) parts.push('kong smoothing');
  if (s.audio3d != null) parts.push(`3D sound ${s.audio3d ? 'on' : 'off'}`);
  if (s.audiorate != null) parts.push(`audio ${s.audiorate / 1000} kHz`);
  if (s.stickcam != null) parts.push(`right-stick camera ${s.stickcam ? 'on' : 'off'}`);
  if (s.stickcam && s.camspeed != null) parts.push(`camera speed ${s.camspeed}`);
  if (s.stickcam && s.caminvert) parts.push(`camera invert ${['', 'horizontal', 'vertical', 'both'][s.caminvert]}`);
  if (s.touch != null) parts.push(`touch ${s.touch}`);
  return parts.join(' · ') || '—';
}

function inputText(): string {
  try {
    if (typeof navigator.getGamepads === 'function') {
      const gp = Array.from(navigator.getGamepads()).find((g) => g && g.connected);
      if (gp) return 'gamepad: ' + gp.id;
    }
  } catch {
    // insecure origins: getGamepads missing or throwing
  }
  return navigator.maxTouchPoints > 0 ? 'touch' : 'keyboard';
}

export function hitCount(probe: string | null | undefined): number {
  return probe ? (probe.match(/^#\d+ dl=/gm) || []).length : 0;
}

export function markRow(mark: { fx: number; fy: number; hasProbe: boolean }, hits: number): [string, string] {
  const at = `(${mark.fx.toFixed(2)}, ${mark.fy.toFixed(2)})`;
  return ['Marked spot', mark.hasProbe ? `${at} · ${hits} triangles probed` : `${at} · probe unavailable`];
}

// ---- collect
export async function collect({
  crash: crashText = null,
  level = null,
  kong = null,
  progress = null,
}: { crash?: string | null; level?: string | null; kong?: string | null; progress?: unknown } = {}): Promise<Report> {
  const m = currentModule();
  function attempt<T>(fn: () => T): T | null;
  function attempt<T>(fn: () => T, fallback: T): T;
  function attempt<T>(fn: () => T, fallback: T | null = null): T | null {
    try {
      return fn();
    } catch {
      return fallback;
    }
  }
  if (!crashText) attempt(() => m!.ccall('port_save_flush_now', null));
  const savePath = saves.path();
  const save = attempt(() => (m!.FS.analyzePath(savePath).exists ? toHex(m!.FS.readFile(savePath)) : null));
  let resumeReason: string | null = null;
  const resume = attempt((): ResumePoint | null => {
    const r = JSON.parse(m!.ccall('port_resume_capture', 'string') as string);
    if (!r.ok) {
      resumeReason = r.reason || 'unavailable';
      return null;
    }
    const { slot, kong: k, map, exit, hasPos, x, y, z, yaw } = r;
    return { slot, kong: k, map, exit, hasPos, x, y, z, yaw };
  });
  if (!resume && resumeReason === null) resumeReason = 'unavailable';
  const prog: unknown = progress || attempt(() => JSON.parse(m!.ccall('port_level_progress', 'string') as string));
  const state = attempt(() => dbg.state());
  const trace = attempt(() => dbg.trace(0).events.slice(-30).map((e) => 'vi ' + e.vi + '  ' + e.text), []);
  const logLines = ringLines(30);
  const version = await versionPromise;
  const current: Record<string, unknown> = {};
  for (const k of [...SETTING_KEYS, 'touch'] as const) {
    const v = attempt(() => settings.get(k));
    if (v !== null && v !== undefined) current[k] = v;
  }
  const dataSettings: Record<string, unknown> = {};
  for (const k of SETTING_KEYS) if (k in current) dataSettings[k] = current[k];
  const crashStack = crash.value?.stack ?? null;

  const data: ReportData = {
    v: 1,
    version: (version && version.version) || null,
    revision: (version && version.revision) || null,
    buildId: attempt(() => m!.portBuildId) || null,
    save,
    resume,
    progress: prog,
    settings: dataSettings as ReportSettings,
    crash: crashText,
    stack: crashText && crashStack && crashStack.length ? crashStack : null,
    mark: null,
  };

  const map = state && Number.isInteger(state.map) ? state.map : resume ? resume.map : null;
  const where: string[] = [];
  if (level) where.push(level);
  if (map !== null) where.push('map 0x' + hex(map));
  if (kong) where.push(kong);
  const p = state && state.player;
  if (p && Number.isFinite(p.x)) where.push(`(${Math.round(p.x)}, ${Math.round(p.y)}, ${Math.round(p.z)})`);

  const table: Array<[string, string]> = [
    ['Version', versionText()],
    ['Build', data.buildId ? data.buildId.slice(0, 12) : '—'],
    ['Where', where.join(' · ') || '—'],
    [
      'Game',
      state ? `mode ${state.mode} · overlay ${state.overlay} · frame ${state.frames} · actors ${state.actors}` : '—',
    ],
    ['Save file', save ? 'included' : 'none yet'],
    ['Resume point', resume ? 'included' : `no (${resumeReason})`],
    ['Settings', settingsText(current as ReportSettings)],
    ['Input', inputText()],
    ['Browser', navigator.userAgent],
    ['GPU', gpuName()],
    ['Screen', `${innerWidth}×${innerHeight} @${devicePixelRatio}x`],
    ['Save server', attempt(() => saves.status().server, '—')],
  ];
  const err = crashText || (state && state.lastError);
  if (err) table.push(['Error', String(err).slice(0, 500)]);

  let blob: string | null;
  try {
    blob = await encode(data);
  } catch {
    blob = null;
  }
  return {
    data,
    table,
    trace,
    log: logLines,
    blobs: blob ? [blob] : [],
    place: level || (map !== null ? 'map 0x' + hex(map) : null),
  };
}

// keeps the topmost (last-drawn) hits within cap characters
export function trimProbe(text: string | null | undefined, cap: number): string | null {
  if (text === null || text === undefined || cap === 0) return null;
  if (text.length <= cap) return text;
  const parts = text.split(/(?=^#\d+ dl=)/m);
  const head = HIT_RE.test(parts[0]!) ? '' : parts.shift()!;
  const hits = parts;
  const kept: string[] = [];
  let total = 0;
  for (let i = hits.length - 1; i >= 0; i--) {
    if (total + hits[i]!.length > cap) break;
    kept.unshift(hits[i]!);
    total += hits[i]!.length;
  }
  if (!kept.length && hits.length) {
    kept.push(hits[hits.length - 1]!.slice(0, cap));
  }
  const dropped = hits.length - kept.length;
  const prefix = dropped > 0 ? `(${dropped} earlier hits omitted)\n` : head.length + total <= cap ? head : '';
  return prefix + kept.join('');
}

export async function withMark(
  report: Report,
  { fx, fy, probe }: { fx: number; fy: number; probe: unknown },
): Promise<Report> {
  const p = typeof probe === 'string' ? probe : null;
  const mark = { fx, fy, probe: trimProbe(p, PROBE_CAPS[0]!) };
  const data = { ...report.data, mark };
  const blobs: string[] = [];
  for (const cap of PROBE_CAPS) {
    try {
      blobs.push(await encode({ ...report.data, mark: { fx, fy, probe: trimProbe(p, cap) } }));
    } catch {
      // keep the variants that encoded
    }
  }
  const table = report.table.filter((r) => r[0] !== 'Marked spot');
  const row = markRow({ fx, fy, hasProbe: p !== null }, hitCount(p));
  const errAt = table.findIndex((r) => r[0] === 'Error');
  if (errAt < 0) table.push(row);
  else table.splice(errAt, 0, row);
  return { ...report, data, table, blobs: blobs.length ? blobs : report.blobs };
}

// ---- issue URL
function cell(v: unknown): string {
  return String(v).replace(/\|/g, '\\|').replace(/\r?\n/g, ' ');
}
function fence(lines: unknown[]): string[] {
  return lines.map((l) => String(l).replace(/`{3,}/g, "'''"));
}
function details(summary: string, lines: unknown[]): string {
  if (!lines.length) return '';
  return `<details><summary>${summary}</summary>\n\n\`\`\`text\n${fence(lines).join('\n')}\n\`\`\`\n\n</details>`;
}
function markdown(report: Report, blob: string | null, nTrace: number, nLog: number): string {
  const parts = [
    ['| | |', '| --- | --- |', ...report.table.map(([k, v]) => `| **${cell(k)}** | ${cell(v)} |`)].join('\n'),
  ];
  parts.push(details('Crash stack', report.data.stack || []));
  parts.push(details('Recent game events', nTrace ? report.trace.slice(-nTrace) : []));
  parts.push(details('Console', nLog ? report.log.slice(-nLog) : []));
  if (blob) {
    parts.push(details('Save, location &amp; probe (developers: F2 → Load report…, or tools/report.py)', [blob]));
  }
  return parts.filter(Boolean).join('\n\n');
}

function firstLine(text: unknown, n = 60): string {
  const l = String(text || '').split('\n')[0]!.trim();
  return l.length > n ? l.slice(0, n - 1) + '…' : l;
}

// text cut in 10% steps (with …) until the URL fits; the empty text as the last resort
export function fitText(text: string, build: (t: string) => string): string {
  let url = build(text);
  for (let f = 0.9; url.length > MAX_URL && f > 0.05; f -= 0.1) {
    url = build(text.slice(0, Math.floor(text.length * f)) + '…');
  }
  return url.length <= MAX_URL ? url : build('');
}

export function issueUrl(kind: 'bug' | 'idea' | 'crash', rawText: unknown, report?: Report | null): string {
  const text = String(rawText || '');
  if (kind === 'idea') {
    return fitText(text, (t) =>
      REPO + '/issues/new?' +
      new URLSearchParams({ template: 'idea.yml', title: 'Idea: ' + firstLine(t), idea: t, version: versionText() }),
    );
  }
  const crashText =
    kind === 'crash' ? String((report && report.data.crash) || crash.value?.message || 'unknown error').slice(0, 500) : null;
  let title: string;
  if (crashText) title = 'Crash: ' + firstLine(crashText);
  else title = 'Bug: ' + (firstLine(text) || (report && report.place) || 'report');
  const build = (body: string, md: string | null) => {
    const what = crashText ? (body ? body + '\n\n' : '') + 'Crashed with: ' + crashText : body;
    const params: Record<string, string> = { template: 'bug_report.yml', title, what };
    if (md !== null) params.report = md;
    return REPO + '/issues/new?' + new URLSearchParams(params);
  };
  if (!report) return fitText(text, (t) => build(t, null));
  const blobs: Array<string | null> = report.blobs.length ? report.blobs : [null];
  const tries: Array<[string | null, number, number]> = [[30, 30], [15, 15], [5, 5], [0, 0]].map(
    ([t, l]) => [blobs[0]!, t!, l!],
  );
  for (const b of blobs.slice(1)) tries.push([b, 0, 0]);
  for (const [b, t, l] of tries) {
    const url = build(text, markdown(report, b, t, l));
    if (url.length <= MAX_URL) return url;
  }
  const md = markdown(report, blobs[blobs.length - 1]!, 0, 0);
  const url = fitText(text, (t) => build(t, md));
  return url.length <= MAX_URL ? url : fitText(text, (t) => build(t, null));
}

// ---- images (clipboard writes take PNG only; dbg.capture() returns JPEG)
function canvasPng(c: HTMLCanvasElement): Promise<Blob> {
  return new Promise((resolve, reject) =>
    c.toBlob((b) => (b ? resolve(b) : reject(new Error('PNG encode failed'))), 'image/png'),
  );
}
async function draw(src: Blob): Promise<{ c: HTMLCanvasElement; ctx: CanvasRenderingContext2D }> {
  const bmp = await createImageBitmap(src);
  const c = document.createElement('canvas');
  c.width = bmp.width;
  c.height = bmp.height;
  const ctx = c.getContext('2d')!;
  ctx.drawImage(bmp, 0, 0);
  bmp.close();
  return { c, ctx };
}
export async function toPng(dataUrl: string): Promise<Blob> {
  const { c } = await draw(await (await fetch(dataUrl)).blob());
  return canvasPng(c);
}
export async function markPng(blob: Blob, fx: number, fy: number): Promise<Blob> {
  const { c, ctx } = await draw(blob);
  const r = Math.max(14, 0.035 * c.width);
  for (const [color, width] of [['#fff', 7], ['#ff2d2d', 4]] as const) {
    ctx.beginPath();
    ctx.arc(fx * c.width, fy * c.height, r, 0, Math.PI * 2);
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.stroke();
  }
  return canvasPng(c);
}

// ---- developer import
export async function load(text: string): Promise<void> {
  const r = await decode(text);
  const m = currentModule();
  if (!m || !m.FS) throw new Error('start the game first');
  if (
    !confirm(
      "Load this report? It replaces this browser's game save with the reporter's (your current save stays in " +
        'the save server history) and reloads into their location.',
    )
  ) {
    return;
  }
  if (r.save) {
    await saves.replace(m, fromHex(r.save));
  } else {
    m.ccall('port_set_paused', null, ['number'], [1]);
    m.ccall('port_save_flush_now', null);
    toast('Report has no save file; loading its location only');
  }
  qs.bootInto(r.save ? r.resume : null, (r.settings || {}) as Record<string, unknown>);
}
