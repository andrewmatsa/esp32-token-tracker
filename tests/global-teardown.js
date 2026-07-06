// @ts-check
// Device mode only: restore the device to its pre-run snapshot.
//  1. Delete any leaked agents (index >= snapshot length) — scratch agents a
//     test created but failed to clean up. Deleted high-to-low to keep indices
//     stable.
//  2. Restore each original agent's name / probeModel / enabled, then the
//     original active selection. apiKey is preserved automatically (an `update`
//     with no apiKey never clears the stored key), so keys are never lost.
const fs = require('fs');
const path = require('path');

async function command(base, obj) {
  await fetch(`${base}/command`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(obj),
  });
}

module.exports = async () => {
  const base = process.env.TT_DEVICE_URL;
  if (!base) return;
  const snapPath = path.join(__dirname, 'test-results', '_snapshot.json');
  if (!fs.existsSync(snapPath)) return;
  const snapshot = JSON.parse(fs.readFileSync(snapPath, 'utf8'));

  const current = (await (await fetch(`${base}/state`)).json()).agents;

  // 1. Remove leaked scratch agents, highest index first.
  for (let i = current.length - 1; i >= snapshot.length; i--) {
    await command(base, { type: 'delete', index: i });
  }

  // 2. Restore reversible attributes of the originals.
  let activeIdx = 0;
  for (let i = 0; i < snapshot.length; i++) {
    const a = snapshot[i];
    if (a.active) activeIdx = i;
    await command(base, {
      type: 'update', index: i, name: a.name,
      ...(a.probeModel ? { probeModel: a.probeModel } : {}),
      syncInterval: a.syncInterval || 0,
    });
    await command(base, { type: 'setEnabled', index: i, enabled: a.enabled !== false });
  }
  await command(base, { type: 'setActive', index: activeIdx });

  console.log(`[global-teardown] restored ${snapshot.length} agent(s) on ${base}`);
};
