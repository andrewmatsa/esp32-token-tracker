// @ts-check
// Browser UI tests — run identically in mock and device modes. CRUD is done on
// scratch agents the test creates and deletes; existing agents are only toggled
// reversibly and restored.
const { test, expect } = require('@playwright/test');
const { getState, command, resetMock, addScratchAgent, deleteAgent } = require('./helpers');

test.beforeEach(async ({ request, page }) => {
  await resetMock(request);
  await page.goto('/');
  // Initial refresh() populates cards + flips the badge to Online.
  await expect(page.locator('#ws-status')).toHaveText('Online');
});

test('loads and renders one card per agent in /state', async ({ page, request }) => {
  const agents = await getState(request);
  await expect(page.locator('.card')).toHaveCount(agents.length);
});

test('scratch CRUD: add → save (auto-active) → delete', async ({ page, request }) => {
  const before = (await getState(request)).length;
  const idx = await addScratchAgent(page, request, 'DeepSeek', 'sk-test-key');

  // Saved agent becomes active and is stored with a key.
  await expect(page.locator(`.card[data-index="${idx}"]`)).toHaveClass(/is-active/);
  const stored = (await getState(request))[idx];
  expect(stored.hasKey).toBe(true);
  expect(stored.active).toBe(true);

  await deleteAgent(page, request, idx);
  expect((await getState(request)).length).toBe(before);
});

test('Claude field logic: keyless daemon field vs keyed dropdown', async ({ page, request }) => {
  const idx = await addScratchAgent(page, request, 'Claude'); // no key → keyless
  const card = page.locator(`.card[data-index="${idx}"]`);

  // Keyless: model input disabled with the daemon placeholder + a ready command.
  await expect(card.locator('.inp-model')).toBeDisabled();
  await expect(card.locator('.inp-model')).toHaveAttribute('placeholder', /Auto-detected by daemon/);
  await expect(card.locator('.daemon-cmd-row')).toBeVisible();
  await expect(card.locator('.daemon-cmd')).toHaveText(new RegExp(`--push claude:${idx}`));

  // Add a key and save → switches to the closed model dropdown; command row hides.
  await card.locator('.inp-apikey').fill('sk-ant-test');
  await card.locator('.btn-save').click();
  await expect.poll(async () => (await getState(request))[idx].hasKey).toBe(true);
  await expect(card.locator('.inp-model-select')).toBeVisible();
  const opts = await card.locator('.inp-model-select option').allTextContents();
  expect(opts.some((o) => o.includes('claude-opus'))).toBeTruthy();
  await expect(card.locator('.daemon-cmd-row')).toBeHidden();

  await deleteAgent(page, request, idx);
});

test('enable toggle round-trip on an existing agent (restored)', async ({ page, request }) => {
  const before = await getState(request);
  test.skip(before.length === 0, 'no agents');
  const original = before[0].enabled !== false;
  const card = page.locator('.card[data-index="0"]');
  const box = '.card[data-index="0"] .inp-enabled';

  await page.setChecked(box, false, { force: true });
  await expect(card).toHaveClass(/is-disabled/);
  await expect.poll(async () => (await getState(request))[0].enabled).toBe(false);

  await page.setChecked(box, true, { force: true });
  await expect(card).not.toHaveClass(/is-disabled/);
  await expect.poll(async () => (await getState(request))[0].enabled).toBe(true);

  // Restore original.
  await command(request, { type: 'setEnabled', index: 0, enabled: original });
});

test('offline badge when /state is unreachable, recovers when restored', async ({ page }) => {
  await page.route('**/state', (r) => r.abort());
  await page.evaluate(() => refresh());
  await expect(page.locator('#ws-status')).toHaveText('Offline');

  await page.unroute('**/state');
  await page.evaluate(() => refresh());
  await expect(page.locator('#ws-status')).toHaveText('Online');
});

test('background poll picks up an out-of-band state change (restored)', async ({ page, request }) => {
  const before = await getState(request);
  test.skip(before.length === 0, 'no agents');
  const original = before[0].enabled !== false;

  // Change state out-of-band (as a daemon or another tab would), then let the
  // page poll — a forced refresh() stands in for the 15 s interval tick.
  await command(request, { type: 'setEnabled', index: 0, enabled: !original });
  await page.evaluate(() => refresh());
  const card = page.locator('.card[data-index="0"]');
  // Flipping to !original: if it was enabled it should now show disabled, and
  // vice-versa — assert the class matches the new (flipped) state.
  if (original) await expect(card).toHaveClass(/is-disabled/);
  else await expect(card).not.toHaveClass(/is-disabled/);

  await command(request, { type: 'setEnabled', index: 0, enabled: original });
  await page.evaluate(() => refresh());
});

test('display-logic (pure functions): model vs probeModel, stale, disabled', async ({ page }) => {
  const now = Math.floor(Date.now() / 1000);

  // claudeInfoLine shows the real last-used `model`, never `probeModel`.
  const infoLine = await page.evaluate((now) => claudeInfoLine({
    name: 'Claude', model: 'claude-sonnet-5', probeModel: 'claude-haiku-4-5-20251001',
    hasKey: true, balance: -1, resetEpoch: now + 3600,
  }), now);
  expect(infoLine).toContain('claude-sonnet-5');
  expect(infoLine).not.toContain('haiku');

  // Stale agent (reset already passed) → loading placeholder, no usage card, and
  // for Claude no logo/title either.
  const stale = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: 'claude-sonnet-5', probeModel: '', hasKey: true,
      used: 0, limit: 100, balance: -1, resetEpoch: now - 3600, used7d: 0, resetEpoch7d: 0,
      enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(stale).toContain('disp-loading-dots');
  expect(stale).toContain('>Syncing<');
  expect(stale).not.toContain('Rate Limit');
  expect(stale).not.toContain('disp-usage-sprite'); // no logo while loading (Claude)

  // Disabled agent → "Disabled" placeholder.
  const disabled = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: '', probeModel: '', hasKey: true,
      used: 5, limit: 100, balance: -1, resetEpoch: now + 3600, used7d: 0, resetEpoch7d: 0,
      enabled: false, active: true },
    presetFor('Claude'),
  ), now);
  expect(disabled).toContain('Disabled');
});
