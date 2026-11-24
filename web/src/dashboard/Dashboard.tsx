import type { ComponentType } from 'preact';
import { useEffect, useState } from 'preact/hooks';
import { Banana } from '../shared/Banana';
import { api } from '../shared/webauthn';
import { DialogHost } from './dialogs';
import { fail } from './fail';
import { AccountPanel } from './panels/AccountPanel';
import { InvitesPanel } from './panels/InvitesPanel';
import { PlayersPanel } from './panels/PlayersPanel';
import { SavesPanel } from './panels/SavesPanel';
import { SettingsPanel } from './panels/SettingsPanel';
import { UsersPanel } from './panels/UsersPanel';
import { Toasts } from './toasts';
import type { Me, PanelProps } from './types';

// A registry of panels rendered into the grid. Add a mode/panel by adding an entry.
const PANELS: { id: string; title: string; cls?: string; owner?: boolean; Body: ComponentType<PanelProps> }[] = [
  { id: 'saves', title: 'Your saves', cls: 'wide', Body: SavesPanel },
  { id: 'players', title: 'Players', cls: 'span2', Body: PlayersPanel },
  { id: 'account', title: 'Account', Body: AccountPanel },
  { id: 'invites', title: 'Invites', cls: 'span2', Body: InvitesPanel },
  { id: 'users', title: 'Users', cls: 'span2', owner: true, Body: UsersPanel },
  { id: 'settings', title: 'Settings', owner: true, Body: SettingsPanel },
];

function signOut(): void {
  api('POST', '/auth/api/logout', {})
    .catch(() => {
      /* cookie cleared or already gone */
    })
    .then(() => location.assign('/auth/'));
}

export function Dashboard() {
  // {user, passkeys, email_enabled}; null until the first GET /api/me, so the grid stays empty on failure
  const [me, setMe] = useState<Me | null>(null);
  const refreshMe = () => api<Me>('GET', '/api/me').then(setMe);

  useEffect(() => {
    refreshMe().catch(fail);
  }, []);

  return (
    <>
      <header class="hdr">
        <div class="brand">
          <Banana />
          <span class="wordmark">Webby</span>
        </div>
        <div class="who">
          <span id="who">
            {me && (
              <>
                {'signed in as '}
                <b>{me.user.username}</b>
              </>
            )}
          </span>
          <button type="button" id="signout" class="btn ghost" onClick={signOut}>
            Sign out
          </button>
        </div>
      </header>
      <main id="grid" class="grid" aria-live="polite">
        {me &&
          PANELS.filter((p) => !p.owner || me.user.owner).map(({ id, title, cls, Body }) => (
            <section key={id} class={'panel ' + (cls || '')} id={'panel-' + id}>
              <h2>{title}</h2>
              <div>
                <Body me={me} refreshMe={refreshMe} signOut={signOut} />
              </div>
            </section>
          ))}
      </main>
      <Toasts />
      <DialogHost />
    </>
  );
}
