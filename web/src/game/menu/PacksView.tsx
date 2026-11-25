// The Texture packs view: imported RT64 packs (texpacks/packs.ts) in priority order, each switchable and movable,
// pack and folder import, and the texture dump (texpacks/dump.ts).
import { useRef, useState } from 'preact/hooks';
import { dumpCount, dumping, startDump, stopDump } from '../texpacks/dump';
import { importArchive, importFolder, move, packList, remove, setEnabled, type PackInfo } from '../texpacks/packs';
import { GameButton } from '../ui/GameButton';
import { packsShown } from './control';

function info(p: PackInfo): string {
  if (!p.enabled) return 'Off';
  return `${p.resolved} textures · ${p.loaded} loaded` + (p.failed > 0 ? ` · ${p.failed} failed` : '');
}

export function PacksView({ shown }: { shown: boolean }) {
  const [note, setNote] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const packIn = useRef<HTMLInputElement>(null);
  const folderIn = useRef<HTMLInputElement>(null);
  const packs = packList.value;
  const run = async (task: Promise<string>) => {
    setBusy(true);
    try {
      setNote(await task);
    } finally {
      setBusy(false);
    }
  };
  return (
    <div style={{ display: shown ? '' : 'none' }}>
      <p class="rintro">
        Packs swap the game’s textures for new art at any resolution. Import RT64 packs (.rtz, .nrm or .zip with an
        rt64.json, as made for DK64 Rekongpiled) or a folder. Where two packs replace the same texture, the higher one
        wins.
      </p>
      {packs.length === 0 ? (
        <p class="msg">
          No texture packs<span>Import one below</span>
        </p>
      ) : (
        <div class="packs">
          {packs.map((p, i) => (
            <div class="pack" key={p.id}>
              <div class="pbody">
                <span class="pname">{p.name}</span>
                <span class="pinfo">{info(p)}</span>
                {p.error !== null && <span class="perr">{p.error}</span>}
              </div>
              <div class="pctl">
                <div class="seg">
                  <GameButton class={p.enabled ? 'sel' : undefined} onClick={() => void setEnabled(p.id, true)}>
                    On
                  </GameButton>
                  <GameButton class={p.enabled ? undefined : 'sel'} onClick={() => void setEnabled(p.id, false)}>
                    Off
                  </GameButton>
                </div>
                <GameButton disabled={i === 0} title="Higher priority" onClick={() => void move(p.id, -1)}>
                  {'\u25B2'}
                </GameButton>
                <GameButton disabled={i === packs.length - 1} title="Lower priority" onClick={() => void move(p.id, 1)}>
                  {'\u25BC'}
                </GameButton>
                <GameButton
                  title="Remove"
                  onClick={() => {
                    if (confirm(`Remove “${p.name}”?`)) void remove(p.id);
                  }}
                >
                  {'\u2715'}
                </GameButton>
              </div>
            </div>
          ))}
        </div>
      )}
      <div class="btns">
        <GameButton class="alt" disabled={busy} onClick={() => packIn.current?.click()}>
          {'Import pack\u2026'}
        </GameButton>
        <GameButton class="alt" disabled={busy} onClick={() => folderIn.current?.click()}>
          {'Import folder\u2026'}
        </GameButton>
        <GameButton
          class="alt"
          onClick={() => {
            if (dumping.value) void run(stopDump());
            else {
              startDump();
              setNote(null);
            }
          }}
        >
          {dumping.value ? `Stop dump (${dumpCount.value})` : 'Start texture dump'}
        </GameButton>
        <GameButton onClick={() => (packsShown.value = false)}>Back</GameButton>
      </div>
      <p class="msg" style={{ display: note === null ? 'none' : '' }}>
        {note}
      </p>
      <div class="foot">
        The dump saves each texture drawn while it runs as a PNG, with an rt64.json. Edit the PNGs at any size, then
        import the zip as a pack.
      </div>
      <input
        ref={packIn}
        type="file"
        accept=".rtz,.nrm,.zip"
        style={{ display: 'none' }}
        onChange={(e) => {
          const input = e.currentTarget;
          const file = input.files?.[0];
          input.value = '';
          if (file) void run(importArchive(file));
        }}
      />
      <input
        ref={(el) => {
          folderIn.current = el;
          if (el) el.webkitdirectory = true;
        }}
        type="file"
        style={{ display: 'none' }}
        onChange={(e) => {
          const input = e.currentTarget;
          const files = input.files;
          if (files && files.length > 0) void run(importFolder(files)).finally(() => (input.value = ''));
        }}
      />
    </div>
  );
}
