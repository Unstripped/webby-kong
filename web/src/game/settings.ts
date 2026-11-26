// Player settings (draw distance, render scale, MSAA, cutout anti-aliasing, texture mips and anisotropy,
// present pass, 60 fps, touch controller, 3D sound, audio rate, kong swap, Disco Chunky, right-stick camera):
// stored in localStorage `dk64.settings`, a URL parameter overrides the stored value for that page load; the Esc menu
// changes them live, except the audio rate, which the game reads at boot (it applies on the next load).
import { signal } from '@preact/signals';
import { callNumber, currentModule, type PortExport } from './port';

export type TouchMode = 'auto' | 'on' | 'off';

export interface SettingsValues {
  drawdist: number;
  scale: number;
  msaa: number;
  a2c: number;
  mips: number;
  aniso: number;
  interp: number;
  touch: TouchMode;
  audio3d: number;
  audiorate: number;
  kongswap: number;
  discochunky: number;
  stickcam: number;
  camspeed: number;
  caminvert: number;
  perpixel: number;
  kongsmooth: number;
  downsample: number;
  sharpen: number;
  fxaa: number;
  dither: number;
}

export type SettingKey = keyof SettingsValues;

interface Def<V> {
  def: V;
  valid: (v: unknown) => v is V;
  parse: (s: string) => unknown;
  fn: PortExport | null;
}

const KEY = 'dk64.settings';
const num = (s: string) => (s === '' ? NaN : Number(s));
const isNum = (ok: (v: number) => boolean) => (v: unknown): v is number => typeof v === 'number' && ok(v);

