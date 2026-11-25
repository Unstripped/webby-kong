// The PAUSED view: the level's progress cards, the settings and the action buttons.
import { useRef } from 'preact/hooks';
import { GameButton } from '../ui/GameButton';
import {
  actionsDisabled,
  close,
  exitNote,
  exitShown,
  exitText,
  exitToDashboard,
  exportSave,
  importSave,
  lastProgress,
  progressMsg,
  packsShown,
  saveNote,
} from './control';
import { COLUMNS, iconSrc, KONGS, LEVELS, WIDE } from './data';
import type { Progress, ReportKind } from './data';
import { SettingsGrid } from './SettingsGrid';

function Icon({ name }: { name: string }) {
  return <img src={iconSrc(name)} alt="" draggable={false} />;
}

function Cards({ data }: { data: Progress }) {
  const cols = COLUMNS.filter((c) => data.kongs.some((k) => k[c.key] !== null));
  return (
    <>
      {data.kongs.map((k, i) => {
        const kong = KONGS[i];
        if (!kong) return null;
        return (
          <div key={kong.id} class={i === data.kong ? 'card cur' : 'card'} style={{ '--k': kong.color }}>
            <div class="name">{kong.name}</div>
            {cols.map((c) => {
              const v = k[c.key];
              // empty rows keep the rows of every card aligned
              if (v === null) {
                return (
                  <div key={c.key} class="row none">
                    <Icon name={c.icon(kong)} />
                    <span class="val" />
                  </div>
                );
              }
              return (
                <div key={c.key} class={c.done(v) ? 'row done' : 'row'} title={c.label} aria-label={`${c.label}: ${c.text(v)}`}>
                  <Icon name={c.icon(kong)} />
                  <span class="val">{c.text(v)}</span>
                </div>
              );
            })}
          </div>
        );
      })}
    </>
  );
}

export function MainView({ shown, showReport }: { shown: boolean; showReport: (kind: ReportKind) => void }) {
  const fileIn = useRef<HTMLInputElement>(null);
  const data = lastProgress.value;
  const reason = progressMsg.value;
  // progress cards, or the message that replaces them
  const cards = reason === null && data !== null;
  const items = cards ? WIDE.filter((w) => data[w.total]) : [];
  const shownExit = exitShown.value;
  return (
    <div style={{ display: shown ? '' : 'none' }}>
      <div class="lbl">{cards ? LEVELS[data.world] || 'Level ' + data.world : reason !== null ? 'Progress' : ''}</div>
      <div class="cards" style={{ display: reason !== null ? 'none' : '' }}>
        {cards && <Cards data={data} />}
      </div>
      <div class="wide" style={{ display: reason !== null || (cards && !items.length) ? 'none' : '' }}>
        {cards &&
          items.map((w) => (
            <span key={w.key} class={data[w.key] === data[w.total] ? 'item done' : 'item'} title={w.label(data[w.total])}>
              <Icon name={w.icon} />
              <span class="val">{`${data[w.key]}/${data[w.total]}`}</span>
            </span>
          ))}
      </div>
      <p class="msg" style={{ display: cards ? 'none' : '' }}>
        {reason !== null && (
          <>
            {"Progress appears while you're playing a level. "}
            <span>{`(${reason})`}</span>
          </>
        )}
      </p>
      <div class="lbl sec">Settings</div>
      <SettingsGrid />
      <div class="foot">Changes apply immediately and are remembered on this device</div>
      <div class="actions main">
        <span class="hint">Esc · Back to resume</span>
        <div class="btns">
          <GameButton class="alt" onClick={() => showReport('idea')}>
            Suggest an idea
          </GameButton>
          <GameButton class="alt" onClick={() => showReport('bug')}>
            Report a bug
          </GameButton>
          <GameButton class="alt" onClick={() => exportSave()}>
            Export save
          </GameButton>
          <GameButton class="alt" disabled={actionsDisabled.value} onClick={() => fileIn.current?.click()}>
            {'Import save\u2026'}
          </GameButton>
          <GameButton class="alt" onClick={() => (packsShown.value = true)}>
            {'Texture packs\u2026'}
          </GameButton>
          <GameButton
            class="alt"
            disabled={actionsDisabled.value}
            style={{ display: shownExit ? '' : 'none' }}
            onClick={() => exitToDashboard()}
          >
            {exitText.value}
          </GameButton>
          {/* gate on: Resume alone spans the bottom row (gate off, it sits beside Texture packs) */}
          <GameButton style={{ gridColumn: shownExit ? '1 / -1' : '' }} onClick={() => close()}>
            Resume
          </GameButton>
        </div>
      </div>
      <p class="msg" style={{ display: saveNote.value === null ? 'none' : '' }}>
        {saveNote.value}
      </p>
      <p class="msg" style={{ display: exitNote.value === null ? 'none' : '' }}>
        {exitNote.value}
      </p>
      <input
        ref={fileIn}
        type="file"
        accept=".eep,.srm"
        style={{ display: 'none' }}
        onChange={(e) => {
          const input = e.currentTarget;
          const file = input.files?.[0];
          input.value = '';
          if (file) void importSave(file);
        }}
      />
    </div>
  );
}
