// The on-screen controller's pad state -> port_touch_pad (TouchOverlay.tsx renders it).
import { computed, signal } from '@preact/signals';
import { currentModule } from '../port';
import * as settings from '../settings';

const STICK_DEADZONE = 0.1; // radial, of full deflection

// pointers holding each button (keyed by its N64 bits); `held` is their OR, also rendered as `.down`
const ptrs = new Map<number, Set<number>>();
export const held = signal(0);
let sx = 0;
let sy = 0;
let sent = '';

export const touchStarted = signal(false);
export const touchMode = computed(() => settings.values.value.touch);
export const kongSwap = computed(() => settings.values.value.kongswap);

export function send(): void {
  const key = held.value + ',' + sx + ',' + sy;
  if (key === sent) return;
  const m = currentModule();
  if (!m) return; // game not started; resend once it is
  m.ccall('port_touch_pad', null, ['number', 'number', 'number'], [held.value, sx, sy]);
  sent = key;
}

function recompute(): void {
  let bits = 0;
  for (const [b, set] of ptrs) if (set.size > 0) bits |= b;
  held.value = bits;
  send();
}

export function press(bits: number, pointerId: number): void {
  let set = ptrs.get(bits);
  if (!set) ptrs.set(bits, (set = new Set()));
  set.add(pointerId);
  recompute();
}

export function release(bits: number, pointerId: number): void {
  ptrs.get(bits)?.delete(pointerId);
  recompute();
}

export function setStick(x: number, y: number): void {
  sx = x;
  sy = y;
  send();
}

// nipplejs vector (unit radius, y up) -> N64 stick units; circle of radius 80 past a radial deadzone
export function stickVec(vx: number, vy: number): [number, number] {
  const m = Math.min(Math.hypot(vx, vy), 1);
  if (m < STICK_DEADZONE) return [0, 0];
  const s = (m - STICK_DEADZONE) / (1 - STICK_DEADZONE) / m;
  return [Math.round(vx * s * 80), Math.round(vy * s * 80)];
}

export function releaseAll(): void {
  for (const set of ptrs.values()) set.clear();
  held.value = 0;
  sx = 0;
  sy = 0;
  send();
}

export function started(): void {
  touchStarted.value = true;
}

// main.tsx: Kong swap Off drops a held ⇄ (the overlay hides it from the signal)
export function installTouch(): void {
  settings.onApply('kongswap', (v) => {
    if (!v) releaseAll();
  });
}
