#pragma once
#include <TFT_eSPI.h>
#include "storage.h"

// Initialize the ST7789 display and backlight.
void display_init();

// Render the full screen for the active agent.
// Call once when state changes; animation ticks use display_tick().
void display_render(const Agent* agent);

// Render the idle screen when no agent is active (shows IP address).
void display_renderIdle(const char* ip);

// Called from loop() every ~500ms to update animated elements only
// (pulse dot, countdown timer) without redrawing the whole screen.
void display_tick(const Agent* agent, uint32_t nowEpoch);

// Render the WiFi setup screen (AP mode). Shows AP SSID and 192.168.4.1.
void display_renderWifiSetup(const char* apSsid);

// Advance the "waiting for you" bouncing-dots animation on the WiFi setup
// screen. Call periodically (e.g. every ANIM_INTERVAL_MS) while blocked in
// the setup portal's connection-wait loop.
void display_tickWifiSetup();

// Render the connecting screen: SSID + a preview of the steps about to run.
void display_renderConnecting(const char* ssid);

// Advance the bouncing-dots animation on the connecting screen. Call
// periodically (e.g. every ANIM_INTERVAL_MS) while blocked waiting for
// WiFi.status() to become WL_CONNECTED.
void display_tickConnecting();
