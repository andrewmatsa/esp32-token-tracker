#include "display.h"
#include "config.h"
#include <Arduino.h>
#include <time.h>

// ─── Color palette ────────────────────────────────────────────────────────────
#define C_BG        TFT_BLACK
#define C_HEADER    0x1082          // dark grey
#define C_TEXT      TFT_WHITE
#define C_SUBTEXT   0xAD55          // light grey
#define C_GREEN     0x07E0
#define C_ORANGE    0xFD20
#define C_RED       TFT_RED
#define C_DARKRED   0x6000
#define C_BAR_BG    0x2945          // dark bar background
#define C_WIFI      0xFD20          // WiFi indicator dot — orange, not blue/cyan (project palette: orange/grey/white/black only)
#define C_SPRITE    0xDBAA          // Claude sprite terracotta (~#d97757)
#define C_PILL_BG   0x394A          // Claude usage pill background (~#3a2b52)
#define C_PILL_TXT  0xCD5F          // Claude usage pill text (~#c9a8ff)
#define C_CARD_BRD  0x2104          // Claude card border grey (~#262626)
#define C_ACC_OPENAI    0x8AFE      // OpenAI accent (~#8B5CF6) — matches data/app.js's PRESETS
#define C_ACC_CURSOR    0x3C1E      // Cursor accent (~#3B82F6) — exact web blue, deliberate
#define C_ACC_DEEPSEEK  0x15D4      // DeepSeek accent (~#14B8A6) — exact web teal, deliberate
#define C_ACC_CODEX     0x15D0      // Codex accent (~#10B981)

// ─── Layout constants (240x240) ───────────────────────────────────────────────
#define HDR_H       56   // header block height
#define BAR_Y      120   // progress bar top y
#define BAR_H       28   // progress bar height
#define BAR_X       16
#define BAR_W      208

static TFT_eSPI tft;
static bool     _blinkState  = false;
static uint8_t  _pulseRadius = 6;
static bool     _pulseGrow   = false;
static uint8_t  _syncDotPhase = 0;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static uint16_t barColor(uint32_t pct) {
    if (pct >= 85) return C_RED;
    if (pct >= 50) return C_ORANGE;
    return C_GREEN;
}

static uint32_t usedPct(uint32_t used, uint32_t limit) {
    return (limit > 0) ? min((uint32_t)100, (used * 100 + limit / 2) / limit) : 0;
}

static void drawProgressBar(uint32_t used, uint32_t limit) {
    uint32_t pct = usedPct(used, limit);
    uint16_t filled = (uint16_t)(BAR_W * pct / 100);

    // Background track
    tft.fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, 5, C_BAR_BG);

    // Filled portion
    if (filled > 0)
        tft.fillRoundRect(BAR_X, BAR_Y, filled, BAR_H, 5, barColor(pct));

    // Percentage label centered on bar
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_TEXT, C_BAR_BG);
    tft.setTextFont(2);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    // Draw label over filled area if wide enough, otherwise over track
    tft.drawString(buf, 120, BAR_Y + BAR_H / 2);
}

static void formatTokens(char* buf, size_t len, uint32_t val) {
    if (val >= 1000000)
        snprintf(buf, len, "%.1fM", val / 1000000.0f);
    else if (val >= 1000)
        snprintf(buf, len, "%.1fK", val / 1000.0f);
    else
        snprintf(buf, len, "%u", (unsigned)val);
}

// "Sync in Xh Ym"/"Xm Ys" — when the next on-device fetchAll() probe for this
// agent is due. Only meaningful for agents with a real API key (keyless/
// daemon-driven agents never get nextSyncEpoch set, so this renders blank).
static void formatSyncIn(char* buf, size_t len, uint32_t nextSyncEpoch, uint32_t nowEpoch) {
    if (nextSyncEpoch == 0) { buf[0] = '\0'; return; }
    if (nowEpoch >= nextSyncEpoch) { strlcpy(buf, "Sync due", len); return; }
    uint32_t secs = nextSyncEpoch - nowEpoch;
    if (secs >= 86400)
        snprintf(buf, len, "Sync in %ud %uh", (unsigned)(secs / 86400), (unsigned)((secs % 86400) / 3600));
    else if (secs >= 3600)
        snprintf(buf, len, "Sync in %uh %02um", (unsigned)(secs / 3600), (unsigned)((secs % 3600) / 60));
    else
        snprintf(buf, len, "Sync in %um %02us", (unsigned)(secs / 60), (unsigned)(secs % 60));
}

