// #rom-drop (RomDrop.tsx): the no-ROM screen. Click it or drop a file anywhere; it also shows load progress and
// errors, and is removed once the game runs.
import { signal } from '@preact/signals';

// idle / error take a file; loading / failed don't (failed: the ROM passed but the start failed, which needs a
// reload: boot.ts never resets `started`)
export type DropState = 'idle' | 'loading' | 'error' | 'failed';

export interface DropView {
  state: DropState;
  title: string;
  detail: string;
}

// null: hidden (until the server ROM check settles); removed: the game runs
export const drop = signal<DropView | null>(null);
export const over = signal(false);
export const removed = signal(false);

export function setDrop(state: DropState, title: string, detail: string): void {
  drop.value = { state, title, detail };
  over.value = false;
}

export function setOver(on: boolean): void {
  over.value = on;
}

export function accepting(): boolean {
  const d = drop.value;
  return !removed.value && d !== null && d.state !== 'loading' && d.state !== 'failed';
}

export function removeDrop(): void {
  removed.value = true;
}
