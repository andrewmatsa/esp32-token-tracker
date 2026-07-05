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
function isOpenAI(name) {
  const n = (name || '').toLowerCase();
  return n.startsWith('gpt') || n.startsWith('openai') || n.startsWith('o1') || n.startsWith('o3');
}
function isDeepSeek(name) { return (name || '').toLowerCase().startsWith('deepseek'); }
// Providers whose usage endpoint reports data per-model, so the user can
// filter the probe/sync to a single model (free text — bucket names are
// account-specific and can't be listed in a fixed dropdown).
function hasModelPicker(name) { return isAnthropic(name) || isCursor(name) || isCodex(name); }
// Providers whose data has two rate-limit windows (short + long), rendered
// as two cards instead of one on the display preview.
function hasDualWindow(agent) { return isAnthropic(agent.name) || (isCodex(agent.name) && !agent.model); }
function hasAutoSync(name) { return isOpenAI(name) || isDeepSeek(name) || isAnthropic(name) || isCursor(name) || isCodex(name); }

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
  updateDisplayPreview();
  startCountdowns();
}

// ─── ESP32 display preview — mirrors display.cpp's adaptive render logic ────
let countdownInterval = null;

function startCountdowns() {
  if (countdownInterval) clearInterval(countdownInterval);
  countdownInterval = setInterval(updateDisplayPreview, 1000);
}

function formatTokens(v) {
  if (v >= 1000000) return (v / 1000000).toFixed(1) + 'M';
  if (v >= 1000)    return (v / 1000).toFixed(1) + 'K';
  return String(v);
}

function formatCountdown(secs) {
  if (secs <= 0) return 'due';
  if (secs >= 86400) return `${Math.floor(secs / 86400)}d ${Math.floor((secs % 86400) / 3600)}h`;
  if (secs >= 3600)  return `${Math.floor(secs / 3600)}h ${String(Math.floor((secs % 3600) / 60)).padStart(2, '0')}m`;
  return `${Math.floor(secs / 60)}m ${String(secs % 60).padStart(2, '0')}s`;
}

function usageBarColor(pct) {
  return pct >= 85 ? '#ef4444' : pct >= 50 ? '#f97316' : '#22c55e';
}

function resetLineFor(resetEpoch) {
  if (!resetEpoch) return '';
  const now = Math.floor(Date.now() / 1000);
  return resetEpoch > now ? 'Resets in ' + formatCountdown(resetEpoch - now) : 'Reset due';
}

// Claude keeps its distinctive pixel-art sprite; every other provider uses
// its own preset.icon character instead.
function claudeSpriteSvg(color) {
  return `
    <svg class="disp-usage-sprite disp-usage-sprite-svg" width="33" height="18" viewBox="0 0 9 5" shape-rendering="crispEdges">
      <rect x="1" y="0" width="7" height="1" fill="${color}"/>
      <rect x="1" y="1" width="1" height="1" fill="${color}"/>
      <rect x="3" y="1" width="3" height="1" fill="${color}"/>
      <rect x="7" y="1" width="1" height="1" fill="${color}"/>
      <rect x="0" y="2" width="9" height="1" fill="${color}"/>
      <rect x="1" y="3" width="7" height="1" fill="${color}"/>
      <rect x="1" y="4" width="1" height="1" fill="${color}"/>
      <rect x="3" y="4" width="1" height="1" fill="${color}"/>
      <rect x="5" y="4" width="1" height="1" fill="${color}"/>
      <rect x="7" y="4" width="1" height="1" fill="${color}"/>
    </svg>`;
}

// Remembers the last rendered value per card slot (`key`) so a changed
// number can be flashed — updateDisplayPreview() re-renders this innerHTML
// wholesale on every tick, so comparing against the DOM isn't an option.
let prevUsageValues = {};

