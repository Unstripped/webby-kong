// Shared WebAuthn helpers + JSON fetch wrapper, used by the sign-in splash and the dashboard.

// An HTTP or network failure from api(). `status` is the HTTP status, or 0 when the server was not reached
// (`code === 'network'`).
export class ApiError extends Error {
  readonly code: string;
  readonly status: number;
  constructor(message: string, code: string, status: number) {
    super(message);
    this.name = 'ApiError';
    this.code = code;
    this.status = status;
  }
}

// A passkey prompt failure with a user-safe message; `code` is 'webauthn_<DOMException name>'.
export class WebauthnError extends Error {
  readonly code: string;
  constructor(message: string, code: string) {
    super(message);
    this.name = 'WebauthnError';
    this.code = code;
  }
}

interface CredentialDescriptorJSON extends Omit<PublicKeyCredentialDescriptor, 'id'> {
  id: string;
}

// PublicKeyCredentialCreationOptions as the server sends it: binary fields base64url.
export interface CreationOptionsJSON
  extends Omit<PublicKeyCredentialCreationOptions, 'challenge' | 'user' | 'excludeCredentials'> {
  challenge: string;
  user: Omit<PublicKeyCredentialUserEntity, 'id'> & { id: string };
  excludeCredentials?: CredentialDescriptorJSON[];
}

// PublicKeyCredentialRequestOptions as the server sends it: binary fields base64url.
export interface RequestOptionsJSON extends Omit<PublicKeyCredentialRequestOptions, 'challenge' | 'allowCredentials'> {
  challenge: string;
  allowCredentials?: CredentialDescriptorJSON[];
}

export interface RegistrationCredentialJSON {
  id: string;
  rawId: string;
  type: string;
  response: { clientDataJSON: string; attestationObject: string; transports: string[] };
  clientExtensionResults: Record<string, never>;
}

export interface AuthenticationCredentialJSON {
  id: string;
  rawId: string;
  type: string;
  response: { clientDataJSON: string; authenticatorData: string; signature: string; userHandle: string | null };
  clientExtensionResults: Record<string, never>;
}

export function b64uToBuf(s: string): ArrayBuffer {
  s = String(s).replace(/-/g, '+').replace(/_/g, '/');
  while (s.length % 4) s += '=';
  const bin = atob(s);
  const u = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
  return u.buffer;
}

export function bufToB64u(buf: ArrayBuffer): string {
  const u = new Uint8Array(buf);
  let s = '';
  for (let i = 0; i < u.length; i++) s += String.fromCharCode(u[i]!);
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function mapIds(list: CredentialDescriptorJSON[]): PublicKeyCredentialDescriptor[] {
  return list.map((d) => ({ ...d, id: b64uToBuf(d.id) }));
}

function prepareCreationOptions(pk: CreationOptionsJSON): PublicKeyCredentialCreationOptions {
  const { challenge, user, excludeCredentials, ...rest } = pk;
  const o: PublicKeyCredentialCreationOptions = {
    ...rest,
    challenge: b64uToBuf(challenge),
    user: { ...user, id: b64uToBuf(user.id) },
  };
  if (excludeCredentials) o.excludeCredentials = mapIds(excludeCredentials);
  return o;
}

function prepareRequestOptions(pk: RequestOptionsJSON): PublicKeyCredentialRequestOptions {
  const { challenge, allowCredentials, ...rest } = pk;
  const o: PublicKeyCredentialRequestOptions = { ...rest, challenge: b64uToBuf(challenge) };
  if (allowCredentials) o.allowCredentials = mapIds(allowCredentials);
  return o;
}

export function supported(): boolean {
  return !!(window.PublicKeyCredential && window.isSecureContext && navigator.credentials);
}

function friendly(err: unknown): string | null {
  const n = err instanceof Error || err instanceof DOMException ? err.name : undefined;
  if (n === 'NotAllowedError') return 'The passkey prompt was cancelled or timed out.';
  if (n === 'InvalidStateError') return 'This device already has a passkey for this account.';
  if (n === 'SecurityError') return 'Passkeys are not allowed on this address. Use HTTPS (or localhost).';
  if (n === 'NotSupportedError') return 'This browser or device does not support the required passkey type.';
  return null;
}

function wrap(err: unknown): never {
  const m = friendly(err);
  if (m) throw new WebauthnError(m, 'webauthn_' + ((err as Error).name || 'error'));
  throw err;
}

// Resolves to the credential JSON the server expects; rejects with an Error (message is user-safe).
export async function register(publicKey: CreationOptionsJSON): Promise<RegistrationCredentialJSON> {
  try {
    const c = (await navigator.credentials.create({ publicKey: prepareCreationOptions(publicKey) })) as PublicKeyCredential;
    const r = c.response as AuthenticatorAttestationResponse;
    return {
      id: c.id,
      rawId: bufToB64u(c.rawId),
      type: c.type,
      response: {
        clientDataJSON: bufToB64u(r.clientDataJSON),
        attestationObject: bufToB64u(r.attestationObject),
        transports: typeof r.getTransports === 'function' ? r.getTransports() : [],
      },
      clientExtensionResults: {},
    };
  } catch (err) {
    return wrap(err);
  }
}

export async function signIn(publicKey: RequestOptionsJSON): Promise<AuthenticationCredentialJSON> {
  try {
    const c = (await navigator.credentials.get({ publicKey: prepareRequestOptions(publicKey) })) as PublicKeyCredential;
    const r = c.response as AuthenticatorAssertionResponse;
    return {
      id: c.id,
      rawId: bufToB64u(c.rawId),
      type: c.type,
      response: {
        clientDataJSON: bufToB64u(r.clientDataJSON),
        authenticatorData: bufToB64u(r.authenticatorData),
        signature: bufToB64u(r.signature),
        userHandle: r.userHandle ? bufToB64u(r.userHandle) : null,
      },
      clientExtensionResults: {},
    };
  } catch (err) {
    return wrap(err);
  }
}

// Same-origin JSON call. Non-GET always sends a JSON body. A 204 (or an empty body) resolves null, so type T
// accordingly. Rejects with ApiError{code,status,message}.
export async function api<T>(method: string, url: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = { Accept: 'application/json' };
  const init: RequestInit = { method, credentials: 'same-origin', headers, cache: 'no-store' };
  if (method !== 'GET' && method !== 'HEAD') {
    headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body === undefined ? {} : body);
  }
  let res: Response;
  try {
    res = await fetch(url, init);
  } catch {
    throw new ApiError('Could not reach the server. Check your connection.', 'network', 0);
  }
  if (res.status === 204) return null as T;
  const txt = await res.text();
  let data: { message?: string; error?: string } | null = null;
  try {
    data = txt ? JSON.parse(txt) : null;
  } catch {
    /* non-JSON body */
  }
  if (res.ok) return data as T;
  throw new ApiError(
    (data && data.message) || 'Request failed (' + res.status + ')',
    (data && data.error) || 'http_' + res.status,
    res.status,
  );
}
