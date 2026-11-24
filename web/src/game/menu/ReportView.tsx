// The report view (bug, idea, crash): a prefilled GitHub issue (report.ts). A bug report shows a screenshot of the
// frozen frame, where a click marks the broken spot and probes the renderer there.
import type { RefObject } from 'preact';
import { useLayoutEffect, useRef, useState } from 'preact/hooks';
import * as debug from '../debug/api';
import * as report from '../report';
import type { Report } from '../report';
import { GameButton } from '../ui/GameButton';
import { crashMsg, lastProgress, runFrames } from './control';
import { KIND_TEXT, KONGS, LEVELS, LIST } from './data';
import type { ReportKind } from './data';

// what the view shows; everything an async completion checks lives in FlowState instead
interface ReportUi {
  shown: boolean; // report view instead of the main view
  kind: ReportKind;
  crash: string; // .rcrash text
  marked: boolean; // .rlist names the marked spot
  go: boolean; // Open GitHub ready (else "Preparing…")
  pin: { fx: number; fy: number } | null;
  img: string | null; // .rimg src
  imgShown: boolean;
  hint: string | null; // .rhint text, null: hidden
  status: string;
  saveShown: boolean;
  done: string | null; // the opened issue URL: shows .rdone
}

interface FlowState {
  kind: ReportKind | null;
  report: Report | null; // resolved report.collect() report, or null
  reportPromise: Promise<Report | null> | null;
  shotBlob: Blob | null; // unmarked screenshot PNG
  curShot: Blob | null; // the PNG Save screenshot downloads
  gen: number; // bumped per view change: late results of an earlier view are dropped
  pending: number; // report preparations in flight
  markSeq: number;
  imgUrl: string | null;
  focus: boolean; // focus the textarea once the view is rendered
}

const coarse = () => matchMedia('(pointer: coarse)').matches;

export interface ReportFlow {
  ui: ReportUi;
  rtext: RefObject<HTMLTextAreaElement | null>;
  rimg: RefObject<HTMLImageElement | null>;
  showReport(kind: ReportKind): void;
  showMain(): void;
  saveShot(): void;
  markClick(e: MouseEvent): void;
  openGithub(): void;
}

export function useReportFlow(): ReportFlow {
  const [ui, setUi] = useState<ReportUi>({
    shown: false,
    kind: 'bug',
    crash: '',
    marked: false,
    go: true,
    pin: null,
    img: null,
    imgShown: true,
    hint: '',
    status: '',
    saveShown: true,
    done: null,
  });
  const s = useRef<FlowState>({
    kind: null,
    report: null,
    reportPromise: null,
    shotBlob: null,
    curShot: null,
    gen: 0,
    pending: 0,
    markSeq: 0,
    imgUrl: null,
    focus: false,
  }).current;
  const rtext = useRef<HTMLTextAreaElement>(null);
  const rimg = useRef<HTMLImageElement>(null);
  const patch = (p: Partial<ReportUi>) => setUi((u) => ({ ...u, ...p }));

  useLayoutEffect(() => {
    if (!s.focus) return;
    s.focus = false;
    rtext.current?.focus();
  });

  function setGo(ready: boolean) {
    patch({ go: ready });
  }
  function begin() {
    s.pending++;
    setGo(false);
  }
  function end(g: number) {
    if (g !== s.gen) return;
    s.pending = Math.max(0, s.pending - 1);
    if (!s.pending) setGo(true);
  }

  function setImg(blob: Blob) {
    if (s.imgUrl) URL.revokeObjectURL(s.imgUrl);
    s.imgUrl = URL.createObjectURL(blob);
    patch({ img: s.imgUrl });
  }

  // must run inside the click: clipboard writes need the user gesture
  function copyShot(pngPromise: Promise<Blob>) {
    const g = s.gen;
    const fallback = () =>
      pngPromise.then(
        () => {
          if (g !== s.gen) return;
          patch({ status: 'Screenshot ready: save it and drag it into the issue', saveShown: true });
        },
        () => {},
      );
    if (!navigator.clipboard || !window.ClipboardItem) {
      fallback();
      return;
    }
    let write: Promise<void>;
    try {
      write = navigator.clipboard.write([new ClipboardItem({ 'image/png': pngPromise })]);
    } catch {
      fallback();
      return;
    }
    write.then(() => {
      if (g !== s.gen) return;
      patch({ status: 'Screenshot copied: paste it into the issue (Ctrl+V / ⌘V)' });
    }, fallback);
  }

  function saveShot() {
    if (!s.curShot) return;
    const d = new Date();
    const p = (n: number) => String(n).padStart(2, '0');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(s.curShot);
    a.download = `dk64-screenshot-${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}` +
      `${p(d.getMinutes())}${p(d.getSeconds())}.png`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  }

  function showReport(kind: ReportKind) {
    const g = ++s.gen;
    s.pending = 0;
    s.kind = kind;
    patch({
      shown: true,
      kind,
      crash: kind === 'crash' ? String(crashMsg.value).slice(0, 300) : '',
      marked: false,
      done: null,
      pin: null,
    });
    if (!coarse()) s.focus = true;
    s.report = null;
    s.reportPromise = null;
    s.shotBlob = s.curShot = null;
    if (kind === 'idea') {
      setGo(true);
      return;
    }
    begin();
    const progress = lastProgress.value;
    const p = progress && progress.ok ? progress : null;
    const kong = p ? KONGS[p.kong] : undefined;
    const reportPromise = report
      .collect({
        crash: kind === 'crash' ? crashMsg.value : null,
        level: p ? LEVELS[p.world] || null : null,
        kong: kong ? kong.name : null,
        progress,
      })
      .catch(() => null);
    s.reportPromise = reportPromise;
    reportPromise.then((r) => {
      if (g !== s.gen) return;
      s.report = r;
      end(g);
    });
    if (kind !== 'bug') return;
    patch({
      imgShown: true,
      img: null,
      hint: `Something look wrong? ${coarse() ? 'Tap' : 'Click'} the spot on the screenshot.`,
      status: '',
      saveShown: false,
    });
    const png = runFrames(() => debug.capture()).then(report.toPng);
    copyShot(png);
    png.then(
      (b) => {
        if (g !== s.gen) return;
        s.shotBlob = s.curShot = b;
        setImg(b);
      },
      () => {
        if (g !== s.gen) return;
        patch({ status: "No screenshot (the game didn't draw a frame)", imgShown: false, hint: null, saveShown: false });
      },
    );
  }

  function markClick(e: MouseEvent) {
    const img = rimg.current;
    const shotBlob = s.shotBlob;
    if (s.kind !== 'bug' || !shotBlob || !img || !img.clientWidth) return;
    const g = s.gen;
    const seq = ++s.markSeq;
    const fx = Math.min(1, Math.max(0, e.offsetX / img.clientWidth));
    const fy = Math.min(1, Math.max(0, e.offsetY / img.clientHeight));
    patch({ pin: { fx, fy }, marked: true });
    // always marked from the clean screenshot, so a second click moves the mark
    const marked = report.markPng(shotBlob, fx, fy);
    copyShot(marked);
    marked.then(
      (b) => {
        if (g !== s.gen || seq !== s.markSeq) return;
        s.curShot = b;
        setImg(b);
      },
      () => {},
    );
    begin();
    const probe = runFrames(() => debug.probe(fx, fy)).catch(() => null);
    Promise.all([s.reportPromise, probe])
      .then(([r, pr]) => r && report.withMark(r, { fx, fy, probe: pr }))
      .then(
        (r) => {
          if (r && g === s.gen && seq === s.markSeq) s.report = r;
        },
        () => {},
      )
      .finally(() => end(g));
  }

  function openGithub() {
    const ta = rtext.current;
    if (!s.kind || !ta) return;
    const url = report.issueUrl(s.kind, ta.value.trim(), s.report);
    const w = window.open(url, '_blank');
    if (w) w.opener = null;
    patch({ done: url });
    ta.value = '';
  }

  function showMain() {
    s.gen++;
    s.pending = 0;
    patch({ shown: false });
    if (rtext.current && document.activeElement === rtext.current) rtext.current.blur();
  }

  return { ui, rtext, rimg, showReport, showMain, saveShot, markClick, openGithub };
}

