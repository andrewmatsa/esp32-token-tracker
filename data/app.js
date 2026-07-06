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
// as two cards instead of one on the display preview. Claude qualifies via
// the Pro/Max subscription's unified 5h/7d headers, pushed by the PC
// companion daemon (tools/usage-daemon.py, using the Claude Code login
// OAuth token) — but only once used7d/resetEpoch7d actually arrive. A plain
// on-device API-key probe (fetcher.cpp's syncAnthropic()) never populates
// those fields, so Claude falls back to the single "Rate Limit" card below —
// mirrors display.cpp's `has7d` check in renderClaudeUsage().
function hasDualWindow(agent) {
  if (isAnthropic(agent.name)) return (agent.used7d > 0 || agent.resetEpoch7d > 0);
  return isCodex(agent.name) && !agent.model;
}
function hasAutoSync(name) { return isOpenAI(name) || isDeepSeek(name) || isAnthropic(name) || isCursor(name) || isCodex(name); }
// Providers the PC companion daemon (tools/usage-daemon.py) can push usage
// for — their --push slug is just the lowercase preset id.
function daemonSlugFor(name) {
  if (isAnthropic(name)) return 'claude';
  if (isCursor(name))    return 'cursor';
  if (isCodex(name))     return 'codex';
  return null;
}
// The daemon defaults --ip to "token-tracker.local" (the device's mDNS
// name, advertised by main.cpp) and --interval to 120 — so the common case
// needs neither flag spelled out. Both stay overridable on the command line
// (e.g. a plain IP, if the user's OS can't resolve .local names).
const DAEMON_DEFAULT_INTERVAL = 120;
function daemonCommandFor(name, index, intervalValue) {
  const slug = daemonSlugFor(name);
  if (!slug) return '';
  const interval = parseInt(intervalValue, 10);
  const intervalArg = Number.isFinite(interval) && interval > 0 ? interval : DAEMON_DEFAULT_INTERVAL;
  const intervalFlag = intervalArg === DAEMON_DEFAULT_INTERVAL ? '' : ` --interval ${intervalArg}`;
  return `python tools/usage-daemon.py --push ${slug}:${index}${intervalFlag}`;
}

function presetFor(name) {
  const lower = (name || '').toLowerCase();
  return PRESETS.find(p => lower.startsWith(p.id) || lower === p.name.toLowerCase())
      || PRESETS.find(p => p.id === 'custom');
}

// ─── State ────────────────────────────────────────────────────────────────────
let agents = [];
// Index of the card that was just saved — flashed briefly in buildCard()
// as immediate feedback, since the actual WS round-trip is fire-and-forget
// and the user otherwise has no confirmation anything happened.
let justSavedIndex = null;
let justSavedTimer = null;

// ─── HTTP state sync ────────────────────────────────────────────────────────
// No persistent connection — the browser polls GET /state on an interval and
// re-fetches immediately after issuing a command. The Online/Offline badge
// reflects the last poll's success.
function setOnline(online) {
  const el = document.getElementById('ws-status');
  el.textContent = online ? 'Online' : 'Offline';
  el.className   = online ? 'badge online' : 'badge offline';
}

// Monotonic guard: when several refreshes overlap (e.g. two quick commands
// each triggering one), their GETs can resolve out of order — only the most
// recently issued response is allowed to render, so a stale one can't clobber
// a newer state.
let refreshSeq = 0;
async function refresh() {
  const seq = ++refreshSeq;
  try {
    const r = await fetch('/state');
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const data = await r.json();
    if (seq !== refreshSeq) return; // a newer refresh has started; drop this one
    agents = data.agents;
    setOnline(true);
    renderAll();
  } catch {
    if (seq === refreshSeq) setOnline(false);
  }
}

async function postCommand(obj) {
  try {
    // Explicit application/json — a bare string body defaults to text/plain,
    // and AsyncWebServer intercepts x-www-form-urlencoded before the body
    // handler, so being explicit keeps the raw JSON reaching /command.
    await fetch('/command', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(obj),
    });
  } catch {
    setOnline(false);
  }
}

