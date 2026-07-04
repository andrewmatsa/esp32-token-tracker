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
#define C_WIFI      0x07FF          // cyan dot for WiFi indicator
#define C_SPRITE    0xDBAA          // Claude sprite terracotta (~#d97757)
#define C_PILL_BG   0x394A          // Claude usage pill background (~#3a2b52)
#define C_PILL_TXT  0xCD5F          // Claude usage pill text (~#c9a8ff)
#define C_CARD_BRD  0x2104          // Claude card border grey (~#262626)

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

// ─── Helpers ──────────────────────────────────────────────────────────────────

static uint16_t barColor(uint32_t pct) {
    if (pct >= 100) return C_RED;
    if (pct >= WARN_THRESHOLD) return C_ORANGE;
    return C_GREEN;
}

static void drawProgressBar(uint32_t used, uint32_t limit) {
    uint32_t pct = (limit > 0) ? min((uint32_t)100, used * 100 / limit) : 0;
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

static void drawCountdown(uint32_t resetEpoch, uint32_t nowEpoch, uint16_t y, bool warn) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);

    if (resetEpoch == 0) {
        tft.setTextColor(C_SUBTEXT, C_BG);
        tft.drawString("No reset date", 120, y);
        return;
    }

    if (nowEpoch >= resetEpoch) {
        tft.setTextColor(C_GREEN, C_BG);
        tft.drawString("Reset due", 120, y);
        return;
    }

    uint32_t secs = resetEpoch - nowEpoch;
    char buf[32];
    if (secs >= 86400) {
        uint32_t days = secs / 86400;
        uint32_t hrs  = (secs % 86400) / 3600;
        snprintf(buf, sizeof(buf), "Reset: %ud %uh", (unsigned)days, (unsigned)hrs);
    } else if (secs >= 3600) {
        uint32_t hrs  = secs / 3600;
        uint32_t mins = (secs % 3600) / 60;
        snprintf(buf, sizeof(buf), "Reset: %uh %02um", (unsigned)hrs, (unsigned)mins);
    } else {
        uint32_t mins = secs / 60;
        uint32_t s    = secs % 60;
        snprintf(buf, sizeof(buf), "Reset: %um %02us", (unsigned)mins, (unsigned)s);
    }

    tft.setTextColor(warn ? C_ORANGE : C_SUBTEXT, C_BG);
    // Clear line first to avoid ghost characters
    tft.fillRect(0, y - 10, 240, 20, C_BG);
    tft.drawString(buf, 120, y);
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

// ─── Claude "Usage" screen (5h / 7d rate-limit windows) ──────────────────────
// Mirrors the browser preview's watch-style layout in temporary.html.

#define CLAUDE_CARD_X   16
#define CLAUDE_CARD_W  208
#define CLAUDE_CARD_H   68
#define CLAUDE_CARD1_Y  48
#define CLAUDE_CARD2_Y 130

