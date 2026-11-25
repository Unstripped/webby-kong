// F2 debug panel: live state, trace toggles, quicksave and marks (the UI half of the old debug.js)
import { useEffect, useRef, useState } from 'preact/hooks';
import * as qs from '../quicksave';
import { GameButton } from '../ui/GameButton';
import {
  audio,
  clearMarks,
  exportMarks,
  loadReport,
  markOrToast,
  marks,
  overlayVisible,
  quickload,
  quicksave,
  refreshNonce,
  state,
  toggleAutoResume,
  toggleTraceBit,
  trace,
  traceMask,
  type DebugStats,
  type TraceEvent,
} from './api';
import './debug.css';

// bits match PORT_TRACE_* in port/include/port/port.h; 8 is for temporary trace points
const CATEGORIES = [
  { bit: 1, label: 'Map' },
  { bit: 2, label: 'Camera' },
  { bit: 4, label: 'Actors' },
  { bit: 8, label: 'Debug' },
];

function hex(n: number, width: number): string {
  return n.toString(16).padStart(width, '0');
}

interface View {
  marksCount: number;
  qsText: string;
  qsAuto: boolean;
  mapText: string;
  audioText: string;
  stats: DebugStats | null;
  mask: number;
  events: TraceEvent[];
  fps: { text: string; cls: string };
}

const INITIAL: View = {
  marksCount: 0,
  qsText: '—',
  qsAuto: false,
  mapText: '-',
  audioText: '-',
  stats: null,
  mask: 0,
  events: [],
  fps: { text: '-- fps', cls: 'ok' },
};

function qsText(): { text: string; auto: boolean } {
  const st = qs.status();
  const parts: string[] = [];
  if (st.snapshot) {
    const t = new Date(st.snapshot.time).toTimeString().slice(0, 8);
    parts.push(`snap ${t} 0x${hex(st.snapshot.map, 2)}${st.snapshot.sameBuild ? '' : ' (other build)'}`);
  }
  if (st.resume) {
    parts.push(`resume 0x${hex(st.resume.map, 2)}`);
  }
  return { text: parts.length ? parts.join(' · ') : '—', auto: st.auto };
}

// sequence per player (– stopped) and the sound ids played last, newest first: the ids audio packs replace
function audioText(a: { seq: number[]; sfx: number[] }): string {
  const seq = a.seq.map((id) => (id < 0 ? '–' : `0x${hex(id, 2)}`)).join(' ');
  const sfx = a.sfx.length ? a.sfx.map((id) => `0x${hex(id, 3)}`).join(' ') : '–';
  return `seq ${seq} · sfx ${sfx}`;
}

