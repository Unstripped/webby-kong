import { useRef } from 'preact/hooks';
import { loadFile } from './boot';
import { drop, over, removed } from './romdrop';

export function RomDropZone() {
  const input = useRef<HTMLInputElement>(null);
  if (removed.value) return null;
  const d = drop.value;
  const disabled = d !== null && (d.state === 'loading' || d.state === 'failed');
  return (
    // z-index 5: above touch controls / debug panel (2-3), below the Esc menu and crash report (10)
    <div id="rom-drop" hidden={d === null} data-state={d?.state} class={over.value ? 'over' : undefined}>
      <button type="button" id="rom-zone" disabled={disabled} onClick={() => input.current!.click()}>
        {/* N64 Game Pak front traced from photos, 116x76: straight sides and bottom, one wide dome arc across the top
            with rounded shoulders, a notch cut into each bottom corner, two vertical seams marking the side strips,
            and a label recess whose top follows the dome. The arrow sits on the label; .spin (3/4 circle centered on
            the label) shows only while loading. */}
        <svg viewBox="0 -2 120 80" aria-hidden="true">
          <g transform="translate(2 0)">
            <path class="body" d="M4 76 V69 H0 V22 Q0 18.2 5 15.5 A111 111 0 0 1 111 15.5 Q116 18.2 116 22 V69 H112 V76 Z" />
            <path class="strip" d="M4 76 V69 H0 V22 Q0 18.2 5 15.5 A111 111 0 0 1 18 9.5 V76 Z M98 9.5 A111 111 0 0 1 111 15.5 Q116 18.2 116 22 V69 H112 V76 H98 Z" />
            <path class="seam" d="M18 9.5 V76 M98 9.5 V76" />
            <path class="label" d="M24 14.5 A104 104 0 0 1 92 14.5 V63 Q92 66 89 66 H27 Q24 66 24 63 Z" />
            <path class="arrow" d="M58 20 V50 M47 40 L58 51 L69 40" />
            <path class="spin" d="M58 30 A11 11 0 1 1 47 41" />
          </g>
        </svg>
        <span aria-live="polite">
          <span id="rom-title">{over.value ? 'RELEASE TO INSERT' : d?.title}</span>
          <span id="rom-detail">{over.value ? '.z64 · .n64 · .v64' : d?.detail}</span>
        </span>
      </button>
      <input
        type="file"
        id="rom"
        accept=".z64,.n64,.v64"
        hidden
        ref={input}
        onChange={(e) => {
          const el = e.currentTarget;
          const file = el.files?.[0];
          el.value = ''; // so picking the same file again after an error fires change again
          if (file) void loadFile(file);
        }}
      />
    </div>
  );
}
