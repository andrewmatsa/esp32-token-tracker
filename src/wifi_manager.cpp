#include "wifi_manager.h"
#include "config.h"
#include "display.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

static const char NVS_NS[]   = "wifi";
static const char KEY_SSID[] = "ssid";
static const char KEY_PASS[] = "pass";

// ─── NVS helpers ─────────────────────────────────────────────────────────────

static void saveCreds(const String& ssid, const String& pass) {
    Preferences p;
    p.begin(NVS_NS, false);
    p.putString(KEY_SSID, ssid);
    p.putString(KEY_PASS, pass);
    p.end();
}

static bool loadCreds(String& ssid, String& pass) {
    Preferences p;
    p.begin(NVS_NS, true);
    ssid = p.getString(KEY_SSID, "");
    pass = p.getString(KEY_PASS, "");
    p.end();
    return ssid.length() > 0;
}

// ─── Public ───────────────────────────────────────────────────────────────────

bool wifi_hasCreds() {
    String s, p;
    return loadCreds(s, p);
}

String wifi_currentSsid() {
    return (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String();
}

String wifi_makeApSsid() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char suffix[5];
    snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
    return String(AP_SSID_PREFIX) + suffix;
}

void wifi_clearCreds() {
    Preferences p;
    p.begin(NVS_NS, false);
    p.clear();
    p.end();
    ESP.restart();
}

// Try connecting with stored credentials. Returns true on success.
bool wifi_connect(const char* apSsid) {
    String ssid, pass;
    if (!loadCreds(ssid, pass)) return false;

    Serial.printf("[WiFi] Connecting to %s\n", ssid.c_str());
    display_renderConnecting(ssid.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_S * 1000UL;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(200);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("[WiFi] Connection failed");
    WiFi.disconnect(true);
    return false;
}

// ─── Setup portal (blocking) ──────────────────────────────────────────────────

void wifi_runSetupPortal(const char* apSsid) {
    Serial.printf("[AP] Starting portal: %s\n", apSsid);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSsid);
    delay(100);

    IPAddress apIp(192, 168, 4, 1);
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));

    display_renderWifiSetup(apSsid);

    // Captive DNS — redirect all domains to us
    DNSServer dns;
    dns.start(53, "*", apIp);

    AsyncWebServer portalServer(80);
    volatile bool  connected = false;

    // Serve setup page. The path arg must be the directory ("/"), not the
    // file itself — passing "/wifi-setup.html" here made the library look
    // for "/wifi-setup.html/wifi-setup.html" (path + default file appended),
    // which doesn't exist, so canHandle() always failed and requests fell
    // through to onNotFound's redirect-to-"/" below — an infinite redirect
    // loop that renders as a blank page in the browser.
    portalServer.serveStatic("/", SPIFFS, "/")
                .setDefaultFile("wifi-setup.html");
    portalServer.serveStatic("/style.css", SPIFFS, "/style.css");

    // Captive portal redirect hooks (iOS, Android, Windows)
    auto redirect = [&](AsyncWebServerRequest* req) {
        req->redirect("http://192.168.4.1/");
    };
    portalServer.on("/hotspot-detect.html", HTTP_GET, redirect);
    portalServer.on("/generate_204",        HTTP_GET, redirect);
    portalServer.on("/connecttest.txt",     HTTP_GET, redirect);
    portalServer.on("/ncsi.txt",            HTTP_GET, redirect);
    portalServer.onNotFound([&](AsyncWebServerRequest* req){ req->redirect("http://192.168.4.1/"); });

    // WiFi scan endpoint
    portalServer.on("/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        int n = WiFi.scanNetworks(false, false);
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["ssid"] = WiFi.SSID(i);
            o["rssi"] = WiFi.RSSI(i);
            o["enc"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // Save credentials endpoint
    portalServer.on("/wifi/save", HTTP_POST, [&](AsyncWebServerRequest* req) {
        if (!req->hasParam("ssid", true) || !req->hasParam("pass", true)) {
            req->send(400, "application/json", "{\"ok\":false,\"msg\":\"Missing fields\"}");
            return;
        }
        String ssid = req->getParam("ssid", true)->value();
        String pass = req->getParam("pass", true)->value();

        Serial.printf("[AP] Trying SSID: %s\n", ssid.c_str());
        display_renderConnecting(ssid.c_str());

        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());

        uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_S * 1000UL;
        while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
            dns.processNextRequest();
            delay(200);
        }

        if (WiFi.status() == WL_CONNECTED) {
            saveCreds(ssid, pass);
            String ip = WiFi.localIP().toString();
            Serial.printf("[AP] Connected! IP: %s\n", ip.c_str());
            req->send(200, "application/json",
                      "{\"ok\":true,\"ip\":\"" + ip + "\"}");
            connected = true;
        } else {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_AP);
            display_renderWifiSetup(apSsid);
            req->send(200, "application/json",
                      "{\"ok\":false,\"msg\":\"Wrong password or network not found\"}");
        }
    });

    portalServer.begin();

    // Block until connected, keep DNS running
    while (!connected) {
        dns.processNextRequest();
        delay(10);
    }

    // Small delay so browser receives the success response before restart
    delay(1500);
    ESP.restart();
}
