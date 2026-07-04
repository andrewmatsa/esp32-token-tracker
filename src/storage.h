#pragma once
#include <Arduino.h>
#include "config.h"

struct Agent {
    char     name[32];       // display name: "Claude", "Cursor", "Codex"
    char     model[48];      // model id: "claude-opus-4-8", "gpt-4o"; for Claude, user-editable probe model
    char     apiKey[128];    // stored on device, used for auto-fetch; never sent back to browser
    uint32_t used;           // tokens consumed (auto-fetched when possible), or Claude's 5h-window %
    uint32_t limit;          // token budget (0 = unknown), or 100 for Claude's 5h-window %
    uint32_t resetEpoch;     // unix timestamp of next reset (auto-computed or 0)
    float    balance;        // credit balance in provider currency (-1 = not applicable)
    bool     active;         // only one agent is active at a time
    bool     enabled;        // false = excluded from auto-sync (fetchAll skips it)
    uint32_t used7d;         // Claude only: 7-day rate-limit window %, 0-100
    uint32_t resetEpoch7d;   // Claude only: unix timestamp of the 7-day window reset
    uint32_t syncIntervalSec; // Claude only: seconds between probes (0 = use FETCH_INTERVAL_MS default)
};

// Load all agents from NVS into the provided array. Returns count loaded.
int  storage_load(Agent agents[MAX_AGENTS]);

// Persist a single agent slot to NVS.
void storage_save(int index, const Agent& agent);

// Erase a single agent slot from NVS and zero-fill the struct.
void storage_delete(int index, Agent agents[MAX_AGENTS], int& count);

// Set one agent as active, clear active flag on all others, persist.
void storage_setActive(int index, Agent agents[MAX_AGENTS], int count);
