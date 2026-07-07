# Historical: migrating browser ↔ ESP32 from WebSocket to plain HTTP

> **Status: completed** (see commit `4a9a33c`, "Replace WebSocket with plain
> HTTP"). `webserver.cpp`/`data/app.js` now use `GET /state` polling +
> `POST /command`, as described below and in `CLAUDE.md`'s HTTP Protocol
> section. This document is kept as the original rationale/ADR for that
> change, not as an active plan — nothing here describes future work.

## Context / why

WebSocket is architecturally unnecessary here:

- **Browser → device** traffic is human-paced CRUD (`update` / `setActive` /
  `setEnabled` / `delete` — one per button click).
- **Device → browser** is a `state` refresh every ~120 s at most (background
  `fetchAll()` is every 10 min; the daemon pushes every ~120 s).
- The highest-frequency data path — the daemon's `/push` — is **already**
  plain HTTP POST, proving HTTP suffices for the push side.

On an ESP32-C3 (320 KB RAM, AsyncTCP) a persistent per-tab socket is the
heaviest option for the least benefit, and `ws.cleanupClients()` in the loop
is a zombie-connection band-aid. **Goal:** same behaviour, simpler transport,
less RAM, no persistent-socket lifecycle to manage.

## Target architecture

**Commands (browser → device): REST.** One endpoint that accepts the exact
same `{type, ...}` payloads the WS handler already parses, so the dispatch
logic is reused verbatim:

- `POST /command` — body `{type:"update"|"setActive"|"setEnabled"|"delete", ...}`

**State (device → browser): short polling.** No persistent connection:

- `GET /state` — returns what `buildStateJson()` already produces
- Browser polls every ~15 s, and refreshes immediately after issuing a command

**Unchanged:** `/push` (daemon), `/wifi/info`, `/wifi/reset`, static SPIFFS
file serving — all already plain HTTP.

> **Alternative for the push side** (documented, not the default): **SSE** via
> `AsyncEventSource` (`/events`, `events.send(json,"state")` + browser
> `new EventSource('/events')`). Keeps instant cross-tab sync at the cost of
> one persistent connection per tab. Prefer plain polling unless instant
> multi-tab sync becomes a real requirement — polling is the lighter, truly
> stateless option and this is a single-user device.

## Firmware changes — `src/webserver.cpp` / `src/webserver.h`

1. **Lift the WS command dispatch into a shared helper.** The
   `if (strcmp(msgType, ...))` block currently inside `onWsEvent` moves
   verbatim into `static void handleCommand(JsonDocument& doc)`. No logic
   change — it already reads `doc["type"]`, `doc["index"]`, etc. and calls the
   existing `_cbUpdate` / `_cbActive` / `_cbEnabled` / `_cbDelete` callbacks.
2. **Add `POST /command`** — a body handler following the exact pattern of the
   existing `/push` handler (buffer the body, wait for `index + len == total`,
   `deserializeJson`, then `handleCommand(doc)`), replying `{"ok":true}`.
3. **Add `GET /state`** —
   `req->send(200, "application/json", buildStateJson(_agents, *_count))`.
   `buildStateJson()` is reused as-is; its `"type":"state"` field is harmless
   over HTTP and lets the browser keep a single parsing path.
4. **Remove the WebSocket:** delete `AsyncWebSocket ws("/ws")`, `onWsEvent`,
   `ws.onEvent(...)`, `server.addHandler(&ws)`, and the `ws.cleanupClients()`
   call in `webserver_loop()` (that loop function can then be removed
   entirely, along with its call in `main.cpp`'s `loop()`).
5. **`webserver_broadcastState()` becomes obsolete** (polling replaces push).
   Prefer deleting it and removing its call sites in `main.cpp` (`onUpdate`,
   `onSetActive`, `onSetEnabled`, `onDelete`, `fetchAll`) for a clean removal;
   the low-churn alternative is keeping it as an empty stub.

## Web changes — `data/app.js`

Replace the WebSocket block with fetch-based equivalents, **keeping
`send(obj)`'s call sites unchanged** so `saveAgent` / `setActive` /
`setEnabled` / `deleteAgent` need no edits beyond the two-command case:

- `async function refresh()` → `fetch('/state')` →
  `agents = (await r.json()).agents; renderAll();`. Wrap in try/catch to drive
  the existing Online/Offline badge (success = online, throw = offline).
- `async function postCommand(obj)` →
  `fetch('/command', { method:'POST', body: JSON.stringify(obj) })`.
- `function send(obj)` → `postCommand(obj).then(refresh)` — preserves the
  "issue command, then reflect new state" behaviour single-command callers
  rely on.
- **`saveAgent` sequencing:** it currently fires two commands (`update` then
  `setActive`). Change to
  `await postCommand(updateMsg); await postCommand({type:'setActive', index:i}); refresh();`
  so both land before a single refresh (avoids a double GET and a race).
- **Startup + background poll:** replace `connect()` with an initial
  `refresh()` plus `setInterval(refresh, 15000)`. Drop the reconnect
  `setTimeout` — the interval self-heals and the badge reflects the last
  poll's success.

## Verification

- Build + flash: `pio run --target upload` + `uploadfs`.
- `curl http://token-tracker.local/state` → returns the agents JSON.
- `curl -X POST http://token-tracker.local/command -d '{"type":"setActive","index":0}'`
  → device switches active agent (watch the TFT + a follow-up `GET /state`).
- Playwright against `data/index.html`: add agent → save (confirm
  update + setActive both applied and the card goes `is-active`) → toggle
  Enabled → delete → all reflected after the immediate refresh.
- Kill the device / block the host → badge flips to Offline within one poll
  interval; restore → flips back. No console errors, no reconnect storm.
- Confirm `/push` (`usage-daemon.py --push claude:0 --once`) and `/wifi/*`
  still work untouched.
- Sanity: free heap equal-or-better with no persistent WS clients;
  `ws.cleanupClients()` is gone.

## Risk / rollback

Pure transport swap — command dispatch and state serialization are reused
verbatim, so behaviour should be identical. Keep it a single isolated commit
so it can be reverted cleanly if any regression appears. Main risk areas: the
`saveAgent` two-command sequencing, and the Online/Offline badge now being
poll-driven rather than connection-driven.

---

## Appendix — ADR: why this was originally deferred (superseded — see status note at top)

At the time this was written, WebSocket was unnecessary but **worked and was
tested**; the AsyncWebServer dependency stayed regardless (static files,
`/push`, `/wifi/*`); the device is single-user / one-tab where the RAM
concern is negligible; and migrating was pure refactoring risk with zero
user-facing benefit at the time. The migration was later picked up anyway
(commit `4a9a33c`) and is complete — this section is kept only as the
original reasoning for why it wasn't done immediately.
