// Esc menu: freezes the game (port_set_paused) and shows the current level's collectables (port_level_progress).
// Opened by Esc, gamepad Back/Select (Standard mapping button 8) or the touch overlay's ☰ button.
// Report a bug / Suggest an idea switch the panel to a report view that opens a prefilled GitHub issue form
// (report.ts): a bug report shows a screenshot of the frozen frame, where a click marks the broken spot and
// probes the renderer there. A crash (wasm trap or abort, detected by report.ts) opens the same view as "UH OH!",
// which cannot be closed.
// Exit to dashboard (sign-in gate on, saves.ts dashboard()): records where the player stands
// (port_resume_capture), runs the pause menu's in-game save (port_save_request; the game runs for the ~1 s the
// EEPROM thread needs, port_save_busy), waits for the upload, stores the resume point on the server and goes to /.
// When something fails it says so and offers "Exit anyway".
// Export save downloads this save game's EEPROM file (shared/savefile.ts): the last in-game save, not progress since.
// Import save… reads an .eep / RetroArch .srm, replaces this save game's EEPROM in place (saves.replace), drops
// its quicksave snapshot (which would bring the old progress back), clears the server's resume point and reloads;
// saves.ts boot() then uploads the import as the newest version, so with the save server the old progress stays in
// its history.
// Texture packs… switches the panel to PacksView.tsx (packsShown): RT64 texture packs and the texture dump.
// This file is the menu's state machine; Menu.tsx renders it.
import { batch, effect, signal } from '@preact/signals';
import { callNumber, callString, callVoid, requireModule } from '../port';
import * as quicksave from '../quicksave';
import { crash } from '../report';
import * as saves from '../saves';
import type { ResumePoint } from '../saves';
import * as savefile from '../../shared/savefile';
import * as pad from '../touch/pad';
import type { Progress, ReportKind } from './data';

export const isStarted = signal(false);
export const isOpen = signal(false);
export const crashMode = signal(false);
export const crashMsg = signal<string | null>(null);
// the Texture packs view (PacksView.tsx) replaces the PAUSED view
export const packsShown = signal(false);

// port_level_progress at the last open(), and the reason the progress cards are replaced by a message (null: cards)
export const lastProgress = signal<Progress | null>(null);
export const progressMsg = signal<string | null>(null);

// main view: the exit button, the two notes under the actions, and the Import / Exit buttons' disabled state
export const exitShown = signal(false);
export const exitText = signal('Exit to dashboard');
export const actionsDisabled = signal(false);
export const exitNote = signal<string | null>(null); // null: hidden
export const saveNote = signal<string | null>(null); // null: hidden

let exiting = false; // Exit to dashboard or Import save in progress: the menu cannot close, and they exclude each other
let exitForce = false;

// the report view (Menu.tsx) registers how to switch views; close() and openCrash() switch synchronously
export interface ViewHooks {
  showMain(): void;
  showReport(kind: ReportKind): void;
}
let view: ViewHooks | null = null;

export function bindView(hooks: ViewHooks): () => void {
  view = hooks;
  return () => {
    if (view === hooks) view = null;
  };
}

// the game draws one or two frames (keys stay swallowed by the open menu): capture and probe need presented frames
export function runFrames<T>(promiseFn: () => Promise<T>): Promise<T> {
  callVoid('port_set_paused', 0);
  return promiseFn().finally(() => {
    if (isOpen.value && !crashMode.value) callVoid('port_set_paused', 1);
  });
}

function note(text: string): void {
  saveNote.value = text;
}

export function exportSave(): void {
  const m = requireModule('dk64menu');
  callVoid('port_save_flush_now');
  const bytes = saves.localBytes(m.FS);
  if (!bytes) {
    note('Nothing to export yet: the game hasn\u2019t saved this save game.');
    return;
  }
  const cur = saves.currentSave();
  const name = savefile.fileName(cur ? cur.name : '');
  savefile.download(bytes, name);
  note('Exported ' + name + ' (your last in-game save).');
}

export async function importSave(file: File): Promise<void> {
  if (exiting) return;
  let bytes: Uint8Array<ArrayBuffer>;
  try {
    bytes = savefile.parse(await file.arrayBuffer());
  } catch (err) {
    note((err as Error).message);
    return;
  }
  if (exiting) return; // Exit to dashboard started while the file was read
  const kept = saves.status().server === 'ok' && saves.currentSave() !== null;
  if (
    !confirm(
      'Import ' + file.name + '? It replaces this save game\u2019s progress and reloads the game. ' +
        (kept
          ? 'The current progress stays in the save server\u2019s history.'
          : 'The current progress in this browser is lost: export it first to keep it.'),
    )
  ) {
    return;
  }
  // until the reload: closing the menu would unpause the game, and its next EEPROM flush (or Exit to dashboard's
  // in-game save) would write the old progress over the import
  exiting = true;
  actionsDisabled.value = true;
  note('Importing\u2026');
  try {
    await quicksave.forget(); // first: if it fails, nothing has changed yet
    await saves.replace(requireModule('dk64menu'), bytes);
    await saves.putResume(null).catch(() => {});
  } catch (err) {
    note('Couldn\u2019t import the save (' + (err as Error).message + ').');
    actionsDisabled.value = false;
    exiting = false;
    return;
  }
  quicksave.bootInto(null, {});
}

