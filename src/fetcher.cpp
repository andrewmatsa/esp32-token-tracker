#include "fetcher.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Arduino.h>

// ─── Provider detection ───────────────────────────────────────────────────────

static bool nameStartsWith(const char* name, const char* prefix) {
    return strncasecmp(name, prefix, strlen(prefix)) == 0;
}

static bool isOpenAI(const Agent& a) {
    // "codex" intentionally excluded: Codex usage isn't visible on the legacy
    // /v1/usage endpoint (it's tracked via a separate ChatGPT-plan/enterprise
    // analytics system), so this endpoint would always report 0 for it.
    return nameStartsWith(a.name, "gpt")    ||
           nameStartsWith(a.name, "openai") ||
           nameStartsWith(a.name, "o1")     ||
           nameStartsWith(a.name, "o3");
}

static bool isDeepSeek(const Agent& a) {
    return nameStartsWith(a.name, "deepseek");
}

static bool isAnthropic(const Agent& a) {
    return nameStartsWith(a.name, "claude") ||
           nameStartsWith(a.name, "anthropic");
}

static bool isCursor(const Agent& a) {
    return nameStartsWith(a.name, "cursor");
}

// ─── OpenAI — sum tokens from day 1 of current month to today ────────────────

static bool syncOpenAI(Agent& agent) {
    struct tm ti{};
    if (!getLocalTime(&ti)) {
        Serial.println("[FETCH] NTP not ready");
        return false;
    }

    int year  = ti.tm_year + 1900;
    int month = ti.tm_mon + 1;
    int today = ti.tm_mday;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    uint32_t total = 0;
    bool anyOk = false;

    // Track most-used model by token count
    struct ModelCount { char id[48]; uint32_t tokens; };
    ModelCount modelCounts[8]{};
    int modelCountLen = 0;

    Serial.printf("[FETCH] OpenAI: syncing %04d-%02d (1..%d)\n", year, month, today);

    for (int day = 1; day <= today; day++) {
        char url[96];
        snprintf(url, sizeof(url),
                 "https://api.openai.com/v1/usage?date=%04d-%02d-%02d",
                 year, month, day);

        if (!http.begin(client, url)) continue;
        http.addHeader("Authorization", String("Bearer ") + agent.apiKey);
        http.setTimeout(8000);

        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            String body = http.getString();
            JsonDocument doc;
            if (deserializeJson(doc, body) == DeserializationError::Ok) {
                for (JsonObject item : doc["data"].as<JsonArray>()) {
                    uint32_t t = item["n_context_tokens_total"].as<uint32_t>()
                               + item["n_generated_tokens_total"].as<uint32_t>();
                    total += t;
                    // Track model with most tokens
                    const char* snap = item["snapshot_id"] | "";
                    if (strlen(snap) > 0 && t > 0) {
                        bool found = false;
                        for (int m = 0; m < modelCountLen; m++) {
                            if (strcmp(modelCounts[m].id, snap) == 0) {
                                modelCounts[m].tokens += t; found = true; break;
                            }
                        }
                        if (!found && modelCountLen < 8) {
                            strlcpy(modelCounts[modelCountLen].id, snap, 48);
                            modelCounts[modelCountLen].tokens = t;
                            modelCountLen++;
                        }
                    }
                }
                anyOk = true;
            }
        } else {
            Serial.printf("[FETCH] OpenAI day %d: HTTP %d\n", day, code);
            if (code == 401 || code == 403) { http.end(); break; }
        }
        http.end();
        delay(80);
    }

    if (anyOk) {
        agent.used    = total;
        agent.balance = -1.0f;
        // Auto-detect dominant model
        if (modelCountLen > 0) {
            int best = 0;
            for (int m = 1; m < modelCountLen; m++)
                if (modelCounts[m].tokens > modelCounts[best].tokens) best = m;
            strlcpy(agent.model, modelCounts[best].id, sizeof(agent.model));
            Serial.printf("[FETCH] OpenAI: model=%s\n", agent.model);
        }
        // Reset on 1st of next month
        struct tm r{};
        r.tm_year = (month == 12) ? year - 1900 + 1 : year - 1900;
        r.tm_mon  = (month == 12) ? 0 : month;
        r.tm_mday = 1;
        agent.resetEpoch = (uint32_t)mktime(&r);
        Serial.printf("[FETCH] OpenAI: %u tokens, reset epoch %u\n", total, agent.resetEpoch);
    }
    return anyOk;
}

