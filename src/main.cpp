#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <time.h>
#include <esp_log.h>
#include "config.h"
#include "storage.h"
#include "display.h"
#include "webserver.h"
#include "wifi_manager.h"
#include "fetcher.h"

// ─── Global state ─────────────────────────────────────────────────────────────
static Agent agents[MAX_AGENTS];
static int   agentCount = 0;
static int   activeIdx  = -1;

// ─── Timers ───────────────────────────────────────────────────────────────────
static unsigned long lastAnim       = 0;
static unsigned long lastFetchCheck = 0;
static unsigned long lastFetchAt[MAX_AGENTS] = {0};
static bool          pendingFetch[MAX_AGENTS] = {false};
#define FETCH_CHECK_MS 5000UL // how often we check which agents are due (cheap, no-op if none)

// ─── Find active agent index ──────────────────────────────────────────────────
static int findActive() {
    for (int i = 0; i < agentCount; i++)
        if (agents[i].active) return i;
    return -1;
}

// ─── Fetch all agents that are due, honoring each agent's own interval ──────
// Agents with syncIntervalSec == 0 fall back to the device-wide FETCH_INTERVAL_MS.
static void fetchAll() {
    unsigned long now = millis();
    bool anyUpdated = false;
    for (int i = 0; i < agentCount; i++) {
        if (strlen(agents[i].apiKey) == 0) continue;
        if (!agents[i].enabled) continue;

        unsigned long interval = (agents[i].syncIntervalSec > 0)
            ? (unsigned long)agents[i].syncIntervalSec * 1000UL
            : FETCH_INTERVAL_MS;
        if (!pendingFetch[i] && (now - lastFetchAt[i] < interval)) continue;

        pendingFetch[i]  = false;
        lastFetchAt[i]   = now;

        // Ephemeral scheduler state for the "Sync in" display — when the
        // NEXT on-device probe for this agent is due. Only meaningful for
        // agents fetchAll() actually probes (real API key, checked above);
        // never persisted, just recomputed every cycle.
        struct tm ti;
        uint32_t nowEpoch = getLocalTime(&ti) ? (uint32_t)mktime(&ti) : 0;
        if (nowEpoch > 0) agents[i].nextSyncEpoch = nowEpoch + (uint32_t)(interval / 1000UL);

        if (fetcher_sync(agents[i])) {
            storage_save(i, agents[i]);
            anyUpdated = true;
        }
    }
    if (anyUpdated) {
        if (activeIdx >= 0) display_render(&agents[activeIdx]);
        // No push to the browser — it polls GET /state on its own interval.
    }
}

// ─── WebServer callbacks ──────────────────────────────────────────────────────
static void onUpdate(int index, const Agent& agent) {
    if (index >= agentCount) agentCount = index + 1;
    agents[index] = agent;
    storage_save(index, agent);
    activeIdx = findActive();

    // Flag for an immediate fetch on the next loop() tick (≤ FETCH_CHECK_MS)
    // instead of syncing here: this callback runs on the async web-server
    // task, and fetcher_sync() is a blocking HTTPS request — running it here
    // stalls the HTTP response to the browser and risks a watchdog reset on
    // the AsyncTCP task. fetchAll() does it safely on the main loop.
    if (strlen(agents[index].apiKey) > 0 && agents[index].enabled)
        pendingFetch[index] = true;

    display_render(activeIdx >= 0 ? &agents[activeIdx] : nullptr);
}

static void onSetActive(int index) {
    if (index < 0 || index >= agentCount) return;
    storage_setActive(index, agents, agentCount);
    activeIdx = index;
    display_render(&agents[activeIdx]);
}

static void onSetEnabled(int index, bool enabled) {
    if (index < 0 || index >= agentCount) return;
    agents[index].enabled = enabled;
    storage_save(index, agents[index]);
    // Defer the fetch to the main loop (see onUpdate) — never block this
    // async web-server callback with a synchronous HTTPS probe.
    if (enabled && strlen(agents[index].apiKey) > 0)
        pendingFetch[index] = true;
    if (index == activeIdx) display_render(&agents[activeIdx]);
}

static void onDelete(int index) {
    if (index < 0 || index >= agentCount) return;
    storage_delete(index, agents, agentCount);
    activeIdx = findActive();
    display_render(activeIdx >= 0 ? &agents[activeIdx] : nullptr);
}