// report.ts: the game trapped or aborted. Not closable; needs no started game or pause.
export function openCrash(msg: string): void {
  crashMode.value = true;
  crashMsg.value = msg;
  lastProgress.value = null;
  pad.releaseAll();
  isOpen.value = true;
  packsShown.value = false;
  view?.showReport('crash');
}

export function open(): void {
  if (!isStarted.value || isOpen.value || crashMode.value) return;
  callVoid('port_set_paused', 1);
  pad.releaseAll();
  batch(() => {
    lastProgress.value = null;
    try {
      // one snapshot per open: the game is frozen until close()
      const data = JSON.parse(callString('port_level_progress')) as Progress;
      lastProgress.value = data;
      progressMsg.value = data.ok ? null : `${data.reason}`;
    } catch (err) {
      progressMsg.value = (err as Error).message;
    }
    exitShown.value = saves.dashboard();
    exitForce = false;
    exitText.value = 'Exit to dashboard';
    actionsDisabled.value = false;
    saveNote.value = null;
    exitNote.value = null;
    isOpen.value = true;
  });
}

export function close(): void {
  if (!isOpen.value || crashMode.value || exiting) return;
  packsShown.value = false;
  view?.showMain();
  callVoid('port_set_paused', 0);
  isOpen.value = false;
  document.getElementById('canvas')?.focus();
}

// the game saves itself (pause menu Save) while unpaused behind the open menu; resolves to a problem or null
export async function inGameSave(): Promise<string | null> {
  callVoid('port_save_request', 1);
  callVoid('port_set_paused', 0);
  const deadline = performance.now() + 8000;
  let busy = true;
  while (busy && performance.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 50));
    busy = callNumber('port_save_busy') !== 0;
  }
  callVoid('port_set_paused', 1);
  if (!busy) return null;
  callVoid('port_save_request', 0);
  return "The game couldn't save right now. Try again in a moment, or exit with your last save.";
}

export async function exitToDashboard(): Promise<void> {
  if (exiting) return;
  exiting = true;
  actionsDisabled.value = true;
  exitNote.value = 'Saving…';
  let problem: string | null = null;
  let resume: ResumePoint & { ok: boolean } = { ok: false } as ResumePoint & { ok: boolean };
  try {
    // where the player stands, while frozen
    resume = JSON.parse(callString('port_resume_capture')) as ResumePoint & { ok: boolean };
    if (resume.ok && !exitForce) problem = await inGameSave();
  } catch (err) {
    problem = "The game couldn't save (" + (err as Error).message + ').';
  }
  if (!problem) {
    try {
      callVoid('port_save_flush_now');
      const ok = await saves.settle();
      await saves.putResume(resume.ok ? resume : null);
      if (!ok) {
        problem =
          "The save server didn't take the upload: your progress stays in this browser and uploads the next time you play this save.";
      }
    } catch (err) {
      problem = "Couldn't reach the save server (" + (err as Error).message + ').';
    }
  }
  if (problem && !exitForce) {
    exitNote.value = problem;
    exitText.value = 'Exit anyway';
    actionsDisabled.value = false;
    exitForce = true;
    exiting = false;
    return;
  }
  location.assign('/');
}

export function toggle(): void {
  if (isOpen.value) close();
  else open();
}

// gamepad Back/Select (Standard Gamepad button 8, unmapped by port/src/input.c): toggle on press
let prevBack = false;
function pollPad(): void {
  let gp: Gamepad | null = null;
  try {
    if (typeof navigator.getGamepads === 'function') {
      gp = Array.from(navigator.getGamepads()).find((g) => g && g.connected) || null;
    }
  } catch {
    gp = null; // insecure origins: getGamepads missing or throwing
  }
  const back = !!(gp && gp.buttons[8] && gp.buttons[8].pressed);
  if (back && !prevBack) toggle();
  prevBack = back;
  requestAnimationFrame(pollPad);
}

export function started(): void {
  if (isStarted.value) return;
  isStarted.value = true;
  requestAnimationFrame(pollPad);
}

export function installMenuControl(): void {
  // capture phase: runs before SDL's keydown listener. Key-ups are left alone so SDL sees every release and no
  // key held at open stays stuck; swallowing key-downs keeps keys pressed in the menu from reaching the game.
  window.addEventListener(
    'keydown',
    (e) => {
      if (isOpen.value) e.stopPropagation(); // no preventDefault: browser shortcuts and typing in the report still work
      if (e.key !== 'Escape' || !isStarted.value || crashMode.value) return;
      e.preventDefault();
      e.stopPropagation();
      if (!e.repeat) toggle();
    },
    true,
  );
  // SDL's window keypress listener calls preventDefault, which would keep characters out of the report textarea
  window.addEventListener(
    'keypress',
    (e) => {
      if (isOpen.value) e.stopPropagation();
    },
    true,
  );
  // report.ts crashed(): open the crash view once the throwing code has unwound
  effect(() => {
    const c = crash.value;
    if (c) setTimeout(() => openCrash(c.message), 0);
  });
}