// ─── DeepSeek — credit balance ───────────────────────────────────────────────

static bool syncDeepSeek(Agent& agent) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    if (!http.begin(client, "https://api.deepseek.com/user/balance")) return false;
    http.addHeader("Authorization", String("Bearer ") + agent.apiKey);
    http.setTimeout(8000);

    int code = http.GET();
    bool ok  = false;

    if (code == HTTP_CODE_OK) {
        String body = http.getString();
        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            JsonArray infos = doc["balance_infos"].as<JsonArray>();
            for (JsonObject info : infos) {
                const char* currency = info["currency"] | "";
                // Prefer CNY (native), then USD
                if (strcmp(currency, "CNY") == 0 || strcmp(currency, "USD") == 0) {
                    agent.balance = atof(info["total_balance"] | "0");
                    agent.used    = 0;
                    ok = true;
                    Serial.printf("[FETCH] DeepSeek balance: %.4f %s\n", agent.balance, currency);
                    break;
                }
            }
            if (!ok && infos.size() > 0) {
                agent.balance = atof(infos[0]["total_balance"] | "0");
                ok = true;
            }
        }
    } else {
        Serial.printf("[FETCH] DeepSeek: HTTP %d\n", code);
    }
    http.end();
    return ok;
}

// ─── Anthropic (Claude) — rate-limit header probe ────────────────────────────
// Requires a Claude Code OAuth token (e.g. from `claude setup-token`), not a
// regular sk-ant-api03... API key. Sends a minimal 1-token Haiku request
// (near-zero cost) purely to read the `anthropic-ratelimit-unified-*`
// response headers, which carry the Pro/Max plan's 5h/7d usage windows.

static bool syncAnthropic(Agent& agent) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    if (!http.begin(client, "https://api.anthropic.com/v1/messages")) return false;

    // TEMPORARY diagnostic: confirm the stored token's length/shape without
    // ever printing the token itself. A genuine `claude setup-token` OAuth
    // token is a single unbroken string with a recognizable prefix
    // (typically "sk-ant-oat01-...") — if the length looks too short, or
    // first/last chars look wrong, the paste likely picked up extra text
    // or got cut off client-side before it ever reached the device.
    {
        int   len = strlen(agent.apiKey);
        char  head[16] = {0}, tail[16] = {0};
        strncpy(head, agent.apiKey, min(len, 12));
        if (len > 12) strncpy(tail, agent.apiKey + len - 8, 8);
        Serial.printf("[FETCH] Anthropic: stored key len=%d, starts '%s...', ends '...%s'\n",
                      len, head, tail);
    }

    static const char* headerKeys[] = {
        "anthropic-ratelimit-unified-5h-utilization",
        "anthropic-ratelimit-unified-5h-reset",
        "anthropic-ratelimit-unified-7d-utilization",
        "anthropic-ratelimit-unified-7d-reset",
        "anthropic-ratelimit-unified-status",
    };
    http.collectHeaders(headerKeys, 5);

    http.addHeader("Authorization", String("Bearer ") + agent.apiKey);
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("anthropic-beta", "oauth-2025-04-20"); // required for Bearer/OAuth auth
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);

    // agent.model doubles as the user-configurable probe model for Claude
    // (set via the web UI); falls back to Haiku if left blank.
    const char* model = (strlen(agent.model) > 0) ? agent.model : "claude-haiku-4-5";
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"model\":\"%s\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\".\"}]}",
             model);
    int code = http.POST((uint8_t*)payload, strlen(payload));
    http.getString(); // drain body, not needed
    bool ok = false;

    if (http.hasHeader("anthropic-ratelimit-unified-5h-utilization")) {
        float util5h = http.header("anthropic-ratelimit-unified-5h-utilization").toFloat();
        uint32_t pct5h = (uint32_t)constrain(util5h * 100.0f, 0.0f, 100.0f);

        float util7d = http.header("anthropic-ratelimit-unified-7d-utilization").toFloat();
        uint32_t pct7d = (uint32_t)constrain(util7d * 100.0f, 0.0f, 100.0f);

        agent.used    = pct5h;
        agent.limit   = 100;    // percentage-based window, not raw tokens
        agent.balance = -1.0f;  // not applicable
        agent.resetEpoch    = (uint32_t)http.header("anthropic-ratelimit-unified-5h-reset").toInt();
        agent.used7d        = pct7d;
        agent.resetEpoch7d  = (uint32_t)http.header("anthropic-ratelimit-unified-7d-reset").toInt();
        ok = true;

        Serial.printf("[FETCH] Anthropic: 5h=%u%% 7d=%u%% status=%s reset5h=%u reset7d=%u\n",
                      pct5h, pct7d,
                      http.header("anthropic-ratelimit-unified-status").c_str(),
                      agent.resetEpoch, agent.resetEpoch7d);
    } else {
        Serial.printf("[FETCH] Anthropic: HTTP %d, no rate-limit headers (needs an OAuth token)\n", code);
    }
    http.end();
    return ok;
}

