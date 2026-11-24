// ROM-free end-to-end tests against tools/serve.py serving build/web (run `npm run build` first): the game page's
// ROM drop zone and window globals without the gate, and passkey sign-in + the dashboard with it.
import fs from 'node:fs';
import { defineConfig, devices } from '@playwright/test';

const python = process.env.PYTHON ?? (fs.existsSync('tools/.venv/bin/python') ? 'tools/.venv/bin/python' : 'python3');

// the server's settings come only from here: no WEBBY_KONG_* (a mounted ROM, another db) leaks in from the shell
function env(extra: Record<string, string>): Record<string, string> {
  const out: Record<string, string> = {};
  for (const [k, v] of Object.entries(process.env)) {
    if (v !== undefined && !k.startsWith('WEBBY_KONG_')) out[k] = v;
  }
  return { ...out, ...extra };
}

function server(name: string, port: number, extra: Record<string, string> = {}) {
  const dir = `build/e2e/${name}`;
  return {
    command: `rm -rf ${dir} && mkdir -p ${dir} && ${python} tools/serve.py --dir build/web --port ${port} --db ${dir}/saves.sqlite3`,
    url: `http://localhost:${port}/`,
    reuseExistingServer: false,
    env: env(extra),
    stdout: 'pipe' as const,
  };
}

export default defineConfig({
  testDir: 'tests/e2e',
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  reporter: process.env.CI ? 'list' : 'line',
  use: { ...devices['Desktop Chrome'], trace: 'retain-on-failure' },
  webServer: [
    server('plain', 8090),
    server('auth', 8091, { WEBBY_KONG_AUTH: '1', WEBBY_KONG_EXTERNAL_URL: 'http://localhost:8091' }),
  ],
  projects: [
    { name: 'plain', testMatch: /game\.spec/, use: { baseURL: 'http://localhost:8090' } },
    { name: 'auth', testMatch: /auth\.spec/, use: { baseURL: 'http://localhost:8091' } },
  ],
});
