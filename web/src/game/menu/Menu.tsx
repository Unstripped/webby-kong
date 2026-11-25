// The Esc menu (#dk64menu): a wood panel with the PAUSED view, the Texture packs view or the report view. State lives
// in control.ts.
import { useLayoutEffect, useRef } from 'preact/hooks';
import { bindView, close, isOpen, packsShown } from './control';
import { iconSrc, KIND_TEXT, KONGS } from './data';
import { MainView } from './MainView';
import './menu.css';
import { PacksView } from './PacksView';
import { ReportView, useReportFlow } from './ReportView';

// preload so the first open doesn't pop in
for (const n of ['gb', 'medal', 'blueprint', 'fairy', 'crown', 'key', ...KONGS.map((k) => 'banana-' + k.id)]) {
  new Image().src = iconSrc(n);
}
new Image().src = '/menu/wood.jpg';

export function Menu() {
  const flow = useReportFlow();
  // close only when the press also started on the backdrop: the click that follows the ☰ tap that opened the
  // menu, or a slider drag released outside the panel, lands on the backdrop too
  const downOnBackdrop = useRef(false);
  // flow's functions only touch refs and the state setter, so the first render's ones stay valid
  useLayoutEffect(() => bindView({ showMain: flow.showMain, showReport: flow.showReport }), []);
  return (
    <div
      id="dk64menu"
      class={isOpen.value ? 'on' : undefined}
      onPointerDown={(e) => {
        downOnBackdrop.current = e.target === e.currentTarget;
      }}
      onClick={(e) => {
        if (e.target === e.currentTarget && downOnBackdrop.current) close();
        downOnBackdrop.current = false;
      }}
    >
      <div class="panel">
        {['tl', 'tr', 'bl', 'br'].map((c) => (
          <span key={c} class={'bolt ' + c} />
        ))}
        <div class="title">
          {flow.ui.shown ? KIND_TEXT[flow.ui.kind].title : packsShown.value ? 'TEXTURE PACKS' : 'PAUSED'}
        </div>
        <MainView shown={!flow.ui.shown && !packsShown.value} showReport={flow.showReport} />
        <PacksView shown={!flow.ui.shown && packsShown.value} />
        <ReportView flow={flow} />
      </div>
    </div>
  );
}