// Generic (non-Claude) card's bottom line: "Resets in..." on the left,
// "Sync in..." on the right (blank when nextSyncEpoch==0 — keyless agents).
static void drawCountdown(uint32_t resetEpoch, uint32_t nowEpoch, uint32_t nextSyncEpoch, uint16_t y, bool warn) {
    char leftBuf[32] = "";
    uint16_t leftColor = warn ? C_ORANGE : C_SUBTEXT;
    if (resetEpoch > 0) {
        if (nowEpoch >= resetEpoch) {
            strlcpy(leftBuf, "Reset due", sizeof(leftBuf));
            leftColor = C_GREEN;
        } else {
            uint32_t secs = resetEpoch - nowEpoch;
            if (secs >= 86400)
                snprintf(leftBuf, sizeof(leftBuf), "Resets in %ud %uh", (unsigned)(secs / 86400), (unsigned)((secs % 86400) / 3600));
            else if (secs >= 3600)
                snprintf(leftBuf, sizeof(leftBuf), "Resets in %uh %02um", (unsigned)(secs / 3600), (unsigned)((secs % 3600) / 60));
            else
                snprintf(leftBuf, sizeof(leftBuf), "Resets in %um %02us", (unsigned)(secs / 60), (unsigned)(secs % 60));
        }
    }

    char rightBuf[24];
    formatSyncIn(rightBuf, sizeof(rightBuf), nextSyncEpoch, nowEpoch);

    // Clear line first to avoid ghost characters
    tft.fillRect(0, y - 10, 240, 20, C_BG);
    tft.setTextFont(1); // smaller than before — needs to fit two columns on 240px

    if (rightBuf[0] == '\0') {
        // No sync countdown to show (keyless agent) — center the reset text
        // alone, same as before this feature existed.
        if (leftBuf[0]) {
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(leftColor, C_BG);
            tft.drawString(leftBuf, 120, y);
        }
        return;
    }

    if (leftBuf[0]) {
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(leftColor, C_BG);
        tft.drawString(leftBuf, 4, y);
    }
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString(rightBuf, 236, y);
}


// ─── Pulse dot animation in header ───────────────────────────────────────────

static void drawPulseDot(bool warn) {
    // Erase previous dot area
    tft.fillCircle(18, 18, 10, warn ? C_DARKRED : C_HEADER);

    uint16_t col = warn ? C_ORANGE : C_GREEN;
    tft.fillCircle(18, 18, _pulseRadius, col);

    // Advance pulse
    if (_pulseGrow) {
        _pulseRadius++;
        if (_pulseRadius >= 9) _pulseGrow = false;
    } else {
        _pulseRadius--;
        if (_pulseRadius <= 4) _pulseGrow = true;
    }
}

static void drawWifiDot() {
    tft.fillCircle(228, 12, 5, C_WIFI);
}

// Bouncing 3-dot "waiting for first data" indicator — same language as
// display_tickWifiSetup()'s waiting-dots, shown while an agent has no
// used/limit/balance/resetEpoch data at all yet (before its first successful fetch).
static void tickSyncingDots(int16_t y) {
    const int cx[3] = {105, 120, 135};
    tft.fillRect(90, y - 8, 60, 16, C_BG);
    for (int i = 0; i < 3; i++) {
        bool active = (i == _syncDotPhase);
        tft.fillCircle(cx[i], y, active ? 5 : 3, active ? C_ORANGE : C_SUBTEXT);
    }
    _syncDotPhase = (_syncDotPhase + 1) % 3;
}

// ─── Provider "Usage" card screen ────────────────────────────────────────────
// Mirrors the browser preview's watch-style layout (data/app.js's
// renderUsageCard()/renderUsageScreen()) — a rounded card with a big value,
// pill badge, progress bar, and reset/sync line. Originally Claude-only;
// generalized to also cover OpenAI, Cursor, DeepSeek, and Codex, each with
// its own pixel-art sprite and accent color (matching data/app.js's PRESETS
// hex colors exactly). Any other/unrecognized provider name falls back to
// the plain generic screen further down in this file (display_render()'s
// non-card body), unchanged.

#define CARD_X   16
#define CARD_W  208
#define CARD_H   68
#define CARD1_Y  84
#define CARD2_Y 160

enum ProviderId { PROV_CLAUDE, PROV_OPENAI, PROV_CURSOR, PROV_DEEPSEEK, PROV_CODEX, PROV_NONE };

static ProviderId classifyProvider(const char* name) {
    if (strncasecmp(name, "claude", 6) == 0 || strncasecmp(name, "anthropic", 9) == 0) return PROV_CLAUDE;
    if (strncasecmp(name, "gpt", 3) == 0 || strncasecmp(name, "openai", 6) == 0 ||
        strncasecmp(name, "o1", 2) == 0  || strncasecmp(name, "o3", 2) == 0)           return PROV_OPENAI;
    if (strncasecmp(name, "cursor", 6) == 0)   return PROV_CURSOR;
    if (strncasecmp(name, "deepseek", 8) == 0) return PROV_DEEPSEEK;
    if (strncasecmp(name, "codex", 5) == 0)     return PROV_CODEX;
    return PROV_NONE;
}

static void drawClaudeSprite(int16_t x, int16_t y, uint8_t scale) {
    static const uint8_t px[5][9] = {
        {0,1,1,1,1,1,1,1,0},
        {0,1,0,1,1,1,0,1,0},
        {1,1,1,1,1,1,1,1,1},
        {0,1,1,1,1,1,1,1,0},
        {0,1,0,1,0,1,0,1,0},
    };
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 9; c++)
            if (px[r][c])
                tft.fillRect(x + c * scale, y + r * scale, scale, scale, C_SPRITE);
}

