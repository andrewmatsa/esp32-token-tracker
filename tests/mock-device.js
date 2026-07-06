// Node mock of the ESP32 token-tracker HTTP device: serves ../data statically
// and implements the same REST endpoints as src/webserver.cpp, so the exact
// same Playwright specs run against it without hardware.
//
// Command semantics mirror webserver.cpp's handleCommand() and main.cpp's
// callbacks. Run standalone: `node mock-device.js --port 8791`.
const http = require('http');
const fs = require('fs');
const path = require('path');

const DATA_DIR = path.join(__dirname, '..', 'data');
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };

// Deterministic seed: one keyed, active, enabled Claude agent — enough for the
// smoke/API assertions to have real data.
function seed() {
  return [{
    name: 'Claude', model: '', probeModel: '', hasKey: true,
    used: 0, limit: 100, resetEpoch: Math.floor(Date.now() / 1000) + 3600,
    balance: -1, active: true, enabled: true, syncInterval: 0,
    used7d: 0, resetEpoch7d: 0,
  }];
}

let agents = seed();

function readBody(req) {
  return new Promise((resolve) => {
    let b = '';
    req.on('data', (c) => (b += c));
    req.on('end', () => resolve(b));
  });
}

function applyCommand(cmd) {
  switch (cmd.type) {
    case 'update': {
      const i = cmd.index;
      if (i < 0 || i > agents.length) return;
      if (i === agents.length) {
        agents.push({
          name: cmd.name || '', model: '', probeModel: '', hasKey: false,
          used: 0, limit: 0, resetEpoch: 0, balance: -1, active: false,
          enabled: true, syncInterval: 0, used7d: 0, resetEpoch7d: 0,
        });
      }
      const a = agents[i];
      if (cmd.name) a.name = cmd.name;
      if (cmd.apiKey) a.hasKey = true;
      // Claude: probeModel is the keyed rate-limit target; model stays daemon-owned.
      if (typeof cmd.probeModel === 'string' && cmd.probeModel) a.probeModel = cmd.probeModel;
      if (typeof cmd.model === 'string' && cmd.model) a.model = cmd.model;
      if (Number.isFinite(cmd.syncInterval)) a.syncInterval = cmd.syncInterval;
      break;
    }
    case 'setActive':
      agents.forEach((a, k) => (a.active = k === cmd.index));
      break;
    case 'setEnabled':
      if (agents[cmd.index]) agents[cmd.index].enabled = !!cmd.enabled;
      break;
    case 'delete':
      if (cmd.index >= 0 && cmd.index < agents.length) agents.splice(cmd.index, 1);
      break;
  }
}

const server = http.createServer(async (req, res) => {
  const url = req.url.split('?')[0];

  if (url === '/state' && req.method === 'GET') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end(JSON.stringify({ type: 'state', agents }));
  }

  if (url === '/command' && req.method === 'POST') {
    let cmd;
    try { cmd = JSON.parse(await readBody(req) || '{}'); }
    catch { res.writeHead(400, { 'Content-Type': 'application/json' }); return res.end('{"ok":false,"error":"bad json"}'); }
    applyCommand(cmd);
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end('{"ok":true}');
  }

  if (url === '/wifi/info' && req.method === 'GET') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end(JSON.stringify({ ssid: 'MockNet', ip: '192.168.0.198' }));
  }

  // Mock-only control: reseed to a known baseline (beforeEach uses this).
  if (url === '/__reset' && req.method === 'POST') {
    agents = seed();
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end('{"ok":true}');
  }
  // Mock-only control: force a usage value, to test the browser's poll pickup.
  if (url === '/__setUsed' && req.method === 'POST') {
    const body = JSON.parse(await readBody(req) || '{}');
    if (agents[body.index]) { agents[body.index].used = body.used; agents[body.index].limit = body.limit ?? 100; }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    return res.end('{"ok":true}');
  }

  // Static files from ../data
  const file = url === '/' ? '/index.html' : url;
  fs.readFile(path.join(DATA_DIR, file), (err, buf) => {
    if (err) { res.writeHead(404); return res.end('not found'); }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
    res.end(buf);
  });
});

if (require.main === module) {
  const portArg = process.argv.indexOf('--port');
  const port = portArg !== -1 ? Number(process.argv[portArg + 1]) : 8791;
  server.listen(port, () => console.log(`mock-device listening on http://localhost:${port}`));
}

module.exports = server;
