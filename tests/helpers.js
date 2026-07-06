// @ts-check
// Shared helpers for both mock and device modes.
const { expect } = require('@playwright/test');

const IS_DEVICE = !!process.env.TT_DEVICE_URL;

/** GET /state -> agents array. Uses the page/request baseURL. */
async function getState(request) {
  const r = await request.get('/state');
  expect(r.ok()).toBeTruthy();
  return (await r.json()).agents;
}

/** POST /command with the correct application/json content type. */
async function command(request, obj) {
  const r = await request.post('/command', {
    headers: { 'Content-Type': 'application/json' },
    data: JSON.stringify(obj),
  });
  expect(r.ok()).toBeTruthy();
  return r;
}

/** Reset mock state to the deterministic seed. No-op on real hardware. */
async function resetMock(request) {
  if (IS_DEVICE) return;
  await request.post('/__reset');
}

/**
 * Add a scratch agent through the real UI at the next free slot, save it, and
 * return its index. Never touches pre-existing agents. `presetName` e.g.
 * 'DeepSeek' | 'Claude' | 'Cursor'. If `apiKey` is given it's typed before Save.
 */
async function addScratchAgent(page, request, presetName, apiKey) {
  const before = (await getState(request)).length;
  await page.click('#btn-add');
  await page.waitForSelector('#preset-grid .preset-btn');
  for (const b of await page.$$('#preset-grid .preset-btn')) {
    if ((await b.innerText()).includes(presetName)) { await b.click(); break; }
  }
  const idx = before; // selectPreset appends at agents.length
  if (apiKey) await page.fill(`.card[data-index="${idx}"] .inp-apikey`, apiKey);
  await page.click(`.card[data-index="${idx}"] .btn-save`);
  // Wait until the device/mock has actually stored it.
  await expect.poll(async () => (await getState(request)).length).toBe(before + 1);
  return idx;
}

/** Delete an agent via the UI (accepts the confirm dialog). */
async function deleteAgent(page, request, index) {
  const before = (await getState(request)).length;
  page.once('dialog', (d) => d.accept());
  await page.click(`.card[data-index="${index}"] .btn-delete`);
  await expect.poll(async () => (await getState(request)).length).toBe(before - 1);
}

module.exports = { IS_DEVICE, getState, command, resetMock, addScratchAgent, deleteAgent };
