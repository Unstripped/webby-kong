import { useEffect, useRef, useState } from 'preact/hooks';
import { api } from '../../shared/webauthn';
import { fail } from '../fail';
import { fmt } from '../format';
import { toast } from '../toasts';
import type { Invite, PanelProps } from '../types';

function copy(text: string): void {
  function fallback() {
    const ta = document.createElement('textarea');
    ta.style.cssText = 'position:fixed;opacity:0';
    ta.value = text;
    document.body.appendChild(ta);
    ta.select();
    let ok = false;
    try {
      ok = document.execCommand('copy');
    } catch {
      /* ignore */
    }
    document.body.removeChild(ta);
    toast(ok ? 'Link copied.' : 'Copy failed. Select the link and copy it manually.', !ok);
  }
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).then(() => toast('Link copied.'), fallback);
  } else fallback();
}

function InviteRow({ inv, owner, load }: { inv: Invite; owner: boolean; load: () => void }) {
  const [revoking, setRevoking] = useState(false);
  // derived client-side from the browser clock; the server sends no state
  const exp = inv.expires_at && new Date(inv.expires_at) < new Date();
  const exhausted = inv.max_uses != null && inv.uses >= inv.max_uses;
  const state = inv.revoked ? ['revoked', 'bad'] : exp ? ['expired', 'bad'] : exhausted ? ['used up', 'bad'] : ['active', 'ok'];
  return (
    <tr>
      <td>
        {inv.note || '\u2013'}
        {inv.last_email_to ? <div class="muted">{'emailed ' + inv.last_email_to}</div> : null}
      </td>
      {owner && <td>{inv.created_by || ''}</td>}
      <td>{inv.uses + '/' + (inv.max_uses == null ? '\u221e' : inv.max_uses)}</td>
      <td>{inv.expires_at ? fmt(inv.expires_at) : 'never'}</td>
      <td>
        <span class={'badge ' + state[1]}>{state[0]}</span>
      </td>
      <td>
        <div class="acts">
          {state[1] === 'ok' && (
            <button
              class="btn small danger"
              type="button"
              disabled={revoking}
              onClick={() => {
                setRevoking(true);
                api('DELETE', '/api/invites/' + inv.id).then(
                  () => {
                    toast('Invite revoked.');
                    load();
                  },
                  (er) => {
                    setRevoking(false);
                    fail(er);
                  },
                );
              }}
            >
              Revoke
            </button>
          )}
        </div>
      </td>
    </tr>
  );
}

export function InvitesPanel({ me }: PanelProps) {
  const [invites, setInvites] = useState<Invite[] | null>(null);
  const [multi, setMulti] = useState(false);
  const [going, setGoing] = useState(false);
  const [created, setCreated] = useState<{ url: string; emailedTo: string | null } | null>(null);
  const maxSel = useRef<HTMLSelectElement>(null);
  const maxIn = useRef<HTMLInputElement>(null);
  const expSel = useRef<HTMLSelectElement>(null);
  const note = useRef<HTMLInputElement>(null);
  const mail = useRef<HTMLInputElement>(null);

  const load = () => {
    api<{ invites: Invite[] }>('GET', '/api/invites')
      .then((r) => setInvites(r.invites))
      .catch(fail);
  };
  useEffect(load, []);

  const onSubmit = (e: Event) => {
    e.preventDefault();
    let max: number | null = 1;
    if (maxSel.current!.value === 'multi') max = maxIn.current!.value ? parseInt(maxIn.current!.value, 10) : null;
    setGoing(true);
    api<{ url: string; email_sent_to: string | null }>('POST', '/api/invites', {
      max_uses: max,
      expires_in_days: expSel.current!.value ? parseInt(expSel.current!.value, 10) : null,
      note: note.current!.value.trim() || null,
      email: mail.current!.value.trim() || null,
    })
      .then((r) => {
        setCreated({ url: r.url, emailedTo: r.email_sent_to });
        note.current!.value = '';
        mail.current!.value = '';
        load();
      })
      .catch(fail)
      .then(() => setGoing(false));
  };

  const owner = me.user.owner;
  return (
    <>
      <form onSubmit={onSubmit}>
        <div class="row">
          <div class="grow">
            <select ref={maxSel} id="inv-type" onChange={(e) => setMulti(e.currentTarget.value === 'multi')}>
              <option value="single">Single-use</option>
              <option value="multi">Reusable</option>
            </select>
          </div>
          <div class="grow">
            <input
              ref={maxIn}
              type="number"
              min="2"
              placeholder="Max uses (blank = unlimited)"
              aria-label="Max uses"
              style={{ display: multi ? '' : 'none' }}
            />
          </div>
        </div>
        <div class="row">
          <div class="grow">
            <select ref={expSel} aria-label="Expires in">
              <option value="">Never expires</option>
              <option value="1">1 day</option>
              <option value="7">7 days</option>
              <option value="30">30 days</option>
            </select>
          </div>
          <div class="grow">
            <input ref={note} type="text" maxLength={120} placeholder="Note (optional)" aria-label="Note" />
          </div>
        </div>
        <div class="row">
          <div class="grow">
            <input
              ref={mail}
              type="email"
              placeholder="Email the link (optional)"
              aria-label="Email recipient"
              disabled={!me.email_enabled}
              title={me.email_enabled ? 'Send the link once it is created' : 'Email is not configured on this server.'}
            />
          </div>
          <button class="btn" type="submit" disabled={going}>
            Create link
          </button>
        </div>
      </form>
      {!me.email_enabled && (
        <p class="muted">Email sending is not configured on this server: copy the link and send it yourself.</p>
      )}
      <div>
        {created && (
          <>
            <div class="link">
              <input readOnly value={created.url} aria-label="Invite link" onFocus={(e) => e.currentTarget.select()} />
              <button class="btn" type="button" onClick={() => copy(created.url)}>
                Copy
              </button>
            </div>
            <p class="muted">{'This is the only time the link is shown \u2014 copy it now.'}</p>
            {created.emailedTo && <p class="muted">{'Emailed to ' + created.emailedTo + '.'}</p>}
          </>
        )}
      </div>
      <div style="height:12px" />
      <div>
        {invites &&
          (!invites.length ? (
            <p class="muted">No invites yet.</p>
          ) : (
            <div class="tbl-wrap">
              <table class="tbl">
                <thead>
                  <tr>
                    <th>Note</th>
                    {owner && <th>By</th>}
                    <th>Uses</th>
                    <th>Expires</th>
                    <th>State</th>
                    <th></th>
                  </tr>
                </thead>
                <tbody>
                  {invites.map((inv) => (
                    <InviteRow key={inv.id} inv={inv} owner={owner} load={load} />
                  ))}
                </tbody>
              </table>
            </div>
          ))}
      </div>
    </>
  );
}
