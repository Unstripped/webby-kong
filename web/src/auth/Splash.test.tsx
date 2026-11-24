import { cleanup, render, screen } from '@testing-library/preact';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { safeNext, Splash, type AuthConfig } from './Splash';

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });
}

afterEach(() => {
  cleanup();
  vi.unstubAllGlobals();
  history.replaceState(null, '', '/');
});

describe('Splash', () => {
  it('offers the owner account when the server has no users yet', async () => {
    const cfg: AuthConfig = {
      auth: true,
      has_users: false,
      registration_open: true,
      rp_name: 'Webby',
      email_enabled: false,
    };
    const fetchMock = vi.fn(async (url: string) => {
      if (url === '/auth/api/config') return json(cfg);
      return json({ error: 'not_found', message: 'nope' }, 404);
    });
    vi.stubGlobal('fetch', fetchMock);

    render(<Splash />);

    expect(await screen.findByRole('heading', { name: 'Create the owner account' })).toBeTruthy();
    expect(screen.getByRole('button', { name: 'Create owner account' })).toBeTruthy();
    await vi.waitFor(() => expect(document.activeElement).toBe(document.getElementById('username')));
    // happy-dom has no WebAuthn, so render()'s setBusy(busy) disables both buttons, as in the original
    expect((document.getElementById('create-btn') as HTMLButtonElement).disabled).toBe(true);
    expect(fetchMock).toHaveBeenCalledWith('/auth/api/config', expect.objectContaining({ method: 'GET' }));
  });
});

describe('safeNext', () => {
  it('accepts same-origin absolute paths only', () => {
    history.replaceState(null, '', '/auth/?next=//evil.com');
    expect(safeNext()).toBe('/');
    history.replaceState(null, '', '/auth/?next=' + encodeURIComponent('/\\evil.com'));
    expect(safeNext()).toBe('/');
    history.replaceState(null, '', '/auth/?next=' + encodeURIComponent('/play?save=3'));
    expect(safeNext()).toBe('/play?save=3');
  });
});
