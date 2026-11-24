import { useEffect, useRef, useState } from 'preact/hooks';
import { api, register, supported, type CreationOptionsJSON } from '../../shared/webauthn';
import { confirmDialog } from '../dialogs';
import { fail } from '../fail';
import { fmt } from '../format';
import { toast } from '../toasts';
import type { PanelProps, Passkey } from '../types';

interface TileProps {
  p: Passkey;
  only: boolean;
  refreshMe: () => Promise<void>;
  rerender: () => void;
}

// One passkey; Rename swaps the name for an inline editor until the list re-renders.
function PasskeyTile({ p, only, refreshMe, rerender }: TileProps) {
  const [editing, setEditing] = useState(false);
  const [saving, setSaving] = useState(false);
  const inputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    if (!editing) return;
    inputRef.current!.focus();
    inputRef.current!.select();
  }, [editing]);

  const doSave = () => {
    const v = inputRef.current!.value.trim();
    if (!v) return;
    setSaving(true);
    api('PATCH', '/api/me/passkeys/' + encodeURIComponent(p.id), { name: v })
      .then(() => refreshMe())
      .then(rerender)
      .catch((e) => {
        setSaving(false);
        fail(e);
      });
  };

  return (
    <div class="tile" style="margin-bottom:8px">
      {editing ? (
        <div class="row">
          <div class="grow">
            <input
              ref={inputRef}
              type="text"
              defaultValue={p.name}
              maxLength={40}
              aria-label="Passkey name"
              onKeyDown={(e) => {
                if (e.key === 'Enter') doSave();
                else if (e.key === 'Escape') rerender();
              }}
            />
          </div>
          <button class="btn small" type="button" disabled={saving} onClick={doSave}>
            Save
          </button>
          <button class="btn small ghost" type="button" onClick={rerender}>
            Cancel
          </button>
        </div>
      ) : (
        <b>{p.name}</b>
      )}
      <div class="muted">
        {'Added ' + fmt(p.created_at) + ' \u00b7 Last used ' + (p.last_used_at ? fmt(p.last_used_at) : 'never')}
      </div>
      <div class="acts" style={editing ? 'margin-top:8px;display:none' : 'margin-top:8px'}>
        <button class="btn small" type="button" onClick={() => setEditing(true)}>
          Rename
        </button>
        <button
          class="btn small danger"
          type="button"
          disabled={only}
          title={only ? 'You can\u2019t remove your only passkey.' : 'Remove this passkey'}
          onClick={() => {
            confirmDialog(
              'Remove passkey \u201c' + p.name + '\u201d? Devices using it will no longer be able to sign in.',
              'Remove',
            ).then((y) => {
              if (!y) return;
              api('DELETE', '/api/me/passkeys/' + encodeURIComponent(p.id))
                .then(() => {
                  toast('Passkey removed.');
                  return refreshMe();
                })
                .then(rerender)
                .catch(fail);
            });
          }}
        >
          Remove
        </button>
      </div>
    </div>
  );
}

export function AccountPanel({ me, refreshMe, signOut }: PanelProps) {
  // render() of the original: bumping it rebuilds the passkey list, closing any open rename editor
  const [version, setVersion] = useState(0);
  const [adding, setAdding] = useState(false);
  const nameIn = useRef<HTMLInputElement>(null);
  const rerender = () => setVersion((n) => n + 1);
  const only = me.passkeys.length <= 1;

  const addPasskey = () => {
    setAdding(true);
    api<{ challenge_id: string; publicKey: CreationOptionsJSON }>('POST', '/api/me/passkeys/options', {})
      .then((o) =>
        register(o.publicKey).then((cred) => {
          const body: { challenge_id: string; credential: unknown; name?: string } = {
            challenge_id: o.challenge_id,
            credential: cred,
          };
          if (nameIn.current!.value.trim()) body.name = nameIn.current!.value.trim();
          return api('POST', '/api/me/passkeys/complete', body);
        }),
      )
      .then(() => {
        toast('Passkey added.');
        nameIn.current!.value = '';
        return refreshMe();
      })
      .then(rerender)
      .catch(fail)
      .then(() => setAdding(false));
  };

  return (
    <>
      <p>
        {'Signed in as '}
        <b>{me.user.username}</b>
        {' '}
        {me.user.owner ? <span class="badge owner">owner</span> : null}
      </p>
      <div>
        {only && <div class="note">{'Add a second passkey so a lost device doesn\u2019t lock you out.'}</div>}
        {me.passkeys.map((p) => (
          <PasskeyTile key={p.id + '/' + version} p={p} only={only} refreshMe={refreshMe} rerender={rerender} />
        ))}
      </div>
      {!supported() && <div class="note">Passkeys need HTTPS (or localhost).</div>}
      <div class="row">
        <div class="grow">
          <input
            ref={nameIn}
            type="text"
            maxLength={40}
            placeholder="Name, e.g. iPhone (optional)"
            aria-label="New passkey name"
          />
        </div>
        <button class="btn" type="button" disabled={adding || !supported()} onClick={addPasskey}>
          Add passkey
        </button>
      </div>
      <div class="row">
        <button class="btn ghost" type="button" onClick={signOut}>
          Sign out
        </button>
      </div>
    </>
  );
}
