// One box per setting: name and current value, the control, a note. Rendered from settings.values; re-read on
// every open (maxSamples / audioRate are live values of the running game).
import type { ComponentChildren } from 'preact';
import * as settings from '../settings';
import type { SettingKey, SettingsValues } from '../settings';
import { GameButton } from '../ui/GameButton';
import { isOpen } from './control';

interface RangeDef {
  kind: 'range';
  key: 'drawdist' | 'scale' | 'camspeed';
  name: string;
  min: number;
  max: number;
  ends: [string, string];
  fmt: (v: number) => string;
  note: string;
}

interface SegOption {
  value: SettingsValues[SettingKey];
  text: string;
  pick: () => void;
}

interface SegDef {
  kind: 'seg';
  key: SettingKey;
  name: string;
  options: SegOption[];
  note: () => string;
}

function range(
  key: RangeDef['key'],
  name: string,
  min: number,
  max: number,
  ends: [string, string],
  fmt: (v: number) => string,
  note: string,
): RangeDef {
  return { kind: 'range', key, name, min, max, ends, fmt, note };
}

function seg<K extends SettingKey>(
  key: K,
  name: string,
  options: Array<[SettingsValues[K], string]>,
  note: () => string,
): SegDef {
  return {
    kind: 'seg',
    key,
    name,
    options: options.map(([value, text]) => ({ value, text, pick: () => settings.set(key, value) })),
    note,
  };
}

const SETTINGS: Array<RangeDef | SegDef> = [
  range('drawdist', 'Draw distance', 1, 16, ['1× N64', '16×'], (v) => `${v}×`,
    'Objects & enemies spawn further out'),
  range('scale', 'Render scale', 0, 8, ['Auto', '8×'], (v) => (v === 0 ? 'Auto' : `${v}×`),
    'Auto follows the window height'),
  seg('msaa', 'Anti-aliasing', [[0, 'Off'], [2, '2×'], [4, '4×'], [8, '8×']], () => {
    const m = settings.maxSamples();
    return m >= 2 ? `MSAA; your GPU supports up to ${m}×` : 'MSAA is not supported by this GPU';
  }),
  seg('interp', 'Frame rate', [[0, '30 (N64)'], [1, '60'], [2, '120'], [3, '240']],
    () => 'Game logic stays at 30; higher rates add interpolated frames, up to the display\'s refresh rate'),
  seg('touch', 'Touch controls', [['auto', 'Auto'], ['on', 'On'], ['off', 'Off']],
    () => 'Auto: shown on touch screens without a gamepad'),
  seg('kongswap', 'Kong swap', [[0, 'Off'], [1, 'On']],
    () => 'D-pad ◀ ▶ (LB / RB with the right-stick camera on; keys F / H) switches to an unlocked kong where a swap barrel could be used'),
  seg('discochunky', 'Disco Chunky', [[0, 'Off'], [1, 'On']],
    () => 'Chunky wears his afro and disco suit; on screen he changes in a puff of smoke once he is on the ground'),
  seg('stickcam', 'Right-stick camera', [[0, 'Off'], [1, 'On']],
    () => 'Gamepad right stick turns the camera 360° and tilts it; D-pad = C buttons, LB / RB = D-pad ◀ ▶'),
  range('camspeed', 'Camera turn speed', 1, 10, ['Slow', 'Fast'], (v) => String(v),
    'Right stick at full tilt; 5 = C-button speed'),
  seg('caminvert', 'Invert camera', [[0, 'Off'], [1, 'Horizontal'], [2, 'Vertical'], [3, 'Both']],
    () => 'Off: stick right turns like C ▶, stick up looks up'),
  seg('audio3d', '3D sound', [[0, 'Off'], [1, 'Headphones']],
    () => 'Positional effects come from their direction, front and back included'),
  seg('audiorate', 'Audio quality', [[22050, '22 kHz (N64)'], [44100, '44 kHz']], () => {
    const running = settings.audioRate();
    return running && running !== settings.get('audiorate')
      ? `Applies when the game is reloaded (running at ${running / 1000} kHz)`
      : 'Sample rate of the sound mix';
  }),
  seg('perpixel', 'Per-pixel lighting', [[0, 'Off (N64)'], [1, 'On']],
    () => 'Lights and fog are worked out for every pixel instead of every vertex: smoother shading on large surfaces, no fog steps across big triangles'),
  seg('kongsmooth', 'Kong smoothing', [[0, 'Off (N64)'], [1, 'On']],
    () => 'Curved-surface (PN triangle) subdivision of the Kong models, for rounder silhouettes; best with per-pixel lighting on, which rounds their shading too'),
];

function SettingBox(props: { name: string; now: string; note: string; children: ComponentChildren }) {
  return (
    <div class="set">
      <div class="head">
        <span class="sname">{props.name}</span>
        <span class="now">{props.now}</span>
      </div>
      {props.children}
      <div class="note">{props.note}</div>
    </div>
  );
}

function RangeSetting({ def }: { def: RangeDef }) {
  const { key, min, max } = def;
  const v = settings.get(key);
  const ticks = [];
  for (let t = min; t <= max; t++) ticks.push(<i key={t} />);
  return (
    <SettingBox name={def.name} now={def.fmt(v)} note={def.note}>
      <div class="slider">
        <input
          type="range"
          min={String(min)}
          max={String(max)}
          step="1"
          tabIndex={-1}
          value={String(v)}
          style={{ '--p': ((v - min) / (max - min)) * 100 + '%' }}
          onInput={(e) => {
            const input = e.currentTarget;
            settings.set(key, Number(input.value));
            // re-read in case set() rejected the value
            const cur = settings.get(key);
            input.value = String(cur);
            input.style.setProperty('--p', ((cur - min) / (max - min)) * 100 + '%');
          }}
        />
        <div class="ticks">{ticks}</div>
      </div>
      <div class="ends">
        <span>{def.ends[0]}</span>
        <span>{def.ends[1]}</span>
      </div>
    </SettingBox>
  );
}

function SegSetting({ def }: { def: SegDef }) {
  const cur = settings.get(def.key);
  const sel = def.key === 'msaa' && cur === 1 ? 0 : cur;
  const max = def.key === 'msaa' ? settings.maxSamples() : 0;
  const now = def.options.find((o) => o.value === sel)?.text ?? '';
  return (
    <SettingBox name={def.name} now={now} note={def.note()}>
      <div class="seg">
        {def.options.map((o) => (
          <GameButton
            key={String(o.value)}
            class={o.value === sel ? 'sel' : undefined}
            disabled={def.key === 'msaa' && typeof o.value === 'number' && o.value > max}
            onClick={o.pick}
          >
            {o.text}
          </GameButton>
        ))}
      </div>
    </SettingBox>
  );
}

export function SettingsGrid() {
  // subscribe: every change re-renders, and so does every open (the live GPU / audio values)
  void settings.values.value;
  void isOpen.value;
  return (
    <div class="settings">
      {SETTINGS.map((def) =>
        def.kind === 'range' ? <RangeSetting key={def.key} def={def} /> : <SegSetting key={def.key} def={def} />,
      )}
    </div>
  );
}
