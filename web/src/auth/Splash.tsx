import { useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';
import type { TargetedSubmitEvent } from 'preact';
import { Banana } from '../shared/Banana';
import {
  api,
  ApiError,
  register,
  signIn,
  supported,
  type CreationOptionsJSON,
  type RequestOptionsJSON,
} from '../shared/webauthn';

export type Mode = 'login' | 'owner' | 'register' | 'invite';

export interface AuthConfig {
  auth: boolean;
  has_users: boolean;
  registration_open: boolean;
  rp_name: string;
  email_enabled: boolean;
}

interface Options<P> {
  challenge_id: string;
  publicKey: P;
}

type MsgKind = 'err' | 'warn' | 'ok';

const HEADS: Record<Mode, readonly [string, string]> = {
  login: ['Sign in to continue', 'Use your passkey. No password needed.'],
  owner: ['Create the owner account', 'First time here. Pick a username, then save a passkey on this device.'],
  register: ['Create an account', 'Pick a username, then save a passkey on this device.'],
  invite: ['You\u2019ve been invited', 'Pick a username, then save a passkey on this device.'],
};

// The post-sign-in destination from ?next=: same-origin absolute paths only ("/x" yes, "//host" and "/\host" no).
export function safeNext(): string {
  const n = new URLSearchParams(location.search).get('next');
  if (n && n.charAt(0) === '/' && n.charAt(1) !== '/' && n.charAt(1) !== '\\') return n;
  return '/';
}

export function Splash() {
  const [mode, setMode] = useState<Mode>('login');
  const [cfg, setCfg] = useState<AuthConfig | null>(null);
  const [inviteToken, setInviteToken] = useState<string | null>(null);
  // null until the first setBusy: the buttons start enabled, as in the static markup before init's render()
  const [busy, setBusyState] = useState<boolean | null>(null);
  const [msg, setMsg] = useState<{ kind: MsgKind; text: string } | null>(null);
  // bumped by every render() of the original controller; each one focuses #username outside login mode
  const [renders, setRenders] = useState(0);
  const busyRef = useRef(false);
  const usernameRef = useRef<HTMLInputElement>(null);

  const say = (kind?: MsgKind, text?: string) => setMsg(kind && text ? { kind, text } : null);
  const setBusy = (b: boolean) => {
    busyRef.current = b;
    setBusyState(b);
  };
  // render() of the original: switch mode, re-apply setBusy(busy), then focus #username outside login mode
  const rerender = (to: Mode) => {
    setMode(to);
    setBusyState(busyRef.current);
    setRenders((n) => n + 1);
  };

  useLayoutEffect(() => {
    if (renders && mode !== 'login') usernameRef.current!.focus();
  }, [renders]);

  useEffect(() => {
    if (!supported()) say('warn', 'Passkeys need HTTPS (or localhost).');
    const m = location.pathname.match(/^\/invite\/([^/]+)/);
    let next: Mode = 'login';
    api<AuthConfig>('GET', '/auth/api/config')
      .then(async (c) => {
        setCfg(c);
        document.title = (c.rp_name || 'Webby') + ' - Sign in';
        if (m) {
          const token = decodeURIComponent(m[1]!);
          setInviteToken(token);
          try {
            await api('GET', '/auth/api/invites/' + encodeURIComponent(token));
            next = 'invite';
          } catch (e) {
            if (!(e instanceof ApiError && e.code === 'invite_invalid')) throw e;
            say('err', 'This invite link is no longer valid. Ask for a new one.');
            next = 'login';
          }
          return;
        }
        next = c.has_users ? 'login' : 'owner';
      })
      .then(
        () => rerender(next),
        (e: Error) => {
          say('err', e.message);
          rerender(next);
        },
      );
  }, []);

  const finish = () => {
    say('ok', 'Signed in. Redirecting\u2026');
    location.assign(safeNext());
  };

  const doLogin = () => {
    if (busyRef.current) return;
    say();
    setBusy(true);
    api<Options<RequestOptionsJSON>>('POST', '/auth/api/login/options', {})
      .then((o) =>
        signIn(o.publicKey).then((cred) =>
          api('POST', '/auth/api/login/complete', { challenge_id: o.challenge_id, credential: cred }),
        ),
      )
      .then(finish, (e: Error) => {
        say('err', e.message);
        setBusy(false);
      });
  };

  const doCreate = (ev: TargetedSubmitEvent<HTMLFormElement>) => {
    ev.preventDefault();
    if (busyRef.current) return;
    const username = usernameRef.current!.value.trim();
    if (!username) return;
    say();
    setBusy(true);
    const body: { username: string; invite?: string | null } = { username };
    if (mode === 'invite') body.invite = inviteToken;
    api<Options<CreationOptionsJSON>>('POST', '/auth/api/register/options', body)
      .then((o) =>
        register(o.publicKey).then((cred) =>
          api('POST', '/auth/api/register/complete', { challenge_id: o.challenge_id, credential: cred }),
        ),
      )
      .then(finish, (e: Error) => {
        say('err', e.message);
        setBusy(false);
      });
  };

  const linkBtn = (label: string, to: Mode) => (
    <button
      type="button"
      class="link"
      onClick={() => {
        say();
        rerender(to);
      }}
    >
      {label}
    </button>
  );

  const disabled = busy !== null && (busy || !supported());
  return (
    <>
      <div class="bn-hero">
        <Banana />
      </div>
      <main class="card" id="card">
        <h1 id="title">{cfg ? cfg.rp_name || 'Webby' : 'Webby'}</h1>
        <h2 id="heading">{HEADS[mode][0]}</h2>
        <p id="sub">{HEADS[mode][1]}</p>

        <form id="form" class={mode === 'login' ? 'hidden' : ''} autocomplete="off" onSubmit={doCreate}>
          <label for="username">Username</label>
          <input
            ref={usernameRef}
            id="username"
            name="username"
            autocapitalize="none"
            autocorrect="off"
            spellcheck={false}
            autocomplete="username webauthn"
            minLength={3}
            maxLength={32}
            pattern="[A-Za-z0-9_.\-]{3,32}"
            required
          />
          <button type="submit" class="primary" id="create-btn" disabled={disabled}>
            {mode === 'owner' ? 'Create owner account' : 'Create account with a passkey'}
          </button>
        </form>

        <button
          type="button"
          class={'primary' + (mode !== 'login' ? ' hidden' : '')}
          id="signin-btn"
          disabled={disabled}
          onClick={doLogin}
        >
          Sign in
        </button>
        <div class="links" id="links">
          {mode === 'login' && cfg && cfg.registration_open && linkBtn('Create account', 'register')}
          {mode === 'register' && linkBtn('Back to sign in', 'login')}
          {mode === 'login' && cfg && !cfg.has_users && linkBtn('Create owner account', 'owner')}
        </div>
        <div class={'msg' + (msg ? ' show ' + msg.kind : '')} id="msg" role="status" aria-live="polite">
          {msg ? msg.text : ''}
        </div>
      </main>
    </>
  );
}
