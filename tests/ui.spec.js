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

test('display-logic (pure functions): model vs probeModel, never-synced, disabled', async ({ page }) => {
  const now = Math.floor(Date.now() / 1000);

  // infoOrWarnLine shows the real last-used `model`, never `probeModel`
  // (this agent is keyed with no dual-window data, so it isn't
  // daemon-dependent — no "Start the daemon" warning to worry about here).
  const infoLine = await page.evaluate((now) => infoOrWarnLine({
    name: 'Claude', model: 'claude-sonnet-5', probeModel: 'claude-haiku-4-5-20251001',
    hasKey: true, balance: -1, resetEpoch: now + 3600,
  }), now);
  expect(infoLine).toContain('claude-sonnet-5');
  expect(infoLine).not.toContain('haiku');

  // Never-synced agent (lastSync: 0, no daemon/on-device push has landed yet)
  // → loading placeholder, no usage card, and for Claude no logo/title either.
  // Note: a lapsed resetEpoch alone does NOT trigger this — see the
  // "stale-but-synced" test below, which is the actual "reset already passed"
  // case and must show last-known data, not Syncing.
  const neverSynced = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: 'claude-sonnet-5', probeModel: '', hasKey: true,
      used: 0, limit: 100, balance: -1, resetEpoch: now - 3600, used7d: 0, resetEpoch7d: 0,
      lastSync: 0, nextSync: 0, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(neverSynced).toContain('disp-loading-dots');
  expect(neverSynced).toContain('>Syncing<');
  expect(neverSynced).not.toContain('Rate Limit');
  expect(neverSynced).not.toContain('disp-usage-sprite'); // no logo while loading (Claude)

  // Disabled agent → "Disabled" placeholder.
  const disabled = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: '', probeModel: '', hasKey: true,
      used: 5, limit: 100, balance: -1, resetEpoch: now + 3600, used7d: 0, resetEpoch7d: 0,
      lastSync: now - 5, nextSync: now + 115, enabled: false, active: true },
    presetFor('Claude'),
  ), now);
  expect(disabled).toContain('Disabled');
});

test('display-logic: stale-but-synced keeps showing last-known data (not Syncing)', async ({ page }) => {
  const now = Math.floor(Date.now() / 1000);

  // Claude with real 5h/7d daemon data whose 5h window has already lapsed —
  // it has synced before (lastSync > 0), so it must keep showing the
  // last-known 52%/etc., NOT fall back to the "Syncing" placeholder.
  // (disp-loading-dots is a separate always-on pulse for any enabled
  // auto-sync agent — see hasAutoSync() in app.js — not a loading indicator,
  // so it's intentionally not asserted against here.)
  const staleDual = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: 'claude-sonnet-5', probeModel: '', hasKey: true,
      used: 52, limit: 100, balance: 12.34, resetEpoch: now - 60,
      used7d: 68, resetEpoch7d: now + 500000,
      lastSync: now - 30, nextSync: now + 90, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(staleDual).not.toContain('>Syncing<');
  expect(staleDual).not.toContain('disp-syncing');
  expect(staleDual).toContain('52%');
  expect(staleDual).toContain('68%');
  expect(staleDual).toContain('Reset due'); // 5h window formatting once resetEpoch has passed

  // Same for the generic (non-Claude) single-window case.
  const staleGeneric = await page.evaluate((now) => renderUsageScreen(
    { name: 'DeepSeek', model: '', probeModel: '', hasKey: true,
      used: 40, limit: 100, balance: -1, resetEpoch: now - 60, used7d: 0, resetEpoch7d: 0,
      lastSync: now - 30, nextSync: now + 90, enabled: true, active: true },
    presetFor('DeepSeek'),
  ), now);
  expect(staleGeneric).not.toContain('>Syncing<');
  expect(staleGeneric).not.toContain('disp-syncing');
  expect(staleGeneric).toContain('40%');
});

