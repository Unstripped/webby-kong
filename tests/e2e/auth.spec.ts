import { expect, test } from '@playwright/test';

// The passkey gate end to end with a CDP virtual authenticator: owner sign-up on the public splash (its bundle is
// under /assets/), the dashboard's saves CRUD, sign-out and sign-in with the same passkey.
test('owner signs up, manages saves, signs out and back in', async ({ page, context }) => {
  const cdp = await context.newCDPSession(page);
  await cdp.send('WebAuthn.enable');
  await cdp.send('WebAuthn.addVirtualAuthenticator', {
    options: {
      protocol: 'ctap2',
      transport: 'internal',
      hasResidentKey: true,
      hasUserVerification: true,
      isUserVerified: true,
    },
  });

  await page.goto('/');
  await expect(page).toHaveURL(/\/auth\/\?next=%2F$/);
  await expect(page.locator('#banana canvas')).toBeAttached();

  await expect(page.locator('#heading')).toHaveText('Create the owner account');
  await page.locator('#username').fill('kongtest');
  await page.locator('#create-btn').click();
  await expect(page).toHaveURL(/\/$/);

  await expect(page.locator('#who')).toContainText('kongtest');
  await expect(page.locator('#panel-users')).toBeVisible();

  const saves = page.locator('#panel-saves');
  await saves.getByLabel('New save name').fill('E2E run');
  await saves.getByRole('button', { name: 'New save' }).click();
  await expect(page.locator('#toasts')).toContainText('Save created.');
  const tile = saves.locator('.tile').filter({ has: page.locator('h3', { hasText: 'E2E run' }) });
  await expect(tile).toHaveCount(1);

  await tile.getByRole('button', { name: 'Delete' }).click();
  await page.locator('.modal').getByRole('button', { name: 'Delete' }).click();
  await expect(page.locator('#toasts')).toContainText('Save deleted.');
  await expect(tile).toHaveCount(0);

  await page.locator('#signout').click();
  await expect(page).toHaveURL(/\/auth\/$/);
  await page.locator('#signin-btn').click();
  await expect(page).toHaveURL(/\/$/);
  await expect(page.locator('#who')).toContainText('kongtest');
});