export function DebugPanel() {
  const visible = overlayVisible.value;
  const nonce = refreshNonce.value;
  const [view, setView] = useState<View>(INITIAL);
  const last = useRef<{ frames: number; t: number } | null>(null); // previous refresh, for fps

  function refresh(): void {
    setView((prev) => {
      const next: View = { ...prev, marksCount: marks().length };
      const q = qsText();
      next.qsText = q.text;
      next.qsAuto = q.auto;
      let s: DebugStats, events: TraceEvent[], mask: number, au: { seq: number[]; sfx: number[] };
      try {
        s = state();
        events = trace(0).events;
        mask = traceMask();
        au = audio();
      } catch (e) {
        next.mapText = (e as Error).message;
        return next;
      }
      const now = performance.now();
      const l = last.current;
      if (l && now - l.t >= 1000) {
        const fps = ((s.frames - l.frames) * 1000) / (now - l.t);
        next.fps = { text: `${fps.toFixed(1)} fps`, cls: fps >= 27 ? 'ok' : fps >= 20 ? 'warn' : 'bad' };
        last.current = { frames: s.frames, t: now };
      } else if (!l) {
        last.current = { frames: s.frames, t: now };
      }
      next.mapText = `0x${hex(s.map, 2)} (${s.map})`;
      next.audioText = audioText(au);
      next.stats = s;
      next.mask = mask;
      next.events = events;
      return next;
    });
  }

  useEffect(() => {
    if (!visible) return;
    last.current = null;
    refresh();
    const timer = setInterval(refresh, 250);
    return () => clearInterval(timer);
  }, [visible]);

  const firstNonce = useRef(true);
  useEffect(() => {
    if (firstNonce.current) {
      firstNonce.current = false;
      return;
    }
    if (overlayVisible.peek()) refresh();
  }, [nonce]);

  const s = view.stats;
  const p = s?.player ?? null;
  const recent = view.events.slice(-5).reverse();
  return (
    <div id="dk64dbg-overlay" style={{ display: visible ? 'block' : 'none' }}>
      <div class="hd">
        <span class="title">DK64 debug</span>
        <span class="hint">F2 hide · F8 mark · Alt+click probe · F5/F9 quicksave</span>
        <span class={'fps ' + view.fps.cls}>{view.fps.text}</span>
      </div>
      <div class="grid">
        <div class="k">map</div>
        <div class="v">{view.mapText}</div>
        <div class="k">mode</div>
        <div class="v">{s ? `${s.mode} · overlay ${s.overlay}` : '-'}</div>
        <div class="k">time</div>
        <div class="v">{s ? `vi ${s.vi} · frame ${s.frames}` : '-'}</div>
        <div class="k">player</div>
        <div class="v">{s ? (p ? `${p.x.toFixed(1)}, ${p.y.toFixed(1)}, ${p.z.toFixed(1)}` : '—') : '-'}</div>
        <div class="k">facing</div>
        <div class="v">{s ? (p ? `yaw ${p.yaw} · chunk ${p.chunk}` : '—') : '-'}</div>
        <div class="k">actors</div>
        <div class="v">{s ? String(s.actors) : '-'}</div>
        <div class="k">audio</div>
        <div class="v">{s ? view.audioText : '-'}</div>
        <div class="k" style={{ display: s?.lastError ? '' : 'none' }}>error</div>
        <div class="v err" style={{ display: s?.lastError ? '' : 'none' }}>{s?.lastError ?? ''}</div>
      </div>
      <div class="sec">
        <div class="lbl">
          <span>Trace</span>
          <span>{s ? `0x${hex(view.mask, 1)}` : ''}</span>
        </div>
        <div class="chips">
          {CATEGORIES.map((c) => (
            <GameButton key={c.bit} class={(view.mask & c.bit) !== 0 ? 'chip on' : 'chip'} onClick={() => toggleTraceBit(c.bit)}>
              {c.label}
            </GameButton>
          ))}
        </div>
        <div class="log">
          {s && !recent.length && <div class="empty">no events yet</div>}
          {recent.map((ev, i) => (
            <div key={i} title={ev.text}>
              <span class="vi">{String(ev.vi)}</span>
              {ev.text}
            </div>
          ))}
        </div>
      </div>
      <div class="sec">
        <div class="lbl">
          <span>Quicksave</span>
          <span>{view.qsText}</span>
        </div>
        <div class="actions">
          <GameButton class="primary" onClick={() => quicksave()}>⤒ Save (F5)</GameButton>
          <GameButton onClick={() => quickload()}>⤓ Load (F9)</GameButton>
          <GameButton class={view.qsAuto ? 'chip on' : 'chip'} onClick={() => toggleAutoResume()}>Auto-resume</GameButton>
          <GameButton onClick={() => loadReport()}>Load report…</GameButton>
        </div>
      </div>
      <div class="sec">
        <div class="lbl">
          <span>Marks</span>
          <span>{String(view.marksCount)}</span>
        </div>
        <div class="actions">
          <GameButton class="primary" onClick={() => markOrToast()}>● Mark</GameButton>
          <GameButton onClick={() => exportMarks()}>⤓ Export</GameButton>
          <GameButton onClick={() => clearMarks()}>Clear</GameButton>
        </div>
      </div>
    </div>
  );
}
