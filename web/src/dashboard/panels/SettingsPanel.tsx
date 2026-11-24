import { useEffect, useState } from 'preact/hooks';
import { api } from '../../shared/webauthn';
import { fail } from '../fail';
import { toast } from '../toasts';

// Owner only: the open-registration switch, disabled until GET /api/settings answers and during each PATCH.
export function SettingsPanel() {
  const [checked, setChecked] = useState(false);
  const [disabled, setDisabled] = useState(true);

  useEffect(() => {
    api<{ registration_open: boolean }>('GET', '/api/settings')
      .then((s) => {
        setChecked(!!s.registration_open);
        setDisabled(false);
      })
      .catch(fail);
  }, []);

  const onChange = (e: Event & { currentTarget: HTMLInputElement }) => {
    const want = e.currentTarget.checked;
    setChecked(want);
    setDisabled(true);
    api<{ registration_open: boolean }>('PATCH', '/api/settings', { registration_open: want })
      .then((s) => {
        setChecked(!!s.registration_open);
        toast('Registration ' + (s.registration_open ? 'opened' : 'closed') + '.');
      })
      .catch((err) => {
        setChecked(!want);
        fail(err);
      })
      .then(() => setDisabled(false));
  };

  return (
    <>
      <label class="switch">
        <input type="checkbox" checked={checked} disabled={disabled} onChange={onChange} />
        <span>Open registration (anyone can create an account)</span>
      </label>
      <p class="muted">Invite links keep working when registration is closed.</p>
    </>
  );
}