test('display-logic: "Sync in" appears on the Claude 5h card and the generic card, not on 7d', async ({ page }) => {
  const now = Math.floor(Date.now() / 1000);

  // Claude dual-window: nextSync should surface as a split "Sync in" row
  // alongside the 5h card's "Resets in", but never on the 7d card — the
  // on-device probe never refreshes 7d data, only the PC daemon does.
  const dual = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: 'claude-sonnet-5', probeModel: '', hasKey: true,
      used: 10, limit: 100, balance: -1, resetEpoch: now + 3600,
      used7d: 20, resetEpoch7d: now + 500000,
      lastSync: now - 5, nextSync: now + 95, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(dual).toContain('disp-usage-reset-split');
  expect(dual).toContain('Sync in');
  // Exactly one split row (the 5h card's) — the 7d card must stay centered.
  expect((dual.match(/disp-usage-reset-split/g) || []).length).toBe(1);

  // Generic single-window card: same split-row treatment.
  const generic = await page.evaluate((now) => renderUsageScreen(
    { name: 'DeepSeek', model: '', probeModel: '', hasKey: true,
      used: 10, limit: 100, balance: -1, resetEpoch: now + 3600, used7d: 0, resetEpoch7d: 0,
      lastSync: now - 5, nextSync: now + 95, enabled: true, active: true },
    presetFor('DeepSeek'),
  ), now);
  expect(generic).toContain('disp-usage-reset-split');
  expect(generic).toContain('Sync in');

  // Keyless/daemon-driven agent (nextSync never set on the device) → no
  // "Sync in" anywhere, even once it has real 5h/7d data from the daemon.
  const daemonDriven = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: 'claude-sonnet-5', probeModel: '', hasKey: false,
      used: 10, limit: 100, balance: -1, resetEpoch: now + 3600,
      used7d: 20, resetEpoch7d: now + 500000,
      lastSync: now - 5, nextSync: 0, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(daemonDriven).not.toContain('disp-usage-reset-split');
  expect(daemonDriven).not.toContain('Sync in');

  // Claude's plain-API-key "Rate Limit" single-window fallback (no 7d data)
  // shows no reset line at all, so "Sync in" has nothing to pair with there —
  // matches the TFT's drawClaudeCard(showReset=false) for that card.
  const rateLimitOnly = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: '', probeModel: '', hasKey: true,
      used: 5, limit: 100, balance: -1, resetEpoch: now + 60, used7d: 0, resetEpoch7d: 0,
      lastSync: now - 5, nextSync: now + 95, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(rateLimitOnly).toContain('Rate Limit');
  expect(rateLimitOnly).not.toContain('Sync in');
});

