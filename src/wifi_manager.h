#pragma once
#include <Arduino.h>

// Try to load saved credentials from NVS and connect. Returns true on success.
bool wifi_connect(const char* apSsid);

// Start AP captive portal. Blocks until credentials are saved and connected.
// apSsid: the AP name shown to the user.
void wifi_runSetupPortal(const char* apSsid);

// Erase saved WiFi credentials from NVS and restart the device.
void wifi_clearCreds();

// Returns true if SSID credentials are stored in NVS.
bool wifi_hasCreds();

// Returns the SSID the device is currently connected to (empty string if not connected).
String wifi_currentSsid();

// Build AP SSID from prefix + last 4 hex chars of MAC.
String wifi_makeApSsid();