export function ReportView({ flow }: { flow: ReportFlow }) {
  const { ui } = flow;
  const t = KIND_TEXT[ui.kind];
  const items = ui.marked ? [...LIST, 'What the renderer drew at the spot you marked'] : LIST;
  return (
    <div style={{ display: ui.shown ? '' : 'none' }}>
      <p class="rintro">{t.intro}</p>
      <textarea ref={flow.rtext} class="rtext" maxLength={1000} rows={4} placeholder={t.placeholder} />
      <div class="rshot" style={{ display: ui.kind === 'bug' ? '' : 'none' }}>
        <div class="rshotwrap">
          <img
            ref={flow.rimg}
            class="rimg"
            alt="Screenshot of the game"
            draggable={false}
            src={ui.img ?? undefined}
            style={{ display: ui.imgShown ? '' : 'none' }}
            onClick={(e) => flow.markClick(e)}
          />
          <span
            class="rpin"
            style={
              ui.pin
                ? { left: ui.pin.fx * 100 + '%', top: ui.pin.fy * 100 + '%', display: 'block' }
                : { display: 'none' }
            }
          />
        </div>
        <p class="rhint" style={{ display: ui.hint === null ? 'none' : '' }}>
          {ui.hint}
        </p>
        <span class="rstatus">{ui.status}</span>
        <GameButton class="alt" style={{ display: ui.saveShown ? '' : 'none' }} onClick={() => flow.saveShot()}>
          Save screenshot
        </GameButton>
      </div>
      <pre class="rcrash" style={{ display: ui.kind === 'crash' ? '' : 'none' }}>
        {ui.crash}
      </pre>
      <ul class="rlist" style={{ display: ui.kind === 'idea' ? 'none' : '' }}>
        {items.map((text) => (
          <li key={text}>{text}</li>
        ))}
      </ul>
      <p class="rnote">
        Opens GitHub in a new tab. Sign in (free account), check it, and press Submit. Issues are public.
      </p>
      <div class="actions">
        <span>
          <GameButton class="alt" style={{ display: ui.kind === 'crash' ? 'none' : '' }} onClick={() => flow.showMain()}>
            Back
          </GameButton>
          <GameButton
            class="alt"
            style={{ display: ui.kind === 'crash' ? '' : 'none' }}
            onClick={() => location.reload()}
          >
            Reload game
          </GameButton>
        </span>
        <GameButton disabled={!ui.go} onClick={() => flow.openGithub()}>
          {ui.go ? 'Open GitHub' : 'Preparing…'}
        </GameButton>
      </div>
      <p class="rdone" style={{ display: ui.done === null ? 'none' : '' }}>
        GitHub opened in a new tab.
        <a target="_blank" rel="noopener" href={ui.done ?? undefined}>
          Open it again
        </a>
      </p>
    </div>
  );
}
