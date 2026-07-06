#include "webserver.h"
#include "wifi_manager.h"
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>

static AsyncWebServer  server(80);
static AsyncWebSocket  ws("/ws");

static Agent*        _agents   = nullptr;
static int*          _count    = nullptr;
static OnAgentUpdate  _cbUpdate  = nullptr;
static OnSetActive    _cbActive  = nullptr;
static OnSetEnabled   _cbEnabled = nullptr;
static OnDelete       _cbDelete  = nullptr;
static OnExternalPush _cbPush    = nullptr;

// ─── JSON helpers ─────────────────────────────────────────────────────────────

static String buildStateJson(Agent agents[MAX_AGENTS], int count) {
    JsonDocument doc;
    doc["type"] = "state";
    JsonArray arr = doc["agents"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["name"]       = agents[i].name;
        o["model"]      = agents[i].model;
        o["probeModel"] = agents[i].probeModel;
        o["hasKey"]     = (strlen(agents[i].apiKey) > 0); // never send key back to browser
        o["used"]       = agents[i].used;
        o["limit"]      = agents[i].limit;
        o["resetEpoch"] = agents[i].resetEpoch;
        o["balance"]    = agents[i].balance;
        o["active"]     = agents[i].active;
        o["enabled"]    = agents[i].enabled;
        o["syncInterval"] = agents[i].syncIntervalSec;
        o["used7d"]       = agents[i].used7d;
        o["resetEpoch7d"] = agents[i].resetEpoch7d;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ─── WebSocket handler ────────────────────────────────────────────────────────

static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        client->text(buildStateJson(_agents, *_count));
        return;
    }
    if (type != WS_EVT_DATA) return;

    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info->final || info->index != 0 || info->len != len) return;
    if (info->opcode != WS_TEXT) return;

    JsonDocument doc;
    if (deserializeJson(doc, data, len) != DeserializationError::Ok) return;

    const char* msgType = doc["type"] | "";

    if (strcmp(msgType, "update") == 0) {
        int idx = doc["index"] | -1;
        if (idx < 0 || idx >= MAX_AGENTS) return;
        Agent a{};
        a.enabled = true; // default for a brand-new agent; overwritten below if it already exists
        // Preserve existing data if this slot already exists
        if (_agents && idx < *_count) a = _agents[idx];

        strlcpy(a.name, doc["name"] | "", sizeof(a.name));

        // Codex/Cursor: `model` is their manual usage-bucket filter, settable here.
        // Claude: `model` is real-last-used, owned only by the daemon's /push — never
        // accepted from the browser; `probeModel` is Claude's rate-limit target instead.
        const char* newModel = doc["model"] | "";
        if (strlen(newModel) > 0) strlcpy(a.model, newModel, sizeof(a.model));

        const char* newProbeModel = doc["probeModel"] | "";
        if (strlen(newProbeModel) > 0) strlcpy(a.probeModel, newProbeModel, sizeof(a.probeModel));

        // Only update API key if a new non-empty value was provided
        const char* newKey = doc["apiKey"] | "";
        if (strlen(newKey) > 0) strlcpy(a.apiKey, newKey, sizeof(a.apiKey));

        // Claude only: probe sync interval, in seconds (0 = use device default)
        if (!doc["syncInterval"].isNull()) a.syncIntervalSec = doc["syncInterval"] | 0;

        if (_cbUpdate) _cbUpdate(idx, a);

    } else if (strcmp(msgType, "setActive") == 0) {
        int idx = doc["index"] | -1;
        if (idx >= 0 && _cbActive) _cbActive(idx);

    } else if (strcmp(msgType, "setEnabled") == 0) {
        int idx = doc["index"] | -1;
        bool enabled = doc["enabled"] | true;
        if (idx >= 0 && _cbEnabled) _cbEnabled(idx, enabled);

    } else if (strcmp(msgType, "delete") == 0) {
        int idx = doc["index"] | -1;
        if (idx >= 0 && _cbDelete) _cbDelete(idx);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void webserver_init(Agent agents[MAX_AGENTS], int* count,
                    OnAgentUpdate  cbUpdate,
                    OnSetActive    cbActive,
                    OnSetEnabled   cbEnabled,
                    OnDelete       cbDelete,
                    OnExternalPush cbPush) {
    _agents    = agents;
    _count     = count;
    _cbUpdate  = cbUpdate;
    _cbActive  = cbActive;
    _cbEnabled = cbEnabled;
    _cbDelete  = cbDelete;
    _cbPush    = cbPush;

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // Push endpoint — lets an external companion process (e.g. a PC daemon
    // reading a local OAuth token) report usage without the token ever
    // touching this device. Same unauthenticated trust model as /wifi/reset.
    server.on("/push", HTTP_POST, [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index + len != total) return; // wait for the full body
            JsonDocument doc;
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            int idx = doc["index"] | -1;
            if (idx < 0 || !_count || idx >= *_count) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"bad index\"}");
                return;
            }
            uint32_t used       = doc["used"]  | 0;
            uint32_t limit      = doc["limit"] | 0;
            uint32_t reset      = doc["resetEpoch"] | 0;
            uint32_t used7d     = doc["used7d"] | 0;
            uint32_t resetEpoch7d = doc["resetEpoch7d"] | 0;
            // Optional: real last-used model / estimated cost (currently Claude
            // only — the daemon omits these keys entirely for providers that
            // don't resolve them, so other agents' existing values are
            // untouched). `model` and `probeModel` are separate fields now
            // (storage.h), so this can apply unconditionally — a keyed agent's
            // manual rate-limit probe target lives in `probeModel` and is never
            // touched here, regardless of whether the daemon also runs for it.
            if (doc["model"].is<const char*>())
                strlcpy(_agents[idx].model, doc["model"], sizeof(_agents[idx].model));
            if (doc["balance"].is<float>())
                _agents[idx].balance = doc["balance"];
            if (_cbPush) _cbPush(idx, used, limit, reset, used7d, resetEpoch7d);
            req->send(200, "application/json", "{\"ok\":true}");
        });

    // WiFi info endpoint — used by the web UI to show current SSID + IP
    server.on("/wifi/info", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["ssid"] = wifi_currentSsid();
        doc["ip"]   = WiFi.localIP().toString();
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // WiFi reset endpoint — clears credentials and reboots into AP setup mode
    server.on("/wifi/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", "{\"ok\":true}");
        delay(300);
        wifi_clearCreds();  // clears NVS and calls ESP.restart()
    });

    // Serve all static files from SPIFFS
    server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

    server.begin();
    Serial.println("[WS] HTTP server started on port 80");
}

void webserver_broadcastState(Agent agents[MAX_AGENTS], int count) {
    if (ws.count() == 0) return;
    ws.textAll(buildStateJson(agents, count));
}

void webserver_loop() {
    ws.cleanupClients();
}
