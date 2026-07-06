# Web UI + HTTP API tests (Playwright)

End-to-end tests for the token-tracker dashboard and its REST API
(`GET /state`, `POST /command`, `/wifi/info`, static files). The **same specs**
run in two modes:

- **mock** (default) — a Node mock of the ESP32 (`mock-device.js`) is started
  automatically; no hardware needed. Ideal for CI and quick local runs.
- **device** — runs against the real device over the network, with automatic
  snapshot + restore of its agent state.

> This is separate from `../test/`, which is PlatformIO's C unit-test directory.

## Setup

```bash
cd tests
npm install
npm run install-browsers   # one-time: downloads the Chromium Playwright uses
```

## Run — mock mode (no hardware)

```bash
npm test              # runs everything against the auto-started mock
npm run report        # open the HTML report from the last run
```

## Run — real device

Point `TT_DEVICE_URL` at the device. It must be flashed with the current
firmware and reachable on the network.

```bash
# PowerShell
$env:TT_DEVICE_URL="http://token-tracker.local"; npm run test:device

# bash / macOS / Linux
TT_DEVICE_URL=http://token-tracker.local npm run test:device
```

### Safety on real hardware

- CRUD tests (`add`/`delete`) only ever touch a **scratch agent** they create
  themselves at the next free slot — never your existing agents.
- Existing agents are only toggled reversibly (`setActive`/`setEnabled`) and
  restored.
- `global-setup.js` snapshots `GET /state` before the run; `global-teardown.js`
  deletes any leaked scratch agents and restores the originals' `active` /
  `enabled` / `name` / `probeModel` afterward.
- `apiKey` is never exposed by `/state`, so it can't be (and isn't) modified or
  restored — which is exactly why tests never delete a pre-existing keyed agent.

## Files

| File | Purpose |
|---|---|
| `playwright.config.js` | Mode selection (env `TT_DEVICE_URL`), projects, reporters |
| `mock-device.js` | Node HTTP mock of the device (serves `../data` + REST endpoints) |
| `helpers.js` | `getState` / `command` / `addScratchAgent` / `deleteAgent` |
| `api.spec.js` | HTTP-level assertions (no browser) |
| `ui.spec.js` | Browser assertions (rendering, CRUD, toggles, offline, display logic) |
| `global-setup.js` / `global-teardown.js` | Device-mode snapshot + restore |
