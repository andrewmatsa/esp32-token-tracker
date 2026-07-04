'use strict';

// ─── Platform presets ─────────────────────────────────────────────────────────
const PRESETS = [
  {
    id: 'claude', name: 'Claude', icon: '◆', color: '#D97706',
    models: ['claude-opus-4-8', 'claude-sonnet-4-6', 'claude-haiku-4-5-20251001'],
  },
  {
    id: 'cursor', name: 'Cursor', icon: '✦', color: '#3B82F6',
    models: ['Fast requests', 'Premium requests'],
  },
  {
    id: 'codex', name: 'Codex', icon: '⚡', color: '#10B981',
    models: ['codex-mini', 'o3-mini', 'gpt-4o'],
  },
  {
    id: 'openai', name: 'GPT / OpenAI', icon: '◎', color: '#8B5CF6',
    models: ['gpt-4o', 'gpt-4o-mini', 'o1', 'o1-mini', 'o3', 'o3-mini'],
  },
  {
    id: 'gemini', name: 'Gemini', icon: '✧', color: '#EF4444',
    models: ['gemini-2.5-pro', 'gemini-2.0-flash', 'gemini-1.5-pro'],
  },
  {
    id: 'copilot', name: 'Copilot', icon: '◉', color: '#6366F1',
    models: ['Premium completions'],
  },
  {
    id: 'deepseek', name: 'DeepSeek', icon: '◈', color: '#14B8A6',
    models: ['deepseek-v3', 'deepseek-r1'],
  },
  {
    id: 'perplexity', name: 'Perplexity', icon: '◇', color: '#F59E0B',
    models: ['Pro queries'],
  },
  {
    id: 'custom', name: 'Custom', icon: '○', color: '#6B7280',
    models: [],
  },
];

function isAnthropic(name) {
  const n = (name || '').toLowerCase();
  return n.startsWith('claude') || n.startsWith('anthropic');
}
function isCursor(name) { return (name || '').toLowerCase().startsWith('cursor'); }
function isCodex(name) { return (name || '').toLowerCase().startsWith('codex'); }
// Providers whose usage endpoint reports data per-model, so the user can
// filter the probe/sync to a single model (free text — bucket names are
// account-specific and can't be listed in a fixed dropdown).
function hasModelPicker(name) { return isAnthropic(name) || isCursor(name) || isCodex(name); }

function presetFor(name) {
  const lower = (name || '').toLowerCase();
  return PRESETS.find(p => lower.startsWith(p.id) || lower === p.name.toLowerCase())
      || PRESETS.find(p => p.id === 'custom');
}

// ─── State ────────────────────────────────────────────────────────────────────
let agents = [];
let ws     = null;

// ─── WebSocket ────────────────────────────────────────────────────────────────
function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);

  ws.onopen = () => {
    document.getElementById('ws-status').textContent = 'Online';
    document.getElementById('ws-status').className   = 'badge online';
  };
  ws.onclose = () => {
    document.getElementById('ws-status').textContent = 'Offline';
    document.getElementById('ws-status').className   = 'badge offline';
    setTimeout(connect, 3000);
  };
  ws.onmessage = (e) => {
    const msg = JSON.parse(e.data);
    if (msg.type === 'state') { agents = msg.agents; renderAll(); }
  };
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN)
    ws.send(JSON.stringify(obj));
}

// ─── Preset modal ─────────────────────────────────────────────────────────────
function openPresetModal() {
  if (agents.length >= 6) { alert('Maximum 6 agents reached.'); return; }
  document.getElementById('preset-modal').hidden = false;
}
function closePresetModal() {
  document.getElementById('preset-modal').hidden = true;
}

function buildPresetGrid() {
  const grid = document.getElementById('preset-grid');
  PRESETS.forEach(p => {
    const btn = document.createElement('button');
    btn.className = 'preset-btn';
    btn.style.setProperty('--pc', p.color);
    btn.innerHTML = `<span class="preset-icon">${p.icon}</span><span>${p.name}</span>`;
    btn.onclick = () => { closePresetModal(); selectPreset(p.id); };
    grid.appendChild(btn);
  });
}

function selectPreset(presetId) {
  const preset = PRESETS.find(p => p.id === presetId) || PRESETS.at(-1);
  const i = agents.length;
  agents.push({
    name:       preset.name,
    model:      '',
    hasKey:     false,
    used:       0,
    limit:      0,
    resetEpoch: 0,
    balance:    -1,
    active:     false,
  });
  const card = buildCard(agents[i], i);
  document.getElementById('agent-list').appendChild(card);
}

