import { cleanup, render, screen } from '@testing-library/preact';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { Dashboard } from './Dashboard';
import type { Me } from './types';

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });
}

const OWNER: Me = {
  user: {
    id: 1,
    username: 'kong',
    owner: true,
    created_at: '2026-01-01T00:00:00Z',
    last_login_at: null,
    disabled: false,
    passkey_count: 1,
  },
  passkeys: [{ id: 'pk1', name: 'Passkey', created_at: '2026-01-01T00:00:00Z', last_used_at: null }],
  email_enabled: false,
};

function stubApi(routes: Record<string, () => Response>) {
  const fetchMock = vi.fn(async (url: string) => {
    const route = routes[url];
    return route ? route() : json({ error: 'not_found', message: 'not found' }, 404);
  });
  vi.stubGlobal('fetch', fetchMock);
  return fetchMock;
}

afterEach(() => {
  cleanup();
  vi.unstubAllGlobals();
});

describe('Dashboard', () => {
  it('renders the panels for the owner', async () => {
    stubApi({
      '/api/me': () => json(OWNER),
      '/api/saves': () => json({ saves: [], dashboard: true }),
      '/api/players': () => json({ players: [] }),
      '/api/invites': () => json({ invites: [] }),
      '/api/users': () => json({ users: [OWNER.user] }),
      '/api/settings': () => json({ registration_open: false }),
    });

    render(<Dashboard />);

    expect(await screen.findByRole('button', { name: 'Start a new game' })).toBeTruthy();
    expect(screen.getByRole('heading', { name: 'Users' })).toBeTruthy();
    expect(document.getElementById('panel-users')).toBeTruthy();
    expect(document.getElementById('who')!.textContent).toBe('signed in as kong');
  });

  it('sends a signed-out visitor to the splash', async () => {
    stubApi({ '/api/me': () => json({ error: 'unauthorized', message: 'sign in first' }, 401) });
    const assign = vi.spyOn(location, 'assign').mockImplementation(() => {});

    render(<Dashboard />);

    await vi.waitFor(() => expect(assign).toHaveBeenCalledWith('/auth/?next=%2F'));
    expect(document.getElementById('grid')!.children.length).toBe(0);
  });
});
