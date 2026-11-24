import { useEffect, useState } from 'preact/hooks';
import { api } from '../../shared/webauthn';
import { confirmDialog } from '../dialogs';
import { fail } from '../fail';
import { fmt } from '../format';
import { toast } from '../toasts';
import type { PanelProps, User } from '../types';

const HEADS = ['User', 'Status', 'Created', 'Last login', 'Passkeys', ''];

// Owner only: every account, with enable/disable and delete; reloads after its own changes.
export function UsersPanel({ me }: PanelProps) {
  const [users, setUsers] = useState<User[] | null>(null);
  const load = () => {
    api<{ users: User[] }>('GET', '/api/users')
      .then((r) => setUsers(r.users))
      .catch(fail);
  };
  useEffect(load, []);

  return (
    <div>
      {users && (
        <div class="tbl-wrap">
          <table class="tbl">
            <thead>
              <tr>
                {HEADS.map((t, i) => (
                  <th key={i}>{t}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {users.map((u) => {
                const locked = u.owner || u.id === me.user.id;
                const why = u.owner ? 'The owner can\u2019t be changed.' : 'You can\u2019t change your own account.';
                return (
                  <tr key={u.id}>
                    <td>
                      {u.username}
                      {' '}
                      {u.owner ? <span class="badge owner">owner</span> : null}
                      {u.id === me.user.id && !u.owner ? <span class="badge">you</span> : null}
                    </td>
                    <td>
                      <span class={'badge ' + (u.disabled ? 'bad' : 'ok')}>{u.disabled ? 'disabled' : 'enabled'}</span>
                    </td>
                    <td>{fmt(u.created_at)}</td>
                    <td>{fmt(u.last_login_at)}</td>
                    <td>{u.passkey_count}</td>
                    <td>
                      <div class="acts">
                        <button
                          class="btn small"
                          type="button"
                          disabled={locked}
                          title={locked ? why : ''}
                          onClick={() => {
                            api('PATCH', '/api/users/' + u.id, { disabled: !u.disabled })
                              .then(() => {
                                toast(u.disabled ? 'User enabled.' : 'User disabled.');
                                load();
                              })
                              .catch(fail);
                          }}
                        >
                          {u.disabled ? 'Enable' : 'Disable'}
                        </button>
                        <button
                          class="btn small danger"
                          type="button"
                          disabled={locked}
                          title={locked ? why : ''}
                          onClick={() => {
                            confirmDialog(
                              'Delete ' + u.username + '? Their passkeys and sessions are removed. This can\u2019t be undone.',
                              'Delete',
                            ).then((y) => {
                              if (y)
                                api('DELETE', '/api/users/' + u.id)
                                  .then(() => {
                                    toast('User deleted.');
                                    load();
                                  })
                                  .catch(fail);
                            });
                          }}
                        >
                          Delete
                        </button>
                      </div>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
