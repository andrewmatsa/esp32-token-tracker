#pragma once

// ─── WiFi provisioning ────────────────────────────────────────────────────────
#define AP_SSID_PREFIX          "TokenTracker-"
#define WIFI_CONNECT_TIMEOUT_S  15

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