// ─── Cursor — monthly request usage (unofficial personal-account endpoint) ───
// Requires the Cursor IDE's own access token — read automatically from its
// local state database by tools/cursor-usage-daemon.py, or pasted here
// directly (this is a plain Bearer token with no Origin restriction, unlike
// Claude's OAuth token, so on-device sync works without a companion daemon).
// Cloudflare rejects requests without a browser-like User-Agent (403, error
// code 1010) — that header is required below.

static bool syncCursor(Agent& agent) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    if (!http.begin(client, "https://api2.cursor.sh/auth/usage")) return false;
    http.addHeader("Authorization", String("Bearer ") + agent.apiKey);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    http.setTimeout(8000);

    int code = http.GET();
    bool ok = false;

    if (code == HTTP_CODE_OK) {
        String body = http.getString();
        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok) {
            uint32_t totalUsed = 0, totalLimit = 0;
            bool limitKnown = true;
            // agent.model, if set by the user, filters to that single model
            // bucket (e.g. "gpt-4") instead of summing every bucket.
            bool filterByModel = strlen(agent.model) > 0;
            for (JsonPair kv : doc.as<JsonObject>()) {
                if (strcmp(kv.key().c_str(), "startOfMonth") == 0) continue;
                if (filterByModel && strcmp(kv.key().c_str(), agent.model) != 0) continue;
                JsonObject model = kv.value().as<JsonObject>();
                if (model.isNull()) continue;
                totalUsed += model["numRequests"].as<uint32_t>();
                if (model["maxRequestUsage"].isNull()) {
                    limitKnown = false;
                } else {
                    totalLimit += model["maxRequestUsage"].as<uint32_t>();
                }
            }
            agent.used    = totalUsed;
            agent.limit   = (limitKnown && totalLimit > 0) ? totalLimit : 0;
            agent.balance = -1.0f;

            // Reset = one month after the billing period's start
            const char* startOfMonth = doc["startOfMonth"] | "";
            struct tm t{};
            if (strlen(startOfMonth) > 0 &&
                sscanf(startOfMonth, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday,
                       &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
                t.tm_year -= 1900;
                t.tm_mon  -= 1;
                t.tm_mon++; // advance one month past the billing period start
                if (t.tm_mon > 11) { t.tm_mon = 0; t.tm_year++; }
                agent.resetEpoch = (uint32_t)mktime(&t);
            }
            ok = true;
            Serial.printf("[FETCH] Cursor: used=%u limit=%u reset=%u\n",
                          agent.used, agent.limit, agent.resetEpoch);
        }
    } else {
        Serial.printf("[FETCH] Cursor: HTTP %d\n", code);
    }
    http.end();
    return ok;
}

// ─── Public ───────────────────────────────────────────────────────────────────

bool fetcher_sync(Agent& agent) {
    if (strlen(agent.apiKey) == 0) return false;
    if (isOpenAI(agent))    return syncOpenAI(agent);
    if (isDeepSeek(agent))  return syncDeepSeek(agent);
    if (isAnthropic(agent)) return syncAnthropic(agent);
    if (isCursor(agent))    return syncCursor(agent);
    Serial.printf("[FETCH] No auto-sync for '%s'\n", agent.name);
    return false;
}