// Ringed circle, approximating OpenAI's web icon '◎'.
static void drawOpenAISprite(int16_t x, int16_t y, uint8_t scale) {
    static const uint8_t px[5][9] = {
        {0,0,1,1,1,1,1,0,0},
        {0,1,1,0,0,0,1,1,0},
        {1,1,0,0,0,0,0,1,1},
        {0,1,1,0,0,0,1,1,0},
        {0,0,1,1,1,1,1,0,0},
    };
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 9; c++)
            if (px[r][c])
                tft.fillRect(x + c * scale, y + r * scale, scale, scale, C_ACC_OPENAI);
}

// 4-point star, approximating Cursor's web icon '✦'.
static void drawCursorSprite(int16_t x, int16_t y, uint8_t scale) {
    static const uint8_t px[5][9] = {
        {0,0,0,0,1,0,0,0,0},
        {0,0,0,1,1,1,0,0,0},
        {0,1,1,1,1,1,1,1,0},
        {0,0,0,1,1,1,0,0,0},
        {0,0,0,0,1,0,0,0,0},
    };
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 9; c++)
            if (px[r][c])
                tft.fillRect(x + c * scale, y + r * scale, scale, scale, C_ACC_CURSOR);
}

// Lightning bolt, approximating Codex's web icon '⚡'.
static void drawCodexSprite(int16_t x, int16_t y, uint8_t scale) {
    static const uint8_t px[5][9] = {
        {0,0,0,1,1,1,0,0,0},
        {0,0,1,1,0,0,0,0,0},
        {0,1,1,1,1,1,1,0,0},
        {0,0,0,0,1,1,0,0,0},
        {0,0,0,1,1,0,0,0,0},
    };
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 9; c++)
            if (px[r][c])
                tft.fillRect(x + c * scale, y + r * scale, scale, scale, C_ACC_CODEX);
}

// Diamond, approximating DeepSeek's web icon '◈'.
static void drawDeepSeekSprite(int16_t x, int16_t y, uint8_t scale) {
    static const uint8_t px[5][9] = {
        {0,0,0,0,1,0,0,0,0},
        {0,0,0,1,1,1,0,0,0},
        {0,0,1,1,1,1,1,0,0},
        {0,0,0,1,1,1,0,0,0},
        {0,0,0,0,1,0,0,0,0},
    };
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 9; c++)
            if (px[r][c])
                tft.fillRect(x + c * scale, y + r * scale, scale, scale, C_ACC_DEEPSEEK);
}

typedef void (*SpriteDrawFn)(int16_t, int16_t, uint8_t);
struct ProviderStyle { uint16_t accent; SpriteDrawFn drawSprite; };

// Indexed by ProviderId — never index with PROV_NONE (callers only reach
// this table after classifyProvider() != PROV_NONE).
static const ProviderStyle PROVIDER_STYLES[] = {
    /* PROV_CLAUDE   */ { C_SPRITE,       drawClaudeSprite   },
    /* PROV_OPENAI   */ { C_ACC_OPENAI,   drawOpenAISprite   },
    /* PROV_CURSOR   */ { C_ACC_CURSOR,   drawCursorSprite   },
    /* PROV_DEEPSEEK */ { C_ACC_DEEPSEEK, drawDeepSeekSprite },
    /* PROV_CODEX    */ { C_ACC_CODEX,    drawCodexSprite    },
};

static void formatResetLine(char* buf, size_t len, uint32_t resetEpoch, uint32_t nowEpoch) {
    if (resetEpoch == 0 || resetEpoch <= nowEpoch) {
        strlcpy(buf, "Reset due", len);
        return;
    }
    uint32_t secs = resetEpoch - nowEpoch;
    if (secs >= 86400)
        snprintf(buf, len, "Resets in %ud %uh", (unsigned)(secs / 86400), (unsigned)((secs % 86400) / 3600));
    else if (secs >= 3600)
        snprintf(buf, len, "Resets in %uh %02um", (unsigned)(secs / 3600), (unsigned)((secs % 3600) / 60));
    else
        snprintf(buf, len, "Resets in %um %02us", (unsigned)(secs / 60), (unsigned)(secs % 60));
}

