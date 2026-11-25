// The Esc menu's audio packs: turn the server's and this device's packs on and off, order them, import a .zip.
// Choices are stored by audiopack/store.ts and apply on the next load (boot.ts).
import { useEffect, useRef, useState } from 'preact/hooks';
import { PackError } from '../audiopack/manifest';
import { enabled, importLocal, listLocal, listServer, move, removeLocal, setEnabled } from '../audiopack/store';
import { GameButton } from '../ui/GameButton';
import { toast } from '../ui/toast';
import { isOpen } from './control';

interface Row {
  key: string;
  name: string;
  author: string;
  songs: number;
  sfx: number;
  local: boolean;
}

async function loadRows(): Promise<Row[]> {
  const [server, local] = await Promise.all([listServer(), listLocal()]);
  const all = new Map<string, Row>();
  for (const p of server) {
    all.set('s:' + p.id, { key: 's:' + p.id, name: p.name, author: p.author, songs: p.songs, sfx: p.sfx, local: false });
  }
  for (const p of local) {
    all.set('l:' + p.sha1, { key: 'l:' + p.sha1, name: p.name, author: p.author, songs: p.songs, sfx: p.sfx, local: true });
  }
  // enabled first in priority order (a key whose pack is gone stays stored but is not shown), then the rest by name
  const on = enabled().flatMap((k) => all.get(k) ?? []);
  const off = [...all.values()].filter((r) => !on.includes(r)).sort((a, b) => a.name.localeCompare(b.name));
  return [...on, ...off];
}

export function AudioPacksSection() {
  const [rows, setRows] = useState<Row[]>([]);
  const [on, setOn] = useState<string[]>([]);
  const fileIn = useRef<HTMLInputElement>(null);
  const open = isOpen.value;

  async function refresh(): Promise<void> {
    const next = await loadRows();
    setRows(next);
    setOn(enabled());
  }

  // on mount and whenever the menu opens: another tab or the dashboard may have changed the lists
  useEffect(() => {
    void refresh();
  }, [open]);

  async function importFile(file: File): Promise<void> {
    try {
      const meta = await importLocal(file);
      toast(`Imported "${meta.name}"`);
    } catch (err) {
      toast(err instanceof PackError ? `Not an audio pack: ${err.message}` : `Import failed: ${String(err)}`);
    }
    await refresh();
  }

  const enabledKeys = on.filter((k) => rows.some((r) => r.key === k));
  return (
    <div class="packs">
      {rows.length === 0 && <div class="foot">No audio packs yet. Import a .zip, or upload one in the dashboard.</div>}
      {rows.map((r) => {
        const pos = enabledKeys.indexOf(r.key);
        const active = pos >= 0;
        return (
          <div class="pack" key={r.key}>
            <GameButton
              class={active ? 'alt on' : 'alt'}
              onClick={() => {
                setEnabled(r.key, !active);
                void refresh();
              }}
            >
              {active ? 'On' : 'Off'}
            </GameButton>
            <div class="info">
              <div class="pname">{r.name}</div>
              <div class="meta">
                {[r.author, `${r.songs} songs`, `${r.sfx} sounds`, r.local ? 'this device' : 'server']
                  .filter(Boolean)
                  .join(' · ')}
              </div>
            </div>
            {active && (
              <>
                <GameButton
                  class="alt"
                  disabled={pos === 0}
                  onClick={() => {
                    move(r.key, -1);
                    void refresh();
                  }}
                >
                  ▲
                </GameButton>
                <GameButton
                  class="alt"
                  disabled={pos === enabledKeys.length - 1}
                  onClick={() => {
                    move(r.key, 1);
                    void refresh();
                  }}
                >
                  ▼
                </GameButton>
              </>
            )}
            {r.local && (
              <GameButton
                class="alt"
                onClick={() => {
                  void removeLocal(r.key.slice(2)).then(() => {
                    toast(`Removed "${r.name}"`);
                    return refresh();
                  });
                }}
              >
                Remove
              </GameButton>
            )}
          </div>
        );
      })}
      <div class="btns">
        <GameButton class="alt" style={{ gridColumn: '1 / -1' }} onClick={() => fileIn.current?.click()}>
          {'Import pack\u2026'}
        </GameButton>
      </div>
      {rows.length > 0 && (
        <div class="foot">Higher packs win when two replace the same sound. Changes apply on the next load.</div>
      )}
      <input
        ref={fileIn}
        type="file"
        accept=".zip"
        style={{ display: 'none' }}
        onChange={(e) => {
          const input = e.currentTarget;
          const file = input.files?.[0];
          input.value = '';
          if (file) void importFile(file);
        }}
      />
    </div>
  );
}
