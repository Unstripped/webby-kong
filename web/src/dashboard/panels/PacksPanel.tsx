// Community audio packs (tools/packs.py): every user sees every pack; players turn them on in the game's Esc menu.
import { useEffect, useRef, useState } from 'preact/hooks';
import type { ServerPack } from '../../shared/packs';
import { api } from '../../shared/webauthn';
import { confirmDialog } from '../dialogs';
import { fail } from '../fail';
import { fmt } from '../format';
import { toast } from '../toasts';

const mib = (bytes: number) => (bytes / (1024 * 1024)).toFixed(1) + ' MiB';

function Tile({ p, load }: { p: ServerPack; load: () => void }) {
  return (
    <div class="tile">
      <h3>
        {p.name}
        {p.version && <span class="muted">{' ' + p.version}</span>}
      </h3>
      {p.author && <p class="stats">{'by ' + p.author}</p>}
      {p.description && <p class="muted">{p.description}</p>}
      <p class="muted">{`${p.songs} songs \u00b7 ${p.sfx} sounds \u00b7 ${mib(p.size)}`}</p>
      <p class="muted">{(p.uploader ? 'uploaded by ' + p.uploader + ' \u00b7 ' : '') + fmt(p.created_at)}</p>
      <div class="acts">
        <a class="btn small" href={'/api/packs/' + p.id + '/file'} download={'pack-' + p.id + '.zip'}>
          Download
        </a>
        {p.can_delete && (
          <button
            class="btn small danger"
            type="button"
            onClick={() => {
              confirmDialog('Delete \u201c' + p.name + '\u201d for everyone?', 'Delete').then((y) => {
                if (y)
                  api('DELETE', '/api/packs/' + p.id)
                    .then(() => {
                      toast('Deleted');
                      load();
                    })
                    .catch(fail);
              });
            }}
          >
            Delete
          </button>
        )}
      </div>
    </div>
  );
}

export function PacksPanel() {
  const [packs, setPacks] = useState<ServerPack[] | null>(null);
  const [uploading, setUploading] = useState(false);
  const fileRef = useRef<HTMLInputElement>(null);

  const load = () => {
    api<{ packs: ServerPack[] }>('GET', '/api/packs')
      .then((r) => setPacks(r.packs))
      .catch(fail);
  };
  useEffect(load, []);

  // the raw zip as the body (api() sends JSON only); tools/packs.py checks it against docs/audio-packs.md
  const onUpload = () => {
    const fileIn = fileRef.current!;
    const f = fileIn.files && fileIn.files[0];
    fileIn.value = '';
    if (!f) return;
    setUploading(true);
    toast('Uploading\u2026');
    fetch('/api/packs', {
      method: 'POST',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'application/zip' },
      body: f,
    })
      .then(async (r) => {
        const body = (await r.json().catch(() => null)) as { pack?: ServerPack; message?: string } | null;
        if (r.status === 201 && body?.pack) {
          toast('Uploaded \u201c' + body.pack.name + '\u201d. Players can turn it on in the game\u2019s Esc menu.');
          load();
        } else if (r.status === 401) {
          fail({ status: 401 });
        } else {
          toast(body?.message || 'HTTP ' + r.status, true);
        }
      })
      .catch(fail)
      .then(() => setUploading(false));
  };

  return (
    <>
      <p class="muted">
        Replacement songs and sound effects for everyone on this server (format: docs/audio-packs.md). Only upload
        audio you have the right to share.
      </p>
      <div class="row" style="margin-top:10px">
        <button class="btn" type="button" disabled={uploading} onClick={() => fileRef.current!.click()}>
          {'Upload pack\u2026'}
        </button>
        <input ref={fileRef} type="file" accept=".zip" style="display:none" onChange={onUpload} />
      </div>
      <div class="panel-grid" style="margin-top:10px">
        {packs && !packs.length && <p class="muted">No audio packs yet.</p>}
        {packs && packs.map((p) => <Tile key={p.id} p={p} load={load} />)}
      </div>
    </>
  );
}