function renderUsageCard(key, pillLabel, bigValueText, pct, barColor, resetLine) {
  const changed = prevUsageValues[key] !== undefined && prevUsageValues[key] !== bigValueText;
  prevUsageValues[key] = bigValueText;
  return `
    <div class="disp-usage-card${changed ? ' is-updated' : ''}">
      <div class="disp-usage-row1">
        <span class="disp-usage-pct${changed ? ' is-updated' : ''}">${bigValueText}</span>
        <span class="disp-usage-pill">${pillLabel}</span>
      </div>
      <div class="disp-usage-bar-wrap"><div class="disp-usage-bar-fill" style="width:${pct}%;background:${barColor}"></div></div>
      <div class="disp-usage-reset">${resetLine}</div>
    </div>`;
}

function renderUsageScreen(active, preset) {
  let cards;

  if (active.enabled === false) {
    cards = `
      <div style="text-align:center;font-size:16px;color:#666;font-weight:bold;margin-top:14px">Disabled</div>
      <div class="disp-syncing">Auto-sync paused</div>`;
  } else if (hasDualWindow(active)) {
    const pct5h = Math.min(100, Math.max(0, active.used || 0));
    const pct7d = Math.min(100, Math.max(0, active.used7d || 0));
    cards = renderUsageCard(`${active.name}:current`, 'Current', pct5h + '%', pct5h, usageBarColor(pct5h), resetLineFor(active.resetEpoch)) +
            renderUsageCard(`${active.name}:weekly`,  'Weekly',  pct7d + '%', pct7d, usageBarColor(pct7d), resetLineFor(active.resetEpoch7d));
  } else {
    const hasLimit   = active.limit > 0;
    const hasUsed    = active.used > 0;
    const hasBalance = active.balance != null && active.balance >= 0;
    const resetLine  = resetLineFor(active.resetEpoch);

    if (hasLimit) {
      const pct = Math.min(100, Math.round(active.used * 100 / active.limit));
      cards = renderUsageCard(`${active.name}:monthly`, 'Monthly', pct + '%', pct, usageBarColor(pct), resetLine);
    } else if (hasUsed) {
      cards = renderUsageCard(`${active.name}:tokens`, 'Tokens', formatTokens(active.used), 100, preset.color, resetLine);
    } else if (hasBalance) {
      cards = renderUsageCard(`${active.name}:balance`, 'Balance', '$' + active.balance.toFixed(2), 100, preset.color, resetLine);
    } else if (active.resetEpoch > 0) {
      // No usage/limit/balance, but a reset time is set — a fetch already
      // completed and legitimately found zero usage (the real device has
      // no separate "synced" flag, so resetEpoch>0 is the proxy for that).
      cards = renderUsageCard(`${active.name}:used`, 'Used', '0', 0, preset.color, resetLine);
    } else {
      const statusText = hasAutoSync(active.name) ? 'Syncing' : 'No auto-sync';
      cards = `
        <div style="text-align:center;font-size:16px;color:#22c55e;font-weight:bold;margin-top:14px">Active</div>
        <div class="disp-syncing">${statusText}</div>`;
    }
  }

  if (active.enabled !== false && hasAutoSync(active.name)) {
    cards += `<div class="disp-loading-dots" style="color:${preset.color}"><span></span><span></span><span></span></div>`;
  }

  const sprite = isAnthropic(active.name)
    ? claudeSpriteSvg(preset.color)
    : `<span class="disp-usage-sprite" style="color:${preset.color}">${preset.icon}</span>`;

  return `
    <div class="disp-usage-wrap">
      <div class="disp-usage-header">
        ${sprite}
        <span class="disp-usage-title">${active.name}</span>
      </div>
      ${cards}
    </div>`;
}

function updateDisplayPreview() {
  const disp   = document.getElementById('esp-display');
  const active = agents.find(a => a.active);

  if (!active) {
    disp.innerHTML = `<div class="disp-idle"><div style="font-size:20px">📟</div><div>No active agent</div></div>`;
    return;
  }

  disp.innerHTML = renderUsageScreen(active, presetFor(active.name));
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
  if (isAnthropic(ag.name) && !ag.hasKey) {
    // The device authenticates as an OAuth session (needed to read the
    // Pro/Max plan's 5h/7d rate-limit headers) — a regular sk-ant-...
    // developer API key is rejected. Run `claude setup-token` once on any
    // machine with the CLI logged in and paste its output here.
    keyInput.placeholder = "OAuth token from 'claude setup-token' — NOT a regular sk-ant-... API key";
  }
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
