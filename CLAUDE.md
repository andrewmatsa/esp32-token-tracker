# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash Commands

```bash
# Build firmware
pio run

# Build + upload to ESP32
pio run --target upload

# Upload web files (SPIFFS filesystem)
pio run --target uploadfs

# Build + upload + open serial monitor
pio run --target upload && pio device monitor

# Serial monitor only (115200 baud)
pio device monitor
```

## Project Overview

ESP32-C3 Super Mini device with a 1.54" ST7789 240×240 TFT display that tracks AI token/credit usage across multiple providers. WiFi is configured via a captive portal; users add agents and API keys through a browser UI; the ESP32 polls provider APIs autonomously every 10 minutes.

## Architecture

### Firmware (`src/`)

| File | Responsibility |
|---|---|
| `main.cpp` | Boot sequence, `fetchAll()` orchestration, loop timers. After WiFi connects, starts an mDNS responder (`MDNS.begin("token-tracker")`) so the device is reachable at `token-tracker.local` regardless of DHCP-assigned IP — this is what `tools/usage-daemon.py --ip` defaults to |
| `storage.h/.cpp` | NVS (Preferences) persistence for `Agent` structs. Namespace: `"ttracker"`, keys like `ag0name`, `ag0key`, `ag0bal` |
| `fetcher.h/.cpp` | HTTPS polling per provider (OpenAI sums daily usage; DeepSeek fetches balance). Sets `agent.model` from `snapshot_id`. Uses `WiFiClientSecure::setInsecure()` |
| `display.h/.cpp` | TFT rendering via TFT_eSPI. `display_render()` is adaptive — shows only what data is available. `display_tick()` called every 500ms for pulse dot + countdown only (no full redraw) |
| `webserver.h/.cpp` | AsyncWebServer port 80. REST: `GET /state` (browser polls this) + `POST /command`; also `/push` (daemon), `/wifi/*`. Sends `hasKey` bool instead of actual API key. SPIFFS serves `data/` as static files. No WebSocket — see HTTP Protocol section |
| `wifi_manager.h/.cpp` | STA connect + captive portal (SoftAP + DNSServer + blocking setup server). On success: `ESP.restart()` |
| `include/config.h` | Pin defines, thresholds, `FETCH_INTERVAL_MS = 600000` |

### Agent struct (storage.h)
```cpp
struct Agent {
    char     name[32];       // "Claude", "GPT / OpenAI"
    char     model[48];      // Codex/Cursor: their usage-bucket filter. Claude: real
                             // last-used model — written ONLY by the PC daemon's /push
                             // (JSONL scan); empty if the daemon has never reported one.
    char     probeModel[48]; // Claude only: user-chosen rate-limit probe target (which
                             // model's limit to check) — unrelated to `model`.
    char     apiKey[512]; // never returned to browser. 512, not a smaller size — Claude
                          // Code OAuth tokens (`claude setup-token`) are long JWT-style
                          // strings, routinely 300-1000+ chars.
    uint32_t used;        // tokens (0 = unknown), or Claude's 5h-window %
    uint32_t limit;       // 0 = unknown, or 100 for Claude's 5h-window %
    uint32_t resetEpoch;  // auto-computed (1st of next month for OpenAI)
    float    balance;     // USD credit (-1 = not applicable)
    bool     active;      // only one agent is active at a time
    bool     enabled;     // false = excluded from auto-sync (fetchAll skips it)
    uint32_t used7d;         // 7-day rate-limit window %, 0-100 — written generically by
                             // the PC daemon's /push (Claude's Pro/Max subscription and
                             // Codex's secondary rate-limit window both use this field)
    uint32_t resetEpoch7d;   // unix timestamp of the 7-day window reset
    uint32_t syncIntervalSec; // seconds between on-device probes (0 = use the global
                              // FETCH_INTERVAL_MS default)
    uint32_t lastSyncEpoch;   // unix timestamp of last successful sync (0 = never synced).
                              // The real "has this agent ever synced" signal — once
                              // nonzero, a lapsed resetEpoch just shows last-known data
                              // instead of blanking back to "Syncing".
    uint32_t nextSyncEpoch;  // unix timestamp of the next on-device fetchAll() attempt
                             // (0 = not scheduled — keyless/daemon-driven agents never
                             // get an on-device probe). EPHEMERAL: recomputed every
                             // fetchAll() cycle, never persisted to NVS.
};
```

