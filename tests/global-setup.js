// @ts-check
// Device mode only: snapshot the real device's agent state before the run so
// global-teardown can restore it. (apiKey is never exposed by /state, so keyed
// agents are only ever snapshotted as hasKey — which is why CRUD tests operate
// on scratch agents they create+delete themselves, never on originals.)
const fs = require('fs');
const path = require('path');

module.exports = async () => {
  const base = process.env.TT_DEVICE_URL;
  if (!base) return;
  const r = await fetch(`${base}/state`);
  const { agents } = await r.json();
  const dir = path.join(__dirname, 'test-results');
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(path.join(dir, '_snapshot.json'), JSON.stringify(agents, null, 2));
  console.log(`[global-setup] snapshotted ${agents.length} agent(s) from ${base}`);
};
