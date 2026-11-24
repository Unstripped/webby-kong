import { expect, test, type ConsoleMessage } from '@playwright/test';

// no ROM is mounted (api/rom 404s) and a source build has no version.json: those two 404s are expected
const EXPECTED_404 = /\/(api\/rom|version\.json)$/;

test('game page without a ROM: drop zone, wrong ROM, F2 panel, window globals', async ({ page }) => {
  const errors: string[] = [];
  page.on('console', (msg: ConsoleMessage) => {
    if (msg.type() === 'error' && !EXPECTED_404.test(msg.location().url)) errors.push(`${msg.text()} @ ${msg.location().url}`);
  });
  page.on('pageerror', (err) => errors.push(err.message));

  await page.goto('/');
  await expect(page.locator('#rom-title')).toHaveText('INSERT CARTRIDGE');
  await expect(page.locator('#rom-detail')).toHaveText('Drop your DK64 ROM or click');

  await page.locator('#rom').setInputFiles({ name: 'x.z64', mimeType: 'application/octet-stream', buffer: Buffer.alloc(4096) });
  await expect(page.locator('#rom-title')).toHaveText('WRONG CARTRIDGE');
  await expect(page.locator('#rom-detail')).toHaveText('Needs Donkey Kong 64 (USA) · try another');
  await expect(page.locator('#rom-drop')).toHaveAttribute('data-state', 'error');

  await page.keyboard.press('F2');
  await expect(page.locator('#dk64dbg-overlay')).toBeVisible();
  await expect(page.locator('#dk64dbg-overlay')).toContainText('game not started');

  const globals = await page.evaluate(() => [typeof window.dk64dbg, typeof window.dk64qs, typeof window.dk64saves]);
  expect(globals).toEqual(['object', 'object', 'object']);
  expect(errors).toEqual([]);
});
