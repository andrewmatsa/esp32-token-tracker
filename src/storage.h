#pragma once
#include <Arduino.h>
#include "config.h"

struct Agent {
    char     name[32];       // display name: "Claude", "Cursor", "Codex"
    char     model[48];      // Codex/Cursor: model id used as their filter ("gpt-4o", etc).
                             // Claude: real last-used model, written ONLY by the PC daemon's
                             // /push (tools/usage-daemon.py's JSONL scan) — empty if unknown.
                             // Never set by the web UI or the on-device probe.
    char     probeModel[48]; // Claude only: user-chosen rate-limit probe target (fetcher.cpp's
                             // syncAnthropic() sends this model in its /v1/messages probe body,
                             // since Anthropic's rate limits are model-specific). Set via the
                             // web UI's dropdown; unrelated to what the user is actually using.
    char     apiKey[512];    // stored on device, used for auto-fetch; never sent back to browser.
                             // 512, not 128 — Claude Code OAuth tokens (from `claude setup-token`)
                             // are long JWT-style strings, routinely 300-1000+ chars; a smaller
                             // buffer silently truncated them via strlcpy() in webserver.cpp,
                             // producing a corrupted token that Anthropic rejects with HTTP 401.
    uint32_t used;           // tokens consumed (auto-fetched when possible), or Claude's 5h-window %
    uint32_t limit;          // token budget (0 = unknown), or 100 for Claude's 5h-window %
    uint32_t resetEpoch;     // unix timestamp of next reset (auto-computed or 0)
    float    balance;        // credit balance in provider currency (-1 = not applicable)
    bool     active;         // only one agent is active at a time
    bool     enabled;        // false = excluded from auto-sync (fetchAll skips it)
    uint32_t used7d;         // Claude only: 7-day rate-limit window %, 0-100
    uint32_t resetEpoch7d;   // Claude only: unix timestamp of the 7-day window reset
    uint32_t syncIntervalSec; // Claude only: seconds between probes (0 = use FETCH_INTERVAL_MS default)
    uint32_t lastSyncEpoch;   // unix timestamp of last successful sync (0 = never synced).
                              // Distinct from resetEpoch (the provider's own window-reset
                              // time) — used to soften stale-window rendering: once a card
                              // has ever synced, a lapsed resetEpoch dims it instead of
                              // blanking it back to "Syncing".
    uint32_t nextSyncEpoch;  // unix timestamp of the next on-device fetchAll() attempt for
                             // this agent (0 = not scheduled — keyless/daemon-driven agents
                             // never get an on-device probe, so this stays 0 for them).
                             // EPHEMERAL: recomputed every fetchAll() cycle from
                             // agent.syncIntervalSec, never persisted to NVS (storage.cpp
                             // intentionally skips it) — it's scheduler state, not user data.
    uint32_t lastPushEpoch;  // unix timestamp of the last PC daemon /push for this agent
                             // (0 = never pushed). Distinct from lastSyncEpoch, which the
                             // on-device probe also stamps on every successful sync even when
                             // it's not allowed to overwrite used/used7d/etc (because daemon
                             // data already exists there) — that made lastSyncEpoch look
                             // "fresh" even when the daemon itself had stopped running, hiding
                             // real staleness. lastPushEpoch is written ONLY by onExternalPush().
                             // EPHEMERAL: never persisted to NVS, like nextSyncEpoch.
};

// Load all agents from NVS into the provided array. Returns count loaded.
int  storage_load(Agent agents[MAX_AGENTS]);

// Persist a single agent slot to NVS.
void storage_save(int index, const Agent& agent);

// Erase a single agent slot from NVS and zero-fill the struct.
void storage_delete(int index, Agent agents[MAX_AGENTS], int& count);

// Set one agent as active, clear active flag on all others, persist.
void storage_setActive(int index, Agent agents[MAX_AGENTS], int count);