test('display-logic: "Start the daemon" warning replaces the info line when daemon-dependent data goes stale', async ({ page }) => {
  const now = Math.floor(Date.now() / 1000);
  const STALE = now - 400; // beyond DAEMON_STALE_SEC (250s)
  const FRESH = now - 30;

  // Keyless Claude (fully daemon-dependent) whose daemon has gone quiet —
  // real last-used model/cost would normally show here, but since it's
  // stale, the warning takes over instead of presenting old data as current.
  const staleKeyless = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: 'claude-sonnet-5', probeModel: '', hasKey: false,
      used: 52, limit: 100, balance: 12.34, resetEpoch: now + 3600,
      used7d: 68, resetEpoch7d: now + 500000,
      lastSync: now - 5, nextSync: 0, lastPush: now - 400, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(staleKeyless).toContain('Start the daemon');
  expect(staleKeyless).not.toContain('claude-sonnet-5');
  expect(staleKeyless).not.toContain('today');

  // Same agent, but the daemon pushed recently — normal info line, no warning.
  const fresh = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: 'claude-sonnet-5', probeModel: '', hasKey: false,
      used: 52, limit: 100, balance: 12.34, resetEpoch: now + 3600,
      used7d: 68, resetEpoch7d: now + 500000,
      lastSync: now - 5, nextSync: 0, lastPush: now - 30, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(fresh).not.toContain('Start the daemon');
  expect(fresh).toContain('claude-sonnet-5');

  // Keyed Claude with dual-window daemon data (the exact scenario that
  // motivated this feature — hasKey=true no longer means "daemon doesn't
  // matter") whose daemon has gone stale.
  const keyedButDaemonDependent = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: 'claude-sonnet-5', probeModel: '', hasKey: true,
      used: 55, limit: 100, balance: 33.68, resetEpoch: now + 3600,
      used7d: 68, resetEpoch7d: now + 500000,
      lastSync: now - 5, nextSync: now + 100, lastPush: now - 400, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(keyedButDaemonDependent).toContain('Start the daemon');

  // Codex is always daemon-only, even with no dual-window data yet — a
  // fresh keyless Codex agent with just a single-window "Current" card and
  // no lastPush ever should still warn (never-pushed = maximally stale).
  const codexNeverPushed = await page.evaluate((now) => renderUsageScreen(
    { name: 'Codex', model: '', probeModel: '', hasKey: false,
      used: 10, limit: 0, balance: -1, resetEpoch: now + 3600, used7d: 0, resetEpoch7d: 0,
      lastSync: now - 5, nextSync: 0, lastPush: 0, enabled: true, active: true },
    presetFor('Codex'),
  ), now);
  expect(codexNeverPushed).toContain('Start the daemon');

  // Cursor with a real API key and no dual-window data isn't daemon-dependent
  // at all — never warn regardless of lastPush.
  const cursorKeyedNoDaemon = await page.evaluate((now) => renderUsageScreen(
    { name: 'Cursor', model: '', probeModel: '', hasKey: true,
      used: 40, limit: 100, balance: -1, resetEpoch: now + 3600, used7d: 0, resetEpoch7d: 0,
      lastSync: now - 5, nextSync: now + 100, lastPush: 0, enabled: true, active: true },
    presetFor('Cursor'),
  ), now);
  expect(cursorKeyedNoDaemon).not.toContain('Start the daemon');
});

test('display-logic: daemon-stale threshold is personalized to a keyless agent\'s "Update every" interval', async ({ page }) => {
  const now = Math.floor(Date.now() / 1000);

  // syncInterval=20 -> threshold = max(10,20)*2.5 = 50s. lastPush 60s ago
  // exceeds that personalized threshold, even though it's well under the
  // flat 250s fallback — proves the shorter interval is actually honored,
  // not silently ignored in favor of the fallback.
  const shortIntervalStale = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: '', probeModel: '', hasKey: false,
      used: 52, limit: 100, balance: -1, resetEpoch: now + 3600,
      used7d: 68, resetEpoch7d: now + 500000, syncInterval: 20,
      lastSync: now - 5, nextSync: 0, lastPush: now - 60, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(shortIntervalStale).toContain('Start the daemon');

  // Same interval, but lastPush only 30s ago — under the 50s personalized
  // threshold, so no warning.
  const shortIntervalFresh = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: '', probeModel: '', hasKey: false,
      used: 52, limit: 100, balance: -1, resetEpoch: now + 3600,
      used7d: 68, resetEpoch7d: now + 500000, syncInterval: 20,
      lastSync: now - 5, nextSync: 0, lastPush: now - 30, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(shortIntervalFresh).not.toContain('Start the daemon');

  // syncInterval=200 -> threshold = max(10,200)*2.5 = 500s. lastPush 400s
  // ago exceeds the flat 250s fallback but not this personalized 500s
  // threshold — proves a longer configured interval also extends tolerance,
  // not just shortens it.
  const longIntervalNotStale = await page.evaluate((now) => renderUsageScreen(
    { name: 'Claude', model: '', probeModel: '', hasKey: false,
      used: 52, limit: 100, balance: -1, resetEpoch: now + 3600,
      used7d: 68, resetEpoch7d: now + 500000, syncInterval: 200,
      lastSync: now - 5, nextSync: 0, lastPush: now - 400, enabled: true, active: true },
    presetFor('Claude'),
  ), now);
  expect(longIntervalNotStale).not.toContain('Start the daemon');
});
