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
| `main.cpp` | Boot sequence, `fetchAll()` orchestration, loop timers |
| `storage.h/.cpp` | NVS (Preferences) persistence for `Agent` structs. Namespace: `"ttracker"`, keys like `ag0name`, `ag0key`, `ag0bal` |
| `fetcher.h/.cpp` | HTTPS polling per provider (OpenAI sums daily usage; DeepSeek fetches balance). Sets `agent.model` from `snapshot_id`. Uses `WiFiClientSecure::setInsecure()` |
| `display.h/.cpp` | TFT rendering via TFT_eSPI. `display_render()` is adaptive — shows only what data is available. `display_tick()` called every 500ms for pulse dot + countdown only (no full redraw) |
| `webserver.h/.cpp` | AsyncWebServer port 80 + AsyncWebSocket `/ws`. Broadcasts `hasKey` bool instead of actual API key. SPIFFS serves `data/` as static files |
| `wifi_manager.h/.cpp` | STA connect + captive portal (SoftAP + DNSServer + blocking setup server). On success: `ESP.restart()` |
| `include/config.h` | Pin defines, thresholds, `FETCH_INTERVAL_MS = 600000` |

### Agent struct (storage.h)
```cpp
struct Agent {
    char     name[32];    // "Claude", "GPT / OpenAI"
    char     model[48];   // auto-detected from API, e.g. "gpt-4o"
    char     apiKey[128]; // never returned to browser
    uint32_t used;        // tokens (0 = unknown)
    uint32_t limit;       // 0 = unknown
    uint32_t resetEpoch;  // auto-computed (1st of next month for OpenAI)
    float    balance;     // USD credit (-1 = not applicable)
    bool     active;
};
```

### WebSocket Protocol (browser ↔ ESP32)
- **ESP32 → browser**: `{ type: "state", agents: [...] }` — includes `hasKey` bool, never `apiKey`
- **browser → ESP32**: `{ type: "update", index, name, apiKey? }` — `apiKey` only when user types a new one; omitting `apiKey` preserves existing key; `model` not sent (auto-detected)
- **browser → ESP32**: `{ type: "setActive", index }` | `{ type: "delete", index }`

### Fetch Logic
- On boot: `doImmediateFetch = true` → `fetchAll()` runs at first `loop()` iteration
- After saving an agent with a new API key: `doImmediateFetch = true`
- Background: every `FETCH_INTERVAL_MS` (10 min)
- Provider dispatch in `fetcher.cpp` by name prefix: OpenAI/GPT/Codex/O1/O3 → `syncOpenAI()`; DeepSeek → `syncDeepSeek()`

### Display Render Logic (display.cpp)
`display_render()` adapts to available data:
1. `limit > 0` → progress bar + "X / Y tokens"
2. `used > 0` only → "Used: X tokens" (no bar)
3. `balance >= 0` only → "Balance: $X.XX"
4. Nothing → "Active" + "Syncing..."

### Web Files (`data/`)
Served from SPIFFS. `index.html` + `app.js` + `style.css` — production UI. `wifi-setup.html` — captive portal page. `temporary.html` (project root, not in SPIFFS) — standalone browser test with `MockWebSocket` and simulated fetch results; uses `localStorage` key `tt_agents_v2`.

### TFT_eSPI Configuration
Display driver is configured entirely via `build_flags` in `platformio.ini` (no `User_Setup.h` file needed). `USER_SETUP_LOADED=1` prevents the library from using its own setup. Pin mapping: SCLK=4, MOSI=6, CS=7, DC=2, RST=3, BL=8.

## Key Constraints

- NVS keys max 15 chars: `ag0name`, `ag0model`, `ag0key`, `ag0used`, `ag0limit`, `ag0reset`, `ag0bal`, `ag0active`
- Max 6 agents (`MAX_AGENTS`)
- All display text and code comments must be in English
- API keys are stored only in NVS; never appear in WebSocket state broadcasts or on TFT display
- `display_tick()` does partial redraws only (pulse dot + countdown) to avoid flicker — `display_render()` does full redraws on state changes