// Redraws just the reset-countdown line inside a usage card (partial
// update). show=false clears the line without drawing anything — used for
// Claude's plain-API-key "Rate Limit" fallback card, whose per-minute window
// has always expired again by the next fetch, so a reset line there would be
// permanently stale/misleading; every other card (Claude's 5h/7d, and any
// single-window card for the other 4 providers, whose resetEpoch values are
// real monthly/weekly cycles) keeps it.
// nextSyncEpoch>0 splits the line into "Resets in..." (left) / "Sync in..."
// (right) — only passed non-zero for the card an on-device probe with a real
// API key actually refreshes; a 7d/weekly card always passes 0 here since
// the daemon, not this device, owns that window.
static void tickCardReset(int16_t cardY, uint32_t resetEpoch, uint32_t nowEpoch, bool show, uint32_t nextSyncEpoch) {
    int16_t y = cardY + CARD_H - 15;
    tft.fillRect(CARD_X + 2, y - 8, CARD_W - 4, 16, C_BG);
    if (!show) return;
    char leftBuf[32];
    formatResetLine(leftBuf, sizeof(leftBuf), resetEpoch, nowEpoch);
    char rightBuf[24];
    formatSyncIn(rightBuf, sizeof(rightBuf), nextSyncEpoch, nowEpoch);
    tft.setTextFont(1);
    tft.setTextColor(C_SUBTEXT, C_BG);
    if (rightBuf[0]) {
        tft.setTextDatum(ML_DATUM);
        tft.drawString(leftBuf, CARD_X + 4, y);
        tft.setTextDatum(MR_DATUM);
        tft.drawString(rightBuf, CARD_X + CARD_W - 4, y);
    } else {
        tft.setTextDatum(MC_DATUM);
        tft.drawString(leftBuf, 120, y);
    }
}