static bool isClaudeAgent(const char* name) {
    return strncasecmp(name, "claude", 6) == 0 || strncasecmp(name, "anthropic", 9) == 0;
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

static uint16_t claudeBarColor(uint32_t pct) {
    if (pct >= 85) return C_RED;
    if (pct >= 50) return C_ORANGE;
    return C_GREEN;
}

static void formatClaudeReset(char* buf, size_t len, uint32_t resetEpoch, uint32_t nowEpoch) {
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

// Redraws just the reset-countdown line inside a Claude usage card (partial update).
static void tickClaudeReset(int16_t cardY, uint32_t resetEpoch, uint32_t nowEpoch) {
    char buf[32];
    formatClaudeReset(buf, sizeof(buf), resetEpoch, nowEpoch);
    int16_t y = cardY + CLAUDE_CARD_H - 15;
    tft.fillRect(CLAUDE_CARD_X + 2, y - 8, CLAUDE_CARD_W - 4, 16, C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString(buf, 120, y);
}

static void drawClaudeCard(int16_t y, const char* label, uint32_t pct, uint32_t resetEpoch, uint32_t nowEpoch) {
    tft.fillRoundRect(CLAUDE_CARD_X, y, CLAUDE_CARD_W, CLAUDE_CARD_H, 8, C_BG);
    tft.drawRoundRect(CLAUDE_CARD_X, y, CLAUDE_CARD_W, CLAUDE_CARD_H, 8, C_CARD_BRD);

    // Row 1: percentage + pill badge
    tft.setTextDatum(ML_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(C_TEXT, C_BG);
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%u%%", (unsigned)pct);
    tft.drawString(pctBuf, CLAUDE_CARD_X + 12, y + 20);

    int16_t pillW = 72, pillH = 18;
    int16_t pillX = CLAUDE_CARD_X + CLAUDE_CARD_W - pillW - 12, pillY = y + 11;
    tft.fillRoundRect(pillX, pillY, pillW, pillH, 9, C_PILL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(C_PILL_TXT, C_PILL_BG);
    tft.drawString(label, pillX + pillW / 2, pillY + pillH / 2 + 1);

    // Progress bar
    int16_t barX = CLAUDE_CARD_X + 12, barY = y + 36, barW = CLAUDE_CARD_W - 24, barH = 7;
    tft.fillRoundRect(barX, barY, barW, barH, 3, C_BAR_BG);
    uint32_t clamped = min((uint32_t)100, pct);
    int16_t filled = (int16_t)(barW * clamped / 100);
    if (filled > 0) tft.fillRoundRect(barX, barY, filled, barH, 3, claudeBarColor(clamped));

    // Reset line
    tickClaudeReset(y, resetEpoch, nowEpoch);
}

static void renderClaudeUsage(const Agent* agent, uint32_t nowEpoch) {
    tft.fillScreen(C_BG);

    // Header: sprite + "Usage" title, centered as a group
    const int16_t spriteW = 36, spriteH = 20;
    tft.setTextFont(4);
    int16_t titleW = tft.textWidth("Usage");
    int16_t groupW = spriteW + 8 + titleW;
    int16_t startX = (240 - groupW) / 2;

    drawClaudeSprite(startX, 14, 4);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString("Usage", startX + spriteW + 8, 14 + spriteH / 2);

    uint32_t pct5h = min((uint32_t)100, agent->used);
    uint32_t pct7d = min((uint32_t)100, agent->used7d);

    drawClaudeCard(CLAUDE_CARD1_Y, "Current", pct5h, agent->resetEpoch, nowEpoch);
    drawClaudeCard(CLAUDE_CARD2_Y, "Weekly",  pct7d, agent->resetEpoch7d, nowEpoch);
}

static void tickClaudeUsage(const Agent* agent, uint32_t nowEpoch) {
    tickClaudeReset(CLAUDE_CARD1_Y, agent->resetEpoch, nowEpoch);
    tickClaudeReset(CLAUDE_CARD2_Y, agent->resetEpoch7d, nowEpoch);
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
        tft.fillScreen(C_BG);
        display_renderIdle("---");
        return;
    }

    if (isClaudeAgent(agent->name)) {
        struct tm ti;
        uint32_t nowEpoch = getLocalTime(&ti) ? (uint32_t)mktime(&ti) : 0;
        renderClaudeUsage(agent, nowEpoch);
        return;
    }

    tft.fillScreen(C_BG);

    bool hasLimit   = (agent->limit > 0);
    bool hasUsed    = (agent->used > 0);
    bool hasBalance = (agent->balance >= 0.0f);

    uint32_t pct  = hasLimit ? min((uint32_t)100, agent->used * 100 / agent->limit) : 0;
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

    if (hasLimit) {
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
    } else {
        // No data yet — show "Active" with dim note
        tft.setTextFont(4);
        tft.setTextColor(C_GREEN, C_BG);
        tft.drawString("Active", 120, 105);
        tft.setTextFont(2);
        tft.setTextColor(C_SUBTEXT, C_BG);
        tft.drawString("Syncing...", 120, 140);
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

    tft.setTextFont(2);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString("No active agent", 120, 120);

    tft.setTextFont(1);
    tft.setTextColor(C_WIFI, C_BG);
    char urlBuf[32];
    snprintf(urlBuf, sizeof(urlBuf), "http://%s", ip);
    tft.drawString(urlBuf, 120, 160);
}

void display_tick(const Agent* agent, uint32_t nowEpoch) {
    if (!agent) return;

    if (isClaudeAgent(agent->name)) {
        tickClaudeUsage(agent, nowEpoch);
        return;
    }

    uint32_t pct  = (agent->limit > 0)
        ? min((uint32_t)100, agent->used * 100 / agent->limit)
        : 0;
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

    // Countdown timer
    bool nearReset = (agent->resetEpoch > 0 &&
                      agent->resetEpoch > nowEpoch &&
                      (agent->resetEpoch - nowEpoch) < (uint32_t)(WARN_HOURS * 3600));
    drawCountdown(agent->resetEpoch, nowEpoch, 201, warn || nearReset);
}

void display_renderWifiSetup(const char* apSsid) {
    tft.fillScreen(C_BG);

    // Header
    tft.fillRect(0, 0, 240, 50, 0x0010);   // dark blue
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(C_TEXT, 0x0010);
    tft.drawString("WiFi Setup", 120, 25);

    // Instructions
    tft.setTextFont(2);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString("Connect to:", 120, 80);

    tft.setTextFont(2);
    tft.setTextColor(C_WIFI, C_BG);
    tft.drawString(apSsid, 120, 105);

    tft.setTextFont(2);
    tft.setTextColor(C_SUBTEXT, C_BG);
    tft.drawString("Then open:", 120, 145);

    tft.setTextFont(4);
    tft.setTextColor(C_GREEN, C_BG);
    tft.drawString("192.168.4.1", 120, 175);
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
    tft.drawString(ssid, 120, 100);

    // Static spinner chars — will be overwritten by repeated calls during connect loop
    static uint8_t frame = 0;
    const char* frames[] = {"[    ]", "[=   ]", "[==  ]", "[=== ]", "[====]", "[ ===]", "[  ==]", "[   =]"};
    tft.setTextFont(2);
    tft.setTextColor(C_WIFI, C_BG);
    tft.drawString(frames[frame % 8], 120, 140);
    frame++;
}