const DEFS: { [K in SettingKey]: Def<SettingsValues[K]> } = {
  drawdist: { def: 4, valid: isNum((v) => Number.isFinite(v) && v >= 1 && v <= 16), parse: num, fn: 'port_set_drawdist' },
  scale: { def: 0, valid: isNum((v) => Number.isInteger(v) && v >= 0 && v <= 8), parse: num, fn: 'port_set_scale' },
  msaa: { def: 4, valid: isNum((v) => [0, 1, 2, 4, 8].includes(v)), parse: num, fn: 'port_set_msaa' },
  a2c: { def: 1, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_a2c' },
  mips: { def: 1, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_mips' },
  aniso: { def: 4, valid: isNum((v) => [0, 2, 4, 8, 16].includes(v)), parse: num, fn: 'port_set_aniso' },
  interp: { def: 1, valid: isNum((v) => [0, 1, 2, 3].includes(v)), parse: num, fn: 'port_set_interp' },
  touch: {
    def: 'auto',
    valid: (v): v is TouchMode => v === 'auto' || v === 'on' || v === 'off',
    parse: (s) => (s === '1' ? 'on' : s === '0' ? 'off' : s),
    fn: null,
  },
  audio3d: { def: 1, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_audio_hrtf' },
  audiorate: { def: 44100, valid: isNum((v) => v === 22050 || v === 44100), parse: num, fn: null },
  kongswap: { def: 1, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_kong_swap' },
  discochunky: { def: 0, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_disco_chunky' },
  stickcam: { def: 1, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_stick_camera' },
  camspeed: { def: 5, valid: isNum((v) => Number.isInteger(v) && v >= 1 && v <= 10), parse: num, fn: 'port_set_camera_speed' },
  caminvert: { def: 0, valid: isNum((v) => [0, 1, 2, 3].includes(v)), parse: num, fn: 'port_set_camera_invert' },
  perpixel: { def: 0, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_per_pixel' },
  kongsmooth: { def: 0, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_kong_smooth' },
  downsample: { def: 1, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_downsample' },
  sharpen: { def: 0, valid: isNum((v) => Number.isInteger(v) && v >= 0 && v <= 10), parse: num, fn: 'port_set_sharpen' },
  fxaa: { def: 0, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_fxaa' },
  dither: { def: 0, valid: isNum((v) => v === 0 || v === 1), parse: num, fn: 'port_set_dither' },
};

const KEYS = Object.keys(DEFS) as SettingKey[];

function defaults(): SettingsValues {
  const out: Partial<Record<SettingKey, unknown>> = {};
  for (const k of KEYS) out[k] = DEFS[k].def;
  return out as SettingsValues;
}

let stored: Record<string, unknown> = {};
let isStarted = false;
const listeners: { [K in SettingKey]?: Array<(v: SettingsValues[K]) => void> } = {};

// the current values; the Esc menu and the touch overlay render from it
export const values = signal<SettingsValues>(defaults());

// stored values, then URL overrides (main.tsx, first)
export function loadSettings(): void {
  stored = {};
  isStarted = false;
  try {
    const parsed: unknown = JSON.parse(localStorage.getItem(KEY) as string);
    if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) stored = parsed as Record<string, unknown>;
  } catch (err) {
    console.warn('settings: ignoring stored settings', err);
  }
  const next: Record<string, unknown> = {};
  const params = new URLSearchParams(location.search);
  for (const k of KEYS) {
    const d = DEFS[k];
    next[k] = d.valid(stored[k]) ? stored[k] : d.def;
    const raw = params.get(k);
    if (raw !== null) {
      const v = d.parse(raw);
      if (d.valid(v)) next[k] = v;
      else console.warn(`settings: ignoring ${k}=${raw}`);
    }
  }
  values.value = next as unknown as SettingsValues;
}

export function get<K extends SettingKey>(key: K): SettingsValues[K] {
  return values.value[key];
}

// a side effect of a live change besides the C setter (only once the game runs, like the C setter)
export function onApply<K extends SettingKey>(key: K, fn: (v: SettingsValues[K]) => void): void {
  ((listeners[key] ??= []) as Array<(v: SettingsValues[K]) => void>).push(fn);
}

export function set<K extends SettingKey>(key: K, value: SettingsValues[K]): SettingsValues[K] {
  const d = DEFS[key] as Def<SettingsValues[K]>;
  if (!d.valid(value)) return values.value[key];
  values.value = { ...values.value, [key]: value };
  stored[key] = value; // only this key: a URL override of another key never reaches storage
  try {
    localStorage.setItem(KEY, JSON.stringify(stored));
  } catch (err) {
    console.warn('settings: cannot store', err);
  }
  if (isStarted && currentModule()) {
    if (d.fn) callNumber(d.fn, value as number);
    for (const fn of (listeners[key] ?? []) as Array<(v: SettingsValues[K]) => void>) fn(value);
  }
  return values.value[key];
}

export function args(): string[] {
  const v = values.value;
  return [
    '--drawdist', String(v.drawdist), '--scale', String(v.scale), '--msaa', String(v.msaa), '--a2c', String(v.a2c),
    '--mips', String(v.mips), '--aniso', String(v.aniso),
    '--interp', String(v.interp), '--hrtf', String(v.audio3d), '--audio-rate', String(v.audiorate),
    '--kong-swap', String(v.kongswap), '--disco-chunky', String(v.discochunky),
    '--stick-camera', String(v.stickcam),
    '--camera-speed', String(v.camspeed), '--camera-invert', String(v.caminvert),
    '--per-pixel', String(v.perpixel), '--kong-smooth', String(v.kongsmooth),
    '--downsample', String(v.downsample), '--sharpen', String(v.sharpen),
    '--fxaa', String(v.fxaa), '--dither', String(v.dither),
  ];
}

export function started(): void {
  isStarted = true;
}

export function maxSamples(): number {
  return isStarted ? callNumber('port_gfx_max_samples') : 0;
}

export function maxAniso(): number {
  return isStarted ? callNumber('port_gfx_max_aniso') : 0;
}

// the rate the running game uses: a quickload restores the rate its snapshot was made at
export function audioRate(): number {
  return isStarted ? callNumber('port_audio_rate') : 0;
}