// bigText/barPct/fillColor are preformatted by the caller so this one
// drawing routine covers every data shape (percentage, token count, dollar
// balance, or plain "0") across all 5 providers.
static void drawUsageCard(int16_t y, const char* label, const char* bigText, uint32_t barPct, uint16_t fillColor,
                           uint32_t resetEpoch, uint32_t nowEpoch, bool showReset, uint32_t nextSyncEpoch) {
    tft.fillRoundRect(CARD_X, y, CARD_W, CARD_H, 8, C_BG);
    tft.drawRoundRect(CARD_X, y, CARD_W, CARD_H, 8, C_CARD_BRD);

    // Row 1: big value text + pill badge
    tft.setTextDatum(ML_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString(bigText, CARD_X + 12, y + 20);

    int16_t pillW = 72, pillH = 18;
    int16_t pillX = CARD_X + CARD_W - pillW - 12, pillY = y + 11;
    tft.fillRoundRect(pillX, pillY, pillW, pillH, 9, C_PILL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(C_PILL_TXT, C_PILL_BG);
    tft.drawString(label, pillX + pillW / 2, pillY + pillH / 2 + 1);

    // Progress bar
    int16_t barX = CARD_X + 12, barY = y + 36, barW = CARD_W - 24, barH = 7;
    tft.fillRoundRect(barX, barY, barW, barH, 3, C_BAR_BG);
    uint32_t clamped = min((uint32_t)100, barPct);
    int16_t filled = (int16_t)(barW * clamped / 100);
    if (filled > 0) tft.fillRoundRect(barX, barY, filled, barH, 3, fillColor);

    // Reset line
    tickCardReset(y, resetEpoch, nowEpoch, showReset, nextSyncEpoch);
}

// Whether this agent's best/only data comes from the PC companion daemon
// (tools/usage-daemon.py), so a stopped daemon should surface a warning
// instead of silently going stale. Codex has no on-device fetch path at
// all, so it's always daemon-dependent; Claude/Cursor are daemon-dependent
// once keyless, OR once they've ever received dual-window daemon data
// (used7d/resetEpoch7d) — losing the daemon at that point degrades them
// back to a lesser (or no) on-device reading, which is worth flagging too.
static bool needsDaemon(ProviderId prov, const Agent* agent) {
    if (prov == PROV_CODEX) return true;
    if (prov != PROV_CLAUDE && prov != PROV_CURSOR) return false;
    return strlen(agent->apiKey) == 0 || agent->used7d > 0 || agent->resetEpoch7d > 0;
}

// Safety margin applied on top of whatever interval we're using as the
// "expected daemon cadence" — tolerates a missed cycle or two (slow API
// response, brief network hiccup) without a false "daemon down" warning.
#define DAEMON_STALE_MULTIPLIER  2.5f

// The threshold daemonIsStale() compares against. agent.syncIntervalSec
// means two different things depending on the agent (see storage.h): for a
// KEYLESS agent it doubles as the suggested --interval in the daemon
// command data/app.js's daemonCommandFor() prints for the user to copy —
// the closest on-device signal we have to what the daemon is actually
// running with, so honor it here. For a KEYED agent it's the on-device
// probe cadence instead, completely unrelated to the daemon's own timing —
// using it here would measure the wrong thing, so those always fall back
// to the flat DAEMON_STALE_SEC (2.5x the daemon's own 120s default).
static uint32_t daemonStaleThreshold(const Agent* agent) {
    if (strlen(agent->apiKey) == 0 && agent->syncIntervalSec > 0)
        return (uint32_t)(max((uint32_t)10, agent->syncIntervalSec) * DAEMON_STALE_MULTIPLIER);
    return DAEMON_STALE_SEC;
}

// True when the daemon has never pushed, or hasn't in over the threshold.
// Deliberately independent of lastSyncEpoch — see storage.h's comment on
// lastPushEpoch for why lastSyncEpoch alone can't detect this.
static bool daemonIsStale(const Agent* agent, uint32_t nowEpoch) {
    return agent->lastPushEpoch == 0 || (nowEpoch - agent->lastPushEpoch) > daemonStaleThreshold(agent);
}

// The line below the header: a "start the daemon" warning when this agent
// needs the daemon and it's gone quiet, otherwise (Claude only) the real
// last-used model + today's estimated cost — both populated ONLY by the PC
// daemon's /push (JSONL-derived), so showing them while the daemon is stale
// would just be presenting old data as current. Mirrors data/app.js's
// infoOrWarnLine(). Called from both renderCardUsage() (full draw) and
// tickCardUsage() (per-tick refresh), since staleness can flip purely from
// time passing, with no new /push event to trigger a full re-render.
static void drawInfoLine(const Agent* agent, uint32_t nowEpoch, ProviderId prov) {
    tft.fillRect(CARD_X, 50, CARD_W, 16, C_BG);

    if (needsDaemon(prov, agent) && daemonIsStale(agent, nowEpoch)) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(1);
        tft.setTextColor(C_ORANGE, C_BG);
        tft.drawString("Start the daemon for fresh data", 120, 58);
        return;
    }

    if (prov != PROV_CLAUDE) return;
    bool hasModel = (agent->model[0] != '\0');
    bool hasCost  = (agent->balance >= 0.0f);
    if (!hasModel && !hasCost) return;
    tft.setTextFont(1);
    if (hasModel) {
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(C_SUBTEXT, C_BG);
        tft.drawString(agent->model, CARD_X, 58);
    }
    if (hasCost) {
        char costBuf[24];
        snprintf(costBuf, sizeof(costBuf), "$%.2f today", agent->balance);
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(C_GREEN, C_BG);
        tft.drawString(costBuf, CARD_X + CARD_W, 58);
    }
}

static void renderCardUsage(const Agent* agent, uint32_t nowEpoch, ProviderId prov) {
    tft.fillScreen(C_BG);

    bool disabled = !agent->enabled;
    // "Never synced" (no data at all yet) is the real "Syncing" trigger now —
    // once there's ever been a real sync, just keep showing that last-known
    // data (a lapsed reset window no longer blanks the card back to "Syncing").
    bool neverSynced = (agent->lastSyncEpoch == 0);
    // The sprite + name header only means something once there's actual
    // usage data on screen — omit both for Disabled/Syncing so the header
    // doesn't show a logo/title for a screen with no usage to report yet.
    bool showHeader = !disabled && !neverSynced;
    bool isAnthropic = (prov == PROV_CLAUDE);

    if (showHeader) {
        // Header: sprite + agent name, centered as a group — matches the web
        // preview's <span class="disp-usage-title">${active.name}</span>
        // (data/app.js), which shows the real agent name, not a generic label.
        const int16_t spriteW = 36, spriteH = 20;
        tft.setTextFont(4);
        int16_t titleW = tft.textWidth(agent->name);
        int16_t groupW = spriteW + 8 + titleW;
        int16_t startX = (240 - groupW) / 2;

        PROVIDER_STYLES[prov].drawSprite(startX, 14, 4);
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString(agent->name, startX + spriteW + 8, 14 + spriteH / 2);
    }

    if (disabled) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(4);
        tft.setTextColor(C_SUBTEXT, C_BG);
        tft.drawString("Disabled", 120, 125);
        tft.setTextFont(2);
        tft.drawString("Auto-sync paused", 120, 150);
        return;
    }

    if (neverSynced) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(C_SUBTEXT, C_BG);
        tft.drawString("Syncing", 120, 125);
        tickSyncingDots(150);
        return;
    }

    drawInfoLine(agent, nowEpoch, prov);

    uint16_t accent = PROVIDER_STYLES[prov].accent;
    // Dual-window data (used7d/resetEpoch7d) is written generically by the PC
    // daemon's /push — Claude's Pro/Max subscription and Codex's primary+
    // secondary rate-limit windows both land here with no provider gate.
    bool has7d = (agent->used7d > 0 || agent->resetEpoch7d > 0);

    if (has7d) {
        uint32_t pct5h = min((uint32_t)100, agent->used);
        uint32_t pct7d = min((uint32_t)100, agent->used7d);
        char pctBuf1[8], pctBuf2[8];
        snprintf(pctBuf1, sizeof(pctBuf1), "%u%%", (unsigned)pct5h);
        snprintf(pctBuf2, sizeof(pctBuf2), "%u%%", (unsigned)pct7d);
        drawUsageCard(CARD1_Y, isAnthropic ? "5h" : "Current", pctBuf1, pct5h, barColor(pct5h),
                      agent->resetEpoch, nowEpoch, true, agent->nextSyncEpoch);
        // Card 2's nextSyncEpoch is deliberately 0 — the on-device probe
        // never touches 7d/weekly data, only the PC daemon does, so "Sync
        // in" wouldn't mean anything on this card.
        drawUsageCard(CARD2_Y, isAnthropic ? "7d" : "Weekly", pctBuf2, pct7d, barColor(pct7d),
                      agent->resetEpoch7d, nowEpoch, true, 0);
        return;
    }

    bool hasLimit   = agent->limit > 0;
    bool hasUsed    = agent->used > 0;
    bool hasBalance = agent->balance >= 0.0f;

    if (hasLimit) {
        // Cursor's real monthly-cycle window, or Claude's plain-API-key
        // per-minute-tier fallback (see tickCardReset()'s comment for why
        // Claude's reset/sync line is suppressed here but not for others).
        uint32_t pct = usedPct(agent->used, agent->limit);
        char pctBuf[8];
        snprintf(pctBuf, sizeof(pctBuf), "%u%%", (unsigned)pct);
        drawUsageCard(CARD1_Y, isAnthropic ? "Rate Limit" : "Monthly", pctBuf, pct, barColor(pct),
                      agent->resetEpoch, nowEpoch, !isAnthropic, isAnthropic ? 0 : agent->nextSyncEpoch);
    } else if (hasUsed) {
        // OpenAI: token count only, no limit. Bar is a cosmetic full accent
        // fill (no threshold coloring — there's no budget to measure against).
        char tokBuf[16];
        formatTokens(tokBuf, sizeof(tokBuf), agent->used);
        drawUsageCard(CARD1_Y, "Tokens", tokBuf, 100, accent,
                      agent->resetEpoch, nowEpoch, true, agent->nextSyncEpoch);
    } else if (hasBalance) {
        // DeepSeek: prepaid balance only, never sets resetEpoch — no reset
        // line to show.
        char balBuf[16];
        snprintf(balBuf, sizeof(balBuf), "$%.2f", agent->balance);
        drawUsageCard(CARD1_Y, "Balance", balBuf, 100, accent,
                      agent->resetEpoch, nowEpoch, false, 0);
    } else if (agent->resetEpoch > 0) {
        // A fetch already completed and legitimately found zero usage.
        drawUsageCard(CARD1_Y, "Used", "0", 0, accent,
                      agent->resetEpoch, nowEpoch, true, agent->nextSyncEpoch);
    }
}