// Usage pushed by an external companion process (e.g. the PC daemon reading
// a local Claude Code OAuth token) instead of the device's own fetcher.
static void onExternalPush(int index, uint32_t used, uint32_t limit, uint32_t resetEpoch,
                           uint32_t used7d, uint32_t resetEpoch7d) {
    if (index < 0 || index >= agentCount) return;
    agents[index].used         = used;
    agents[index].limit        = limit;
    agents[index].resetEpoch   = resetEpoch;
    agents[index].used7d       = used7d;
    agents[index].resetEpoch7d = resetEpoch7d;
    agents[index].lastSyncEpoch = (uint32_t)time(nullptr);
    agents[index].lastPushEpoch = agents[index].lastSyncEpoch;
    storage_save(index, agents[index]);
    if (index == activeIdx) display_render(&agents[activeIdx]);
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("[BOOT] Token Tracker starting");

    // TEMPORARY: surface the esp-idf WiFi driver's own internal log lines
    // (auth/assoc failures, RF init) instead of only Arduino's WiFi.h
    // status codes, while diagnosing the AP/STA transmit issue.
    esp_log_level_set("wifi", ESP_LOG_VERBOSE);
    esp_log_level_set("wifi_init", ESP_LOG_VERBOSE);

    display_init();

    // SPIFFS must be mounted before wifi_manager (setup page lives there)
    if (!SPIFFS.begin(true)) {
        Serial.println("[BOOT] SPIFFS mount failed");
    }

    // TEMPORARY: one-off STA TX diagnostic — see include/config.h. Tests
    // whether the radio can actually transmit and complete a real
    // association, independent of the (possibly broken) AP/beacon path.
    // Does not affect normal boot when TEST_STA_SSID is left blank.
    if (strlen(TEST_STA_SSID) > 0) {
        Serial.printf("[DIAG] Attempting STA connect to '%s'...\n", TEST_STA_SSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(TEST_STA_SSID, TEST_STA_PASS);
        uint32_t deadline = millis() + 15000UL;
        while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(200);
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[DIAG] STA connected! IP=%s RSSI=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        } else {
            Serial.printf("[DIAG] STA connect FAILED, status=%d\n", (int)WiFi.status());
        }
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(200);
    }

    String apSsid = wifi_makeApSsid();

    // Try stored credentials; fall back to captive portal (blocks + restarts)
    if (!wifi_connect(apSsid.c_str())) {
        wifi_runSetupPortal(apSsid.c_str());
        // Never reaches here — portal restarts the device on success
    }

    // Sync NTP for countdown accuracy (UTC; display shows remaining seconds)
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");

    // Advertise as token-tracker.local so the PC companion daemon
    // (tools/usage-daemon.py) doesn't need a hardcoded/changing IP — DHCP
    // leases shift, but the mDNS name doesn't. Requires the querying OS to
    // support mDNS (.local) resolution — built in on macOS/Linux, needs
    // Bonjour or Windows 10's native multicast DNS support on Windows;
    // usage-daemon.py's --ip flag still accepts a plain IP as a fallback.
    if (MDNS.begin("token-tracker")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[MDNS] Responder started: token-tracker.local");
    } else {
        Serial.println("[MDNS] Failed to start");
    }

    // Load persisted agents from NVS
    agentCount = storage_load(agents);
    Serial.printf("[NVS] Loaded %d agents\n", agentCount);
    activeIdx = findActive();

    // Start web server
    webserver_init(agents, &agentCount, onUpdate, onSetActive, onSetEnabled, onDelete, onExternalPush);

    // Show cached data immediately; fetch will happen at first loop() iteration.
    // token-tracker.local (mDNS, started above) survives DHCP IP changes,
    // so it's a more durable hint than WiFi.localIP() would be.
    if (activeIdx >= 0)
        display_render(&agents[activeIdx]);
    else
        display_renderIdle("token-tracker.local");

    for (int i = 0; i < agentCount; i++) pendingFetch[i] = true;
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    // AsyncWebServer handles requests off its own task — nothing to service
    // here (the old ws.cleanupClients() call is gone with the WebSocket).
    unsigned long now = millis();

    // Check (cheaply) which agents are due for a fetch, honoring per-agent intervals
    if (now - lastFetchCheck >= FETCH_CHECK_MS) {
        lastFetchCheck = now;
        fetchAll();
    }

    // Display animation tick
    if (now - lastAnim >= ANIM_INTERVAL_MS) {
        lastAnim = now;
        if (activeIdx >= 0) {
            struct tm ti;
            uint32_t epoch = getLocalTime(&ti) ? (uint32_t)mktime(&ti) : 0;
            display_tick(&agents[activeIdx], epoch);
        }
    }
}
