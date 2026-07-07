#pragma once

// ─── WiFi provisioning ────────────────────────────────────────────────────────
#define AP_SSID_PREFIX          "TokenTracker-"
#define WIFI_CONNECT_TIMEOUT_S  15

// ─── TEMPORARY: STA TX diagnostic ────────────────────────────────────────────
// Fill in your real WiFi credentials here, flash, watch Serial Monitor, then
// delete/blank these again — this file is not meant to carry real
// credentials long-term. Leave TEST_STA_SSID empty to skip the diagnostic
// entirely (normal captive-portal boot).
#define TEST_STA_SSID  ""
#define TEST_STA_PASS  ""

// ─── Display SPI pins (ESP32-C3 Super Mini → ST7789 240x240) ─────────────────
#define PIN_SCLK  4
#define PIN_MOSI  6
#define PIN_CS    7
#define PIN_DC    2
#define PIN_RST   3
#define PIN_BL    8

// ─── Agent limits ─────────────────────────────────────────────────────────────
#define MAX_AGENTS       6
#define WARN_THRESHOLD   85   // percent — triggers warning mode
#define WARN_HOURS       24   // hours before reset — triggers warning mode

// ─── Display timing ──────────────────────────────────────────────────────────
#define ANIM_INTERVAL_MS    500     // pulse animation tick
#define CLOCK_INTERVAL_MS  1000     // countdown timer refresh

// ─── Auto-fetch ───────────────────────────────────────────────────────────────
#define FETCH_INTERVAL_MS  600000UL // 10 minutes between background syncs

// Fallback threshold (seconds) for how long without a PC daemon /push
// before an agent that depends on it is considered stale and shows a
// "start the daemon" warning instead of its last-known info line. Used
// as-is for keyed agents (display.cpp's daemonStaleThreshold() — their
// syncIntervalSec means the on-device probe cadence, not the daemon's, so
// there's no better on-device signal); for keyless agents, that function
// instead scales the daemon-command-suggested syncIntervalSec by the same
// DAEMON_STALE_MULTIPLIER. tools/usage-daemon.py's own default --interval
// is 120s, so 300s (2.5x) tolerates normal jitter without being too slow to
// notice a genuinely stopped daemon. Mirrored in data/app.js's
// DAEMON_STALE_SEC — keep both in sync.
#define DAEMON_STALE_SEC  300

// ─── Anthropic probe defaults ─────────────────────────────────────────────────
// Model used by syncAnthropic() when the user leaves the web UI's Model
// dropdown on "Default". Shared with display.cpp so the TFT/web preview can
// show this name when agent.model is empty instead of leaving it blank.
#define ANTHROPIC_DEFAULT_PROBE_MODEL  "claude-haiku-4-5"