// ─── Render ───────────────────────────────────────────────────────────────────
function renderAll() {
  const list = document.getElementById('agent-list');
  list.innerHTML = '';
  agents.forEach((ag, i) => list.appendChild(buildCard(ag, i)));
}

function buildCard(ag, i) {
  const tpl    = document.getElementById('tpl-card');
  const card   = tpl.content.cloneNode(true).querySelector('.card');
  const preset = presetFor(ag.name);

  card.dataset.index    = i;
  card.dataset.platform = preset.id;
  card.style.setProperty('--pc', preset.color);

  // Header
  card.querySelector('.platform-icon').textContent = preset.icon;
  card.querySelector('.platform-name').textContent = ag.name;

  // Auto-detected model — shown read-only if available (providers with a
  // model picker show an editable input instead, below)
  const modelEl = card.querySelector('.detected-model');
  if (ag.model && !hasModelPicker(ag.name)) { modelEl.textContent = ag.model; modelEl.hidden = false; }

  // API key — show placeholder dots if key already stored on device
  const keyInput = card.querySelector('.inp-apikey');
  if (ag.hasKey) keyInput.placeholder = '••••••••  (saved — enter new key to replace)';
  if (isCodex(ag.name)) {
    keyInput.placeholder = 'No key needed — reads local Codex CLI login (see tools/usage-daemon.py)';
    keyInput.disabled = true;
  }

  if (hasModelPicker(ag.name)) {
    card.querySelector('.claude-config-row').hidden = false;
    const modelInput = card.querySelector('.inp-model');
    modelInput.value = ag.model || '';
    modelInput.placeholder = isAnthropic(ag.name) ? 'Default (claude-haiku-4-5)' : 'Default / all models';
    card.querySelector('.inp-interval').value = ag.syncInterval || '';
  }

  // State classes — usage stats themselves are shown on the device's own
  // display, not duplicated here
  const pct = (ag.limit > 0) ? Math.min(100, Math.round(ag.used * 100 / ag.limit)) : 0;
  if (ag.active) card.classList.add('is-active');
  if (pct >= 85)  card.classList.add('is-warning');
  if (pct >= 100) card.classList.add('is-maxed');

  const enabledInput = card.querySelector('.inp-enabled');
  enabledInput.checked = ag.enabled !== false; // default to enabled for agents saved before this field existed
  if (!enabledInput.checked) card.classList.add('is-disabled');
  enabledInput.onchange = () => setEnabled(i, enabledInput.checked);

  // Buttons
  card.querySelector('.btn-save').onclick   = () => saveAgent(i);
  card.querySelector('.btn-delete').onclick = () => deleteAgent(i);

  // Clicking the card itself (outside inputs/buttons) makes it the active
  // agent — no dedicated "Set Active" button needed.
  card.onclick = (e) => {
    if (e.target.closest('button, input, select, label')) return;
    setActive(i);
  };

  return card;
}

// ─── Actions ──────────────────────────────────────────────────────────────────
function saveAgent(i) {
  const card   = document.querySelector(`.card[data-index="${i}"]`);
  const apiKey = card.querySelector('.inp-apikey').value.trim();

  const name = card.querySelector('.platform-name').textContent.trim();
  const msg = { type: 'update', index: i, name };
  // Only include apiKey if the user typed something new
  if (apiKey) msg.apiKey = apiKey;

  if (hasModelPicker(name)) {
    const model = card.querySelector('.inp-model').value.trim();
    const interval = parseInt(card.querySelector('.inp-interval').value, 10);
    if (model) msg.model = model;
    msg.syncInterval = Number.isFinite(interval) && interval > 0 ? interval : 0;
  }

  send(msg);
}

function setActive(i)   { send({ type: 'setActive', index: i }); }
function setEnabled(i, enabled) { send({ type: 'setEnabled', index: i, enabled }); }

function deleteAgent(i) {
  if (!confirm(`Delete "${agents[i]?.name}"?`)) return;
  send({ type: 'delete', index: i });
}

// ─── WiFi helpers ─────────────────────────────────────────────────────────────
async function loadWifiInfo() {
  try {
    const res  = await fetch('/wifi/info');
    const data = await res.json();
    document.getElementById('wifi-info').textContent =
      `WiFi: ${data.ssid || '—'}  |  ${data.ip}`;
  } catch { /* ignore */ }
}

async function resetWifi() {
  if (!confirm('This will clear WiFi credentials and restart the device into setup mode.')) return;
  try { await fetch('/wifi/reset', { method: 'POST' }); } catch { /* restarting */ }
  alert('Device is restarting into WiFi setup mode.');
}

// ─── Init ─────────────────────────────────────────────────────────────────────
buildPresetGrid();
connect();
loadWifiInfo();
