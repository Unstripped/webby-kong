import { useEffect, useRef, useState } from 'preact/hooks';
import * as savefile from '../../shared/savefile';
import { api, ApiError } from '../../shared/webauthn';
import { confirmDialog, promptDialog } from '../dialogs';
import { fail } from '../fail';
import { fmt, statsText } from '../format';
import { toast } from '../toasts';
import type { Save } from '../types';

function playUrl(save: Save): string {
  return '/play?save=' + save.id;
}

// the save's latest EEPROM version as an .eep file (shared/savefile.ts)
function exportSave(s: Save): void {
  fetch('/api/saves/' + s.id + '/eeprom', { credentials: 'same-origin', cache: 'no-store' })
    .then(async (r) => {
      if (r.ok) {
        const buf = await r.arrayBuffer();
        const name = savefile.fileName(s.name);
        savefile.download(new Uint8Array(buf), name);
        toast('Exported ' + name + '.');
        return;
      }
      const b: { error?: string; message?: string } | null = await r.json().catch(() => null);
      if (b && b.error === 'no_save_data') {
        toast('Nothing to export yet: this save has no progress.', true);
        return;
      }
      throw new ApiError((b && b.message) || 'Export failed (' + r.status + ')', 'http_' + r.status, r.status);
    })
    .catch(fail);
}

function Tile({ s, load }: { s: Save; load: () => void }) {
  return (
    <div class="tile">
      <h3>{s.name}</h3>
      <p class="stats">{statsText(s.summary)}</p>
      <p class="muted">{'Last played ' + fmt(s.played_at)}</p>
      <div class="acts">
        <button class="btn" type="button" onClick={() => location.assign(playUrl(s))}>
          Play
        </button>
        <button
          class="btn small"
          type="button"
          onClick={() => {
            promptDialog('Rename \u201c' + s.name + '\u201d', s.name, 'Rename').then((name) => {
              if (name)
                api('PATCH', '/api/saves/' + s.id, { name })
                  .then(() => {
                    toast('Save renamed.');
                    load();
                  })
                  .catch(fail);
            });
          }}
        >
          Rename
        </button>
        <button class="btn small" type="button" onClick={() => exportSave(s)}>
          Export
        </button>
        <button
          class="btn small danger"
          type="button"
          onClick={() => {
            confirmDialog(
              'Delete ' + s.name + '? Its whole save history is removed. This can\u2019t be undone.',
              'Delete',
            ).then((y) => {
              if (y)
                api('DELETE', '/api/saves/' + s.id)
                  .then(() => {
                    toast('Save deleted.');
                    load();
                  })
                  .catch(fail);
            });
          }}
        >
          Delete
        </button>
      </div>
    </div>
  );
}

export function SavesPanel() {
  // null until the first load: the play button and the tiles stay empty
  const [saves, setSaves] = useState<Save[] | null>(null);
  const [creating, setCreating] = useState(false);
  const [importing, setImporting] = useState(false);
  const [starting, setStarting] = useState(false);
  const nameRef = useRef<HTMLInputElement>(null);
  const fileRef = useRef<HTMLInputElement>(null);

  const load = () => {
    api<{ saves: Save[] }>('GET', '/api/saves')
      .then((r) => {
        setStarting(false);
        setSaves(r.saves);
      })
      .catch(fail);
  };
  useEffect(load, []);

  const onCreate = (e: Event) => {
    e.preventDefault();
    const nameIn = nameRef.current!;
    setCreating(true);
    api('POST', '/api/saves', { name: nameIn.value.trim() })
      .then(() => {
        nameIn.value = '';
        toast('Save created.');
        load();
      })
      .catch(fail)
      .then(() => setCreating(false));
  };

  // Import save…: a new save game whose first version is the file's EEPROM (shared/savefile.ts parses .eep / .srm)
  const onImport = () => {
    const fileIn = fileRef.current!, nameIn = nameRef.current!;
    const f = fileIn.files && fileIn.files[0];
    fileIn.value = '';
    if (!f) return;
    setImporting(true);
    f.arrayBuffer()
      .then((buf) => {
        const bytes = savefile.parse(buf);
        return api<{ save: Save }>('POST', '/api/saves', {
          name: nameIn.value.trim() || f.name.replace(/\.(eep|srm)$/i, '').trim().slice(0, 40),
          eeprom: btoa(String.fromCharCode(...bytes)),
        });
      })
      .then((r) => {
        nameIn.value = '';
        toast('Imported as \u201c' + r.save.name + '\u201d.');
        load();
      })
      .catch(fail)
      .then(() => setImporting(false));
  };

  const first = saves && saves[0];
  return (
    <>
      <div>
        {first ? (
          <button class="play" type="button" onClick={() => location.assign(playUrl(first))}>
            {'Continue ' + first.name}
          </button>
        ) : saves ? (
          <button
            class="play"
            type="button"
            disabled={starting}
            onClick={() => {
              setStarting(true);
              api<{ save: Save }>('POST', '/api/saves', {}).then(
                (c) => location.assign(playUrl(c.save)),
                (er) => {
                  setStarting(false);
                  fail(er);
                },
              );
            }}
          >
            Start a new game
          </button>
        ) : null}
      </div>
      <form class="row" style="margin-top:14px" onSubmit={onCreate}>
        <input
          ref={nameRef}
          type="text"
          class="grow"
          maxLength={40}
          placeholder="New save name (optional)"
          aria-label="New save name"
        />
        <button class="btn" type="submit" disabled={creating}>
          New save
        </button>
        <button class="btn ghost" type="button" disabled={importing} onClick={() => fileRef.current!.click()}>
          {'Import save\u2026'}
        </button>
        <input ref={fileRef} type="file" accept=".eep,.srm" style="display:none" onChange={onImport} />
      </form>
      <div class="panel-grid" style="margin-top:10px">
        {saves && saves.map((s) => <Tile key={s.id} s={s} load={load} />)}
      </div>
    </>
  );
}
