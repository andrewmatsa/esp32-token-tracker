// @ts-check
// HTTP-level tests (no browser) — run identically in mock and device modes.
const { test, expect } = require('@playwright/test');
const { getState, command, resetMock } = require('./helpers');

test.beforeEach(async ({ request }) => {
  await resetMock(request);
});

test('GET / serves the dashboard HTML', async ({ request }) => {
  const r = await request.get('/');
  expect(r.status()).toBe(200);
  expect(await r.text()).toContain('Token Tracker');
});

test('GET /state returns a well-formed agents array', async ({ request }) => {
  const agents = await getState(request);
  expect(Array.isArray(agents)).toBeTruthy();
  for (const a of agents) {
    expect(a).toHaveProperty('name');
    expect(a).toHaveProperty('hasKey');
    expect(a).toHaveProperty('enabled');
    expect(a).toHaveProperty('active');
    expect(a).toHaveProperty('used');
    expect(a).toHaveProperty('limit');
    expect(a).toHaveProperty('probeModel');
    // lastSync/nextSync are the ephemeral scheduler fields added for the
    // "never synced" vs "stale but shows last-known data" vs "Sync in"
    // display logic — never persisted device-side, but always present on
    // /state (0 when not applicable, e.g. a keyless/daemon-driven agent).
    expect(a).toHaveProperty('lastSync');
    expect(a).toHaveProperty('nextSync');
    // apiKey must never be exposed.
    expect(a).not.toHaveProperty('apiKey');
  }
});

test('GET /wifi/info returns ssid and ip', async ({ request }) => {
  const r = await request.get('/wifi/info');
  expect(r.status()).toBe(200);
  const info = await r.json();
  expect(info).toHaveProperty('ssid');
  expect(info).toHaveProperty('ip');
});

test('POST /command setActive applies and is reflected in /state (restored)', async ({ request }) => {
  const before = await getState(request);
  test.skip(before.length === 0, 'no agents to activate');
  const prevActive = Math.max(0, before.findIndex((a) => a.active));

  // Activate the last agent, verify, then restore the original active one.
  const target = before.length - 1;
  const res = await command(request, { type: 'setActive', index: target });
  expect(await res.json()).toMatchObject({ ok: true });

  const after = await getState(request);
  expect(after[target].active).toBe(true);
  after.forEach((a, i) => { if (i !== target) expect(a.active).toBe(false); });

  await command(request, { type: 'setActive', index: prevActive });
});

test('POST /command setEnabled toggles and restores', async ({ request }) => {
  const before = await getState(request);
  test.skip(before.length === 0, 'no agents');
  const original = before[0].enabled !== false;

  await command(request, { type: 'setEnabled', index: 0, enabled: !original });
  await expect.poll(async () => (await getState(request))[0].enabled).toBe(!original);

  await command(request, { type: 'setEnabled', index: 0, enabled: original });
  await expect.poll(async () => (await getState(request))[0].enabled).toBe(original);
});

test('setEnabled(true) on a keyed agent responds quickly (non-blocking fetch)', async ({ request }) => {
  const before = await getState(request);
  const idx = before.findIndex((a) => a.hasKey);
  test.skip(idx === -1, 'no keyed agent present');
  const wasEnabled = before[idx].enabled !== false;

  const t0 = Date.now();
  await command(request, { type: 'setEnabled', index: idx, enabled: true });
  const elapsed = Date.now() - t0;
  // The old blocking fetcher_sync() could stall this several seconds; the
  // deferred-fetch fix returns immediately.
  expect(elapsed).toBeLessThan(3000);

  await command(request, { type: 'setEnabled', index: idx, enabled: wasEnabled });
});