static void tickCardUsage(const Agent* agent, uint32_t nowEpoch, ProviderId prov) {
    if (!agent->enabled) return; // static "Disabled" screen, nothing to animate
    if (agent->lastSyncEpoch == 0) { tickSyncingDots(150); return; }
    // Re-check every tick, not just on a full render — staleness can flip
    // purely from time passing, with no new /push event to trigger a redraw.
    drawInfoLine(agent, nowEpoch, prov);
    bool isAnthropic = (prov == PROV_CLAUDE);
    bool has7d = (agent->used7d > 0 || agent->resetEpoch7d > 0);
    if (has7d) {
        tickCardReset(CARD1_Y, agent->resetEpoch, nowEpoch, true, agent->nextSyncEpoch);
        tickCardReset(CARD2_Y, agent->resetEpoch7d, nowEpoch, true, 0);
        return;
    }
    bool hasLimit   = agent->limit > 0;
    bool hasUsed    = agent->used > 0;
    bool hasBalance = agent->balance >= 0.0f;
    if (hasLimit) {
        tickCardReset(CARD1_Y, agent->resetEpoch, nowEpoch, !isAnthropic, isAnthropic ? 0 : agent->nextSyncEpoch);
    } else if (hasUsed) {
        tickCardReset(CARD1_Y, agent->resetEpoch, nowEpoch, true, agent->nextSyncEpoch);
    } else if (hasBalance) {
        tickCardReset(CARD1_Y, agent->resetEpoch, nowEpoch, false, 0);
    } else if (agent->resetEpoch > 0) {
        tickCardReset(CARD1_Y, agent->resetEpoch, nowEpoch, true, agent->nextSyncEpoch);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void display_init() {
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, HIGH);

    tft.init();
    tft.setRotation(0);
    tft.fillScreen(C_BG);
}

void display_render(const Agent* agent) {
    if (!agent) {
        // e.g. after deleting the last/only active agent — token-tracker.local
        // (mDNS, started in main.cpp's setup()) works regardless of the
        // device's current DHCP-assigned IP, so it's a stable hint to show.
        tft.fillScreen(C_BG);
        display_renderIdle("token-tracker.local");
        return;
    }

    ProviderId prov = classifyProvider(agent->name);
    if (prov != PROV_NONE) {
        struct tm ti;
        uint32_t nowEpoch = getLocalTime(&ti) ? (uint32_t)mktime(&ti) : 0;
        renderCardUsage(agent, nowEpoch, prov);
        return;
    }

    tft.fillScreen(C_BG);

    struct tm ti;
    uint32_t nowEpoch = getLocalTime(&ti) ? (uint32_t)mktime(&ti) : 0;
    // A window whose reset time has already passed without a fresh sync
    // landing yet still holds real last-known numbers — once there's ever
    // been a real sync, just keep showing them instead of blanking back to
    // "Syncing" (neverSynced, from lastSyncEpoch, is the true "no data at
    // all yet" signal now; resetEpoch is only the provider's own window).
    bool neverSynced = (agent->lastSyncEpoch == 0);

    bool hasLimit   = !neverSynced && (agent->limit > 0);
    bool hasUsed    = !neverSynced && (agent->used > 0);
    bool hasBalance = !neverSynced && (agent->balance >= 0.0f);

    uint32_t pct  = usedPct(agent->used, agent->limit);
    bool warn = hasLimit && (pct >= WARN_THRESHOLD);

    // ── Header block ──────────────────────────────────────────────────────────
    uint16_t hdrBg = warn ? C_DARKRED : C_HEADER;
    tft.fillRect(0, 0, 240, HDR_H, hdrBg);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_TEXT, hdrBg);
    tft.setTextFont(4);
    tft.drawString(agent->name, 36, 18);

    tft.setTextFont(1);
    tft.setTextColor(C_SUBTEXT, hdrBg);
    tft.drawString(agent->model, 36, 38);

    drawWifiDot();
    drawPulseDot(warn);

    // ── Divider ───────────────────────────────────────────────────────────────
    tft.drawFastHLine(0, HDR_H, 240, warn ? C_RED : 0x4228);

    // ── Content — show whatever data is available ─────────────────────────────
    tft.setTextDatum(MC_DATUM);

    if (!agent->enabled) {
        tft.setTextFont(4);
        tft.setTextColor(C_SUBTEXT, C_BG);
        tft.drawString("Disabled", 120, 110);
        tft.setTextFont(2);
        tft.drawString("Auto-sync paused", 120, 135);
    } else if (hasLimit) {
        // Full mode: "X / Y tokens" + progress bar
        char usedBuf[16], limBuf[16];
        formatTokens(usedBuf, sizeof(usedBuf), agent->used);
        formatTokens(limBuf,  sizeof(limBuf),  agent->limit);
        char line[40];
        snprintf(line, sizeof(line), "%s / %s tokens", usedBuf, limBuf);
        tft.setTextFont(2);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString(line, 120, 82);
        drawProgressBar(agent->used, agent->limit);
        if (pct >= 100) {
            tft.setTextFont(4);
            tft.setTextColor(C_RED, C_BG);
            tft.drawString("LIMIT REACHED", 120, BAR_Y + BAR_H + 24);
        }
    } else if (hasUsed && hasBalance) {
        // Tokens used + balance (e.g. future providers)
        char usedBuf[16];
        formatTokens(usedBuf, sizeof(usedBuf), agent->used);
        char line[32];
        snprintf(line, sizeof(line), "Used: %s tokens", usedBuf);
        tft.setTextFont(2);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString(line, 120, 105);
        char balLine[28];
        snprintf(balLine, sizeof(balLine), "Balance: $%.2f", agent->balance);
        tft.setTextColor(C_GREEN, C_BG);
        tft.drawString(balLine, 120, 135);
    } else if (hasUsed) {
        // Only token count (e.g. OpenAI — no limit set)
        char usedBuf[16];
        formatTokens(usedBuf, sizeof(usedBuf), agent->used);
        char line[32];
        snprintf(line, sizeof(line), "Used: %s tokens", usedBuf);
        tft.setTextFont(4);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString(line, 120, 120);
    } else if (hasBalance) {
        // Only balance (e.g. DeepSeek prepaid)
        char balLine[28];
        snprintf(balLine, sizeof(balLine), "Balance: $%.2f", agent->balance);
        tft.setTextFont(4);
        tft.setTextColor(C_GREEN, C_BG);
        tft.drawString(balLine, 120, 120);
    } else if (!neverSynced && agent->resetEpoch > 0) {
        // A fetch already completed and legitimately found zero usage —
        // neverSynced is the real "has this agent ever synced" signal now
        // (lastSyncEpoch), matching the web preview's equivalent branch
        // instead of misleadingly showing "Syncing..." above a live reset
        // countdown.
        tft.setTextFont(4);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString("Used: 0", 120, 120);
    } else {
        // No data yet — show just "Syncing" + an animated loading indicator
        // (bouncing dots), advanced every tick by display_tick() below.
        tft.setTextFont(2);
        tft.setTextColor(C_SUBTEXT, C_BG);
        tft.drawString("Syncing", 120, 110);
        tickSyncingDots(135);
    }

    // ── Countdown placeholder — filled by display_tick ────────────────────────
    tft.fillRect(0, 190, 240, 22, C_BG);
}

