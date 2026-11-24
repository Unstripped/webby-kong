import type { SaveSummary } from './types';

export function fmt(iso: string | null | undefined): string {
  if (!iso) return '\u2013';
  const d = new Date(iso);
  return isNaN(d.getTime()) ? '\u2013' : d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

// save game summaries (tools/saves.py SUMMARY_KEYS, uploaded by the game)
export function fmtPlay(sec: number): string {
  const m = Math.floor(sec / 60);
  return Math.floor(m / 60) + 'h ' + String(m % 60).padStart(2, '0') + 'm';
}

export function statsText(s: SaveSummary | null): string {
  return s
    ? s.percent + '% \u00b7 ' + s.goldenBananas + ' GB \u00b7 ' + s.keys + '/8 keys \u00b7 ' + fmtPlay(s.playSeconds)
    : 'Not started yet';
}