// Issue a command, then reflect the resulting state. Callers that fire a
// single command rely on this; multi-command flows (see saveAgent) await the
// commands themselves and call refresh() once.
function send(obj) {
  postCommand(obj).then(refresh);
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

function renderUsageCard(key, pillLabel, bigValueText, pct, barColor, resetLine, subLine = '') {
  const changed = prevUsageValues[key] !== undefined && prevUsageValues[key] !== bigValueText;
  prevUsageValues[key] = bigValueText;
  return `
    <div class="disp-usage-card${changed ? ' is-updated' : ''}">
      <div class="disp-usage-row1">
        <span class="disp-usage-pct${changed ? ' is-updated' : ''}">${bigValueText}</span>
        <span class="disp-usage-pill">${pillLabel}</span>
      </div>
      <div class="disp-usage-bar-wrap"><div class="disp-usage-bar-fill" style="width:${pct}%;background:${barColor}"></div></div>
      ${subLine ? `<div class="disp-usage-sub">${subLine}</div>` : ''}
      <div class="disp-usage-reset">${resetLine}</div>
    </div>`;
}

// Mirrors display.cpp's renderClaudeUsage() info line: real last-used model
// (left) + estimated cost for today (right). Both come ONLY from the PC
// daemon's JSONL scan (usage-daemon.py's claude_scan_usage_today()) — a
// keyed agent's rate-limit probe target (active.probeModel) is a separate,
// unrelated field and must never appear here, or this line would
// misrepresent which model the user is actually using. Cost is not a real
// Pro/Max charge — Anthropic doesn't expose one for the flat subscription.
function claudeInfoLine(active) {
  if (!isAnthropic(active.name)) return '';
  const hasModel = !!active.model;
  const hasCost  = active.balance != null && active.balance >= 0;
  if (!hasModel && !hasCost) return '';
  return `
    <div class="disp-usage-info">
      <span class="disp-usage-info-model">${hasModel ? active.model : ''}</span>
      <span class="disp-usage-info-cost">${hasCost ? '$' + active.balance.toFixed(2) + ' today' : ''}</span>
    </div>`;
}

// Minimal loading placeholder — just the status text + bouncing dots
// (appended separately below), no "Active" banner and no model/cost info
// line, so a syncing/stale agent doesn't show anything not yet confirmed.
function loadingCards(active) {
  const statusText = hasAutoSync(active.name) ? 'Syncing' : 'No auto-sync';
  return `<div class="disp-syncing">${statusText}</div>`;
}

function renderUsageScreen(active, preset) {
  let cards;
  let isLoading = false;

  if (active.enabled === false) {
    isLoading = true;
    cards = `
      <div style="text-align:center;font-size:16px;color:#666;font-weight:bold;margin-top:14px">Disabled</div>
      <div class="disp-syncing">Auto-sync paused</div>`;
  } else if (hasDualWindow(active)) {
    const claude = isAnthropic(active.name);
    const pct5h = Math.min(100, Math.max(0, active.used || 0));
    const pct7d = Math.min(100, Math.max(0, active.used7d || 0));
    cards = renderUsageCard(`${active.name}:current`, claude ? '5h' : 'Current', pct5h + '%', pct5h, usageBarColor(pct5h), resetLineFor(active.resetEpoch)) +
            renderUsageCard(`${active.name}:weekly`,  claude ? '7d' : 'Weekly',  pct7d + '%', pct7d, usageBarColor(pct7d), resetLineFor(active.resetEpoch7d));
  } else {
    const hasLimit   = active.limit > 0;
    const hasUsed    = active.used > 0;
    const hasBalance = active.balance != null && active.balance >= 0;
    const resetLine  = resetLineFor(active.resetEpoch);
    // A window whose reset time has already passed without a fresh sync
    // landing yet holds stale numbers (e.g. a frozen "0%" from before the
    // window rolled over) — showing them next to "Reset due" is misleading,
    // so treat this exactly like "no data yet" until the next fetch arrives.
    const isStale = active.resetEpoch > 0 && active.resetEpoch <= Math.floor(Date.now() / 1000);

    if (isStale) {
      isLoading = true;
      cards = loadingCards(active);
    } else if (hasLimit) {
      const pct = Math.min(100, Math.round(active.used * 100 / active.limit));
      // Claude's percentage here is a per-minute tier rate limit, not a
      // monthly budget like OpenAI/Cursor/Codex — label it accordingly,
      // matching the physical display's "Rate Limit" pill.
      const label = isAnthropic(active.name) ? 'Rate Limit' : 'Monthly';
      // Raw "used / limit tokens" sub-line + limit-reached note — mirrors
      // display.cpp's hasLimit branch, which shows both the numbers and a
      // "LIMIT REACHED" banner, not just the percentage.
      const subLine = `${formatTokens(active.used)} / ${formatTokens(active.limit)} tokens` +
        (pct >= 100 ? ' — <span style="color:#ef4444;font-weight:600">LIMIT REACHED</span>' : '');
      cards = renderUsageCard(`${active.name}:monthly`, label, pct + '%', pct, usageBarColor(pct), resetLine, subLine);
    } else if (hasUsed && hasBalance) {
      // Both a token count and a balance (e.g. future providers) — mirrors
      // display.cpp's combined "Used: X tokens" + "Balance: $Y" branch.
      cards = renderUsageCard(`${active.name}:tokens`, 'Tokens', formatTokens(active.used), 100, preset.color, resetLine,
        'Balance: $' + active.balance.toFixed(2));
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
      isLoading = true;
      cards = loadingCards(active);
    }
  }

  if (active.enabled !== false && hasAutoSync(active.name)) {
    cards += `<div class="disp-loading-dots" style="color:${preset.color}"><span></span><span></span><span></span></div>`;
  }

  const sprite = isAnthropic(active.name)
    ? claudeSpriteSvg(preset.color)
    : `<span class="disp-usage-sprite" style="color:${preset.color}">${preset.icon}</span>`;

  // Claude's TFT screen (display.cpp renderClaudeUsage()) draws neither the
  // sprite nor the "Usage" title while loading/disabled — this preview must
  // mirror that exactly, not just approximate it. Other providers' TFT
  // screen (display_render()'s generic branch) always keeps its header
  // (agent name + model) regardless of state, so only hide it here for
  // Claude specifically.
  const hideHeader = isLoading && isAnthropic(active.name);
  return `
    <div class="disp-usage-wrap">
      <div class="disp-usage-header">
        ${hideHeader ? '' : sprite}
        ${hideHeader ? '' : `<span class="disp-usage-title">${active.name}</span>`}
      </div>
      <div class="disp-usage-body">
        ${isLoading ? '' : claudeInfoLine(active)}
        ${cards}
      </div>
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

  if (justSavedIndex === i) card.classList.add('just-saved');

  // Header
  card.querySelector('.platform-icon').textContent = preset.icon;
  card.querySelector('.platform-name').textContent = ag.name;

  // Auto-detected model — shown read-only if available (providers with a
  // model picker show an editable input instead, below)
  const modelEl = card.querySelector('.detected-model');
  if (ag.model && !hasModelPicker(ag.name)) { modelEl.textContent = ag.model; modelEl.hidden = false; }

  // API key — provider-specific hints on exactly how to obtain and add one,
  // shown until a key is actually saved (see tools/README.md for the
  // full walkthrough of each option).
  const keyInput = card.querySelector('.inp-apikey');
  if (!ag.hasKey) {
    if (isAnthropic(ag.name)) {
      // Anthropic disabled OAuth session tokens (claude setup-token) for
      // third-party clients ~Feb 2026 — a regular developer API key from
      // console.anthropic.com is the only thing that still authenticates.
      keyInput.placeholder = 'Regular API key from console.anthropic.com (sk-ant-api03-...)';
    } else if (isCursor(ag.name)) {
      // The token lives in Cursor IDE's local session database, not
      // anywhere a user would normally see it — no simple CLI command like
      // Claude's, so point at the daemon as the practical way to get it.
      keyInput.placeholder = "Paste Cursor's access token, or leave empty and run: python tools/usage-daemon.py --push cursor:N";
    } else if (isCodex(ag.name)) {
      keyInput.placeholder = 'No key needed here — run: python tools/usage-daemon.py --push codex:N (reads local Codex CLI login)';
    }
  }
  if (ag.hasKey) keyInput.placeholder = '••••••••  (saved — enter new key to replace)';
  if (isCodex(ag.name)) {
    keyInput.disabled = true;
  }

  if (hasModelPicker(ag.name)) {
    card.querySelector('.claude-config-row').hidden = false;
    const modelInput  = card.querySelector('.inp-model');
    const modelSelect = card.querySelector('.inp-model-select');
    const modelLabel  = card.querySelector('.model-label');
    // "Model" is ambiguous for Claude — this field only picks which model's
    // rate limit to check, not what the user is actually using — so label it
    // explicitly to avoid the confusion a bare "Model" caused previously.
    modelLabel.textContent = isAnthropic(ag.name) ? 'Rate-limit model' : 'Model';

    if (isAnthropic(ag.name) && ag.hasKey) {
      // Keyed Claude agent: `probeModel` is sent straight into the
      // /v1/messages probe body (fetcher.cpp syncAnthropic) to read that
      // model's own rate-limit headers — an invalid/typo'd name makes
      // Anthropic reject the probe outright (no limits at all, silently).
      // A hard <select> of known-valid models prevents that failure mode
      // entirely; free text isn't safe here the way it is for Cursor/Codex's
      // cosmetic bucket filters. This is NOT the same as `model` (the real
      // last-used model, shown elsewhere) — picking a value here only
      // changes which model's rate limit is checked, not what's "in use."
      modelInput.hidden = true;
      modelSelect.hidden = false;
      if (!modelSelect.dataset.populated) {
        const claudeModels = PRESETS.find(p => p.id === 'claude').models;
        modelSelect.innerHTML = '<option value="">Default (claude-haiku-4-5)</option>'
          + claudeModels.map(m => `<option value="${m}">${m}</option>`).join('');
        modelSelect.dataset.populated = '1';
      }
      modelSelect.value = ag.probeModel || '';
    } else {
      modelSelect.hidden = true;
      modelInput.hidden = false;
      modelInput.value = ag.model || '';
      if (isAnthropic(ag.name)) {
        // Keyless Claude agent: the PC daemon (tools/usage-daemon.py) pushes
        // the real last-used model from local Claude Code logs every cycle —
        // manual entry here would just get overwritten, so disable the field
        // instead of leaving a misleading editable box.
        modelInput.disabled = true;
        modelInput.placeholder = 'Auto-detected by daemon (real last-used model)';
      } else {
        modelInput.disabled = false;
        modelInput.placeholder = 'Default / all models';
      }
    }
    const intervalInput = card.querySelector('.inp-interval');
    intervalInput.value = ag.syncInterval || '';

    // Ready-to-paste daemon command for keyless (daemon-driven) agents —
    // updates live as the interval field changes, no save round-trip needed.
    const cmdRow = card.querySelector('.daemon-cmd-row');
    const cmdEl  = card.querySelector('.daemon-cmd');
    if (!ag.hasKey && daemonSlugFor(ag.name)) {
      cmdRow.hidden = false;
      cmdEl.textContent = daemonCommandFor(ag.name, i, intervalInput.value);
      // token-tracker.local needs mDNS support on the machine running the
      // daemon (built in on macOS/Linux; Windows needs Bonjour or its own
      // native mDNS) — surface the real IP as a hover fallback in case it
      // doesn't resolve, without cluttering the copy-paste command itself.
      cmdEl.title = `If "token-tracker.local" doesn't resolve, add: --ip ${location.hostname}`;
      intervalInput.oninput = () => {
        cmdEl.textContent = daemonCommandFor(ag.name, i, intervalInput.value);
      };
      card.querySelector('.btn-copy-cmd').onclick = () => {
        navigator.clipboard.writeText(cmdEl.textContent);
      };
    } else {
      cmdRow.hidden = true;
    }
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
  const saveBtn = card.querySelector('.btn-save');
  if (justSavedIndex === i) saveBtn.textContent = 'Saved ✓';
  saveBtn.onclick = () => saveAgent(i);
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
async function saveAgent(i) {
  const card   = document.querySelector(`.card[data-index="${i}"]`);
  const apiKey = card.querySelector('.inp-apikey').value.trim();

  const name = card.querySelector('.platform-name').textContent.trim();
  const msg = { type: 'update', index: i, name };
  // Only include apiKey if the user typed something new
  if (apiKey) msg.apiKey = apiKey;

  if (hasModelPicker(name)) {
    const modelSelect = card.querySelector('.inp-model-select');
    const interval = parseInt(card.querySelector('.inp-interval').value, 10);
    if (!modelSelect.hidden) {
      // Keyed Claude agent: this is the rate-limit probe target, a
      // separate field from `model` (the real last-used model).
      if (modelSelect.value) msg.probeModel = modelSelect.value;
    } else {
      const model = card.querySelector('.inp-model').value.trim();
      if (model) msg.model = model;
    }
    msg.syncInterval = Number.isFinite(interval) && interval > 0 ? interval : 0;
  }

  // Save, then mark active — saving (new agent or edit) makes it the one
  // shown on the device/dashboard, no separate click needed. Both commands
  // are awaited in order before a single refresh(), so the device has applied
  // them (and appended a brand-new agent to its array) by the time we re-read
  // state — avoiding a race and a double GET.
  await postCommand(msg);
  await postCommand({ type: 'setActive', index: i });

  // Immediate "Saved" feedback — shown for a couple seconds, surviving the
  // refresh() re-render that follows (renderAll() reads justSavedIndex every
  // time it rebuilds the cards).
  justSavedIndex = i;
  if (justSavedTimer) clearTimeout(justSavedTimer);
  justSavedTimer = setTimeout(() => { justSavedIndex = null; renderAll(); }, 2000);
  renderAll();
  refresh();
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
refresh();
// Background poll to pick up device-side changes (background fetchAll every
// 10 min, daemon /push every ~120 s) — the interval self-heals a dropped
// connection, so no separate reconnect logic is needed.
setInterval(refresh, 15000);
loadWifiInfo();