void display_renderIdle(const char* ip) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 240, 56, C_HEADER);

    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(C_TEXT, C_HEADER);
    tft.drawString("Token Tracker", 120, 28);

    tft.setTextFont(4);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString("No active agent", 120, 115);

    tft.setTextFont(2);
    tft.setTextColor(C_ORANGE, C_BG);
    char urlBuf[32];
    snprintf(urlBuf, sizeof(urlBuf), "http://%s", ip);
    tft.drawString(urlBuf, 120, 160);
}

void display_tick(const Agent* agent, uint32_t nowEpoch) {
    if (!agent) return;
    if (!agent->enabled) return; // static "Disabled" screen, nothing to animate

    ProviderId prov = classifyProvider(agent->name);
    if (prov != PROV_NONE) {
        tickCardUsage(agent, nowEpoch, prov);
        return;
    }

    uint32_t pct  = usedPct(agent->used, agent->limit);
    bool warn = (agent->limit > 0) && (pct >= WARN_THRESHOLD);

    // Blink header background in warning mode
    if (warn) {
        _blinkState = !_blinkState;
        uint16_t hdrBg = _blinkState ? C_DARKRED : C_HEADER;
        tft.fillRect(0, 0, 240, HDR_H, hdrBg);

        // Redraw text over new background
        tft.setTextDatum(ML_DATUM);
        tft.setTextFont(4);
        tft.setTextColor(C_TEXT, hdrBg);
        tft.drawString(agent->name, 36, 18);
        tft.setTextFont(1);
        tft.setTextColor(C_SUBTEXT, hdrBg);
        tft.drawString(agent->model, 36, 38);
        drawWifiDot();
    }

    // Pulse dot
    drawPulseDot(warn);

    // Advance the "waiting for first data" bouncing dots — only while this
    // agent has never synced at all; once it has, keep showing last-known
    // data via the countdown below instead of these dots.
    bool neverSynced = (agent->lastSyncEpoch == 0);
    if (neverSynced) tickSyncingDots(135);

    bool nearReset = (agent->resetEpoch > 0 &&
                      agent->resetEpoch > nowEpoch &&
                      (agent->resetEpoch - nowEpoch) < (uint32_t)(WARN_HOURS * 3600));
    drawCountdown(agent->resetEpoch, nowEpoch, agent->nextSyncEpoch, 201, warn || nearReset);
}

