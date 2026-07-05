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

// ─── Anthropic (Claude) — tier rate-limit header probe ───────────────────────
// Uses a regular sk-ant-api03... developer API key via `x-api-key`. The
// Pro/Max-plan "unified" 5h/7d usage windows required a Claude Code OAuth
// session token (`claude setup-token`) sent as `Authorization: Bearer` —
// Anthropic disabled that for third-party clients (~Feb 2026; returns
// "OAuth authentication is currently not supported" regardless of header
// shape, confirmed via serial log — not something any header/encoding fix
// on our side can work around). A plain API key has no account-wide
// balance/usage endpoint, so the closest available signal is the
// account tier's standard per-minute token rate limit, exposed via
// `anthropic-ratelimit-tokens-*` response headers on every request.

// Parses an RFC3339 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ", the format
// Anthropic uses for `anthropic-ratelimit-*-reset`) into a Unix epoch.
// mktime() normally treats the struct as local time, but main.cpp's
// configTime(0, 0, ...) pins the device's local time to UTC (zero offset,
// zero DST), so local time IS UTC here — no timegm() needed.
static uint32_t parseRfc3339ToEpoch(const String& s) {
    int y, mo, d, h, mi, se;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = se;
    time_t epoch = mktime(&t);
    return (epoch > 0) ? (uint32_t)epoch : 0;
}

static bool syncAnthropic(Agent& agent) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    if (!http.begin(client, "https://api.anthropic.com/v1/messages")) return false;

    static const char* headerKeys[] = {
        "anthropic-ratelimit-tokens-limit",
        "anthropic-ratelimit-tokens-remaining",
        "anthropic-ratelimit-tokens-reset",
    };
    http.collectHeaders(headerKeys, 3);

    http.addHeader("x-api-key", agent.apiKey);
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "token-tracker-esp32/1.0");
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

    if (http.hasHeader("anthropic-ratelimit-tokens-limit") &&
        http.hasHeader("anthropic-ratelimit-tokens-remaining")) {
        long limitVal     = http.header("anthropic-ratelimit-tokens-limit").toInt();
        long remainingVal = http.header("anthropic-ratelimit-tokens-remaining").toInt();

        if (limitVal > 0) {
            uint32_t pct = (uint32_t)constrain(
                100.0f * (float)(limitVal - remainingVal) / (float)limitVal, 0.0f, 100.0f);

            agent.used    = pct;
            agent.limit   = 100;   // percentage-based window, not raw tokens
            agent.balance = -1.0f; // not applicable
            agent.resetEpoch    = parseRfc3339ToEpoch(http.header("anthropic-ratelimit-tokens-reset"));
            agent.used7d        = 0; // no second window with a standard API key
            agent.resetEpoch7d  = 0;
            ok = true;

            Serial.printf("[FETCH] Anthropic: tokens %ld/%ld used=%u%% reset=%u (HTTP %d)\n",
                          limitVal - remainingVal, limitVal, pct, agent.resetEpoch, code);
        }
    } else {
        Serial.printf("[FETCH] Anthropic: HTTP %d, no rate-limit headers (bad/invalid API key?)\n", code);
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