### HTTP Protocol (browser ↔ ESP32)
Plain REST + polling — no WebSocket. The browser polls `GET /state` every 15 s
(and re-fetches right after issuing a command); commands are `POST /command`.
See `webserver.cpp` and `docs/http-migration-plan.md` for the rationale (the
old WS transport was replaced because the workload is human-paced commands +
multi-minute state refresh, for which a persistent per-tab socket is the
heaviest option for the least benefit on an ESP32-C3).
- **`GET /state`** → `{ type: "state", agents: [...] }` — includes `hasKey` bool, never `apiKey`; includes both `model` and `probeModel` for Claude agents (see below). (`type` field is a harmless carry-over from the old WS payload — the browser keeps one parsing path.)
- **`POST /command`** body `{ type: "update", index, name, apiKey?, model?, probeModel? }` — `apiKey` only when user types a new one; omitting `apiKey` preserves existing key. For OpenAI, `model` is auto-detected-and-overwritten-every-cycle by the on-device fetcher (`fetcher.cpp`'s dominant `snapshot_id`), so any value the browser sends there is transient. For Codex (no on-device fetcher — see Fetch Logic below) and Cursor, `model` is a user-set free-text bucket filter the browser's value actually sticks, either typed directly or via the daemon's `--push codex:N:model`/`cursor:N:model`. `probeModel` is Claude-specific (see below); the browser never sends `model` for a Claude agent, and the server ignores it if it did (only `webserver.cpp`'s `/push` handler, i.e. the PC daemon, may set a Claude agent's `model`).
- **`POST /command`** body `{ type: "setActive", index }` | `{ type: "setEnabled", index, enabled }` | `{ type: "delete", index }`.
- **Claude's `model` and `probeModel` are two independent fields, not one overloaded field** — this was a real bug (see git history) where a single field was used both as "which model to check the rate limit for" and "which model did the user really just use," and showing the former under an unqualified "current model" label was actively misleading. Now: `probeModel` is the user's manual rate-limit target, set via the web UI's closed `<select>` (`fetcher.cpp`'s `syncAnthropic()` sends it in the `/v1/messages` probe body — Anthropic's rate limits are model-specific, and an invalid name would make Anthropic reject the probe outright, hence the closed dropdown instead of free text). `model` is exclusively the real last-used model, written only by `tools/usage-daemon.py`'s `/push` (JSONL transcript scan) — it is never set by the web UI or the on-device probe, and is empty until the daemon has run at least once. The two fields can't collide since they're never written by the same path.

### Fetch Logic
- On boot: `doImmediateFetch = true` → `fetchAll()` runs at first `loop()` iteration
- After saving an agent with a new API key: `doImmediateFetch = true`
- Per-agent interval: `agent.syncIntervalSec` if set (user-configurable via the web UI), else the global `FETCH_INTERVAL_MS` (10 min) default
- Provider dispatch in `fetcher.cpp` by name prefix: OpenAI/GPT/O1/O3 → `syncOpenAI()`; DeepSeek → `syncDeepSeek()`; Claude/Anthropic → `syncAnthropic()`; Cursor → `syncCursor()`. **Codex has no on-device fetcher at all** — `isOpenAI()` explicitly excludes it (the legacy `/v1/usage` endpoint `syncOpenAI()` calls doesn't cover Codex usage), so a Codex agent is always daemon/bridge-only, and its `apiKey` field is left disabled entirely in the web UI.
- Agents with an empty API key are skipped by the fetcher and instead get usage pushed by the PC companion (`tools/usage-daemon.py`) via `POST /push`. The daemon's `claude` provider reads the Claude Code login OAuth token from `~/.claude/.credentials.json` and reports the real Pro/Max 5h + 7d subscription windows (unified rate-limit headers) — `syncAnthropic()`'s on-device probe with a plain API key only yields the one per-minute tier window. The daemon's `codex` provider similarly reports a genuine dual-window shape (`primary_window`/`secondary_window` → `used`/`resetEpoch` and `used7d`/`resetEpoch7d`) — these two 7-day fields are written generically by `webserver.cpp`'s `/push` handler and `main.cpp`'s `onExternalPush()`, with no provider gate, so Claude and Codex share the exact same dual-window storage/render path.
- `fetcher_sync()` stamps `agent.lastSyncEpoch = now` on every successful on-device sync; `onExternalPush()` does the same for daemon pushes. `main.cpp`'s `fetchAll()` also computes `agent.nextSyncEpoch = now + interval` right before each on-device probe attempt (ephemeral scheduler state, not persisted) — this drives the "Sync in" countdown described below.

### Display Render Logic (display.cpp)
Two rendering paths, chosen by `classifyProvider(agent->name)`:
- **Card-based screen** (`renderCardUsage()`/`tickCardUsage()`) for Claude, OpenAI, Cursor, DeepSeek, and Codex — a rounded card per rate-limit window (pct or token count or balance, pill-badge label, progress bar, reset/sync line), each provider with its own pixel-art sprite and accent color (mirrors `data/app.js`'s `renderUsageCard()`/`PRESETS`). Two cards when `used7d > 0 || resetEpoch7d > 0` (Claude's Pro/Max subscription or Codex's secondary window); otherwise one card whose shape follows whichever of `limit > 0` / `used > 0` / `balance >= 0` is populated. `agent.lastSyncEpoch == 0` ("never synced") shows "Syncing" + bouncing dots instead, regardless of provider — the real trigger for that state, not a lapsed `resetEpoch` (once an agent has ever synced, an expired reset window just keeps showing its last-known data).
- **Plain generic screen** (the rest of `display_render()`'s body) for any other/unrecognized provider name: header bar (name + model) + adaptive single-line content —
  1. `limit > 0` → progress bar + "X / Y tokens"
  2. `used > 0` only → "Used: X tokens" (no bar)
  3. `balance >= 0` only → "Balance: $X.XX"
  4. Never synced → "Syncing" + bouncing dots
- Both paths show a "Resets in…" / "Sync in…" split line where applicable — "Sync in" only appears on the one window an on-device probe with a real API key actually refreshes (0/blank for keyless, daemon-driven agents, and for Claude's plain-API-key "Rate Limit" fallback card, whose per-minute window is too short-lived to show a reset line at all).

### Web Files (`data/`)
Served from SPIFFS. `index.html` + `app.js` + `style.css` — production UI. `wifi-setup.html` — captive portal page. `temporary.html` (project root, not in SPIFFS) — standalone browser test with `MockWebSocket` and simulated fetch results; uses `localStorage` key `tt_agents_v2`.

### TFT_eSPI Configuration
Display driver is configured entirely via `build_flags` in `platformio.ini` (no `User_Setup.h` file needed). `USER_SETUP_LOADED=1` prevents the library from using its own setup. Pin mapping: SCLK=4, MOSI=6, CS=7, DC=2, RST=3, BL=8.

## Key Constraints

- NVS keys max 15 chars: `ag0name`, `ag0model`, `ag0pmodel`, `ag0key`, `ag0used`, `ag0limit`, `ag0reset`, `ag0bal`, `ag0active`, `ag0en`, `ag0u7d`, `ag0r7d`, `ag0ivl`, `ag0sync` (see `storage.cpp`'s `makeKey()`). `nextSyncEpoch` has no NVS key — it's ephemeral scheduler state, recomputed every `fetchAll()` cycle, never persisted.
- Max 6 agents (`MAX_AGENTS`)
- All display text and code comments must be in English
- API keys are stored only in NVS; never appear in `GET /state` responses or on TFT display
- `display_tick()` does partial redraws only (pulse dot + countdown) to avoid flicker — `display_render()` does full redraws on state changes

## Display Synchronization

This project has two user interfaces:

1. ESP32 TFT display
2. Web dashboard

Whenever any UI state, metric, layout, progress bar, animation, text,
or status changes, BOTH displays MUST be updated.

Never implement a feature only on the TFT display.

Never implement a feature only on the web dashboard.

Before considering a task complete, verify that both interfaces remain synchronized.