// Palette restricted to orange / grey / black / white only — no green, red,
// cyan, or blue anywhere on this screen.
void display_renderWifiSetup(const char* apSsid) {
    tft.fillScreen(C_BG);

    // Header
    tft.fillRect(0, 0, 240, 50, C_HEADER);   // dark grey
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(C_TEXT, C_HEADER);
    tft.drawString("WiFi Setup", 120, 25);

    // Instructions
    tft.setTextFont(4);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString("Connect to:", 120, 75);

    tft.setTextFont(2);
    tft.setTextColor(C_ORANGE, C_BG);
    tft.drawString(apSsid, 120, 100);

    tft.setTextFont(4);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString("Then open:", 120, 140);

    tft.setTextFont(4);
    tft.setTextColor(C_ORANGE, C_BG);
    tft.drawString("192.168.4.1", 120, 175);

    tft.setTextFont(2);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString("Waiting for you", 120, 205);
}

// ─── Waiting-dots animation for the WiFi setup screen ────────────────────────
// Call periodically (e.g. every ANIM_INTERVAL_MS) while blocked waiting for
// the user to submit the setup form — same bouncing-dot language as the
// browser UI's loading indicator, so the two feel consistent.
static uint8_t _wifiDotPhase = 0;

void display_tickWifiSetup() {
    const int y = 222;
    const int cx[3] = {105, 120, 135};

    tft.fillRect(90, y - 8, 60, 16, C_BG); // erase previous frame
    for (int i = 0; i < 3; i++) {
        bool active = (i == _wifiDotPhase);
        tft.fillCircle(cx[i], y, active ? 5 : 3, active ? C_ORANGE : C_SUBTEXT);
    }
    _wifiDotPhase = (_wifiDotPhase + 1) % 3;
}

void display_renderConnecting(const char* ssid) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 240, 50, C_HEADER);

    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(C_TEXT, C_HEADER);
    tft.drawString("Connecting...", 120, 25);

    tft.setTextFont(2);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString(ssid, 120, 90);

    // What's about to happen, in order — a static preview list (this screen
    // has no visibility into main.cpp's setup() steps after WiFi actually
    // connects, so it's informational, not a live-ticking checklist).
    tft.setTextDatum(ML_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString("- Connecting to WiFi", 24, 126);
    tft.drawString("- Getting IP address", 24, 150);
    tft.drawString("- Starting web server", 24, 174);
}

// ─── Waiting-dots animation for the "Connecting..." screen ──────────────────
// Call periodically (e.g. every ANIM_INTERVAL_MS) while blocked waiting for
// WiFi.status() to become WL_CONNECTED — same bouncing-dot language as
// display_tickWifiSetup()/tickSyncingDots(), so every "waiting" screen on
// this device feels consistent.
static uint8_t _connectingDotPhase = 0;

void display_tickConnecting() {
    const int y = 205;
    const int cx[3] = {105, 120, 135};

    tft.fillRect(90, y - 8, 60, 16, C_BG); // erase previous frame
    for (int i = 0; i < 3; i++) {
        bool active = (i == _connectingDotPhase);
        tft.fillCircle(cx[i], y, active ? 5 : 3, active ? C_ORANGE : C_SUBTEXT);
    }
    _connectingDotPhase = (_connectingDotPhase + 1) % 3;
}
