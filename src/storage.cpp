#include "storage.h"
#include <Preferences.h>

// NVS namespace
static Preferences prefs;

// Build a short NVS key: e.g. "ag2used" (max 15 chars)
static void makeKey(char* buf, int idx, const char* field) {
    snprintf(buf, 16, "ag%d%s", idx, field);
}

int storage_load(Agent agents[MAX_AGENTS]) {
    prefs.begin("ttracker", true);
    int count = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        char key[16];
        makeKey(key, i, "name");
        if (!prefs.isKey(key)) continue;

        prefs.getString(key, agents[count].name, sizeof(agents[count].name));

        makeKey(key, i, "model");
        prefs.getString(key, agents[count].model, sizeof(agents[count].model));

        makeKey(key, i, "pmodel");
        prefs.getString(key, agents[count].probeModel, sizeof(agents[count].probeModel));

        makeKey(key, i, "key");
        prefs.getString(key, agents[count].apiKey, sizeof(agents[count].apiKey));

        makeKey(key, i, "used");
        agents[count].used = prefs.getUInt(key, 0);

        makeKey(key, i, "limit");
        agents[count].limit = prefs.getUInt(key, 0);

        makeKey(key, i, "reset");
        agents[count].resetEpoch = prefs.getUInt(key, 0);

        makeKey(key, i, "bal");
        agents[count].balance = prefs.getFloat(key, -1.0f);

        makeKey(key, i, "active");
        agents[count].active = prefs.getBool(key, false);

        makeKey(key, i, "en");
        agents[count].enabled = prefs.getBool(key, true);

        makeKey(key, i, "u7d");
        agents[count].used7d = prefs.getUInt(key, 0);

        makeKey(key, i, "r7d");
        agents[count].resetEpoch7d = prefs.getUInt(key, 0);

        makeKey(key, i, "ivl");
        agents[count].syncIntervalSec = prefs.getUInt(key, 0);

        count++;
    }
    prefs.end();
    return count;
}

void storage_save(int index, const Agent& agent) {
    prefs.begin("ttracker", false);
    char key[16];

    makeKey(key, index, "name");   prefs.putString(key, agent.name);
    makeKey(key, index, "model");  prefs.putString(key, agent.model);
    makeKey(key, index, "pmodel"); prefs.putString(key, agent.probeModel);
    makeKey(key, index, "key");    prefs.putString(key, agent.apiKey);
    makeKey(key, index, "used");   prefs.putUInt(key, agent.used);
    makeKey(key, index, "limit");  prefs.putUInt(key, agent.limit);
    makeKey(key, index, "reset");  prefs.putUInt(key, agent.resetEpoch);
    makeKey(key, index, "bal");    prefs.putFloat(key, agent.balance);
    makeKey(key, index, "active"); prefs.putBool(key, agent.active);
    makeKey(key, index, "en");     prefs.putBool(key, agent.enabled);
    makeKey(key, index, "u7d");    prefs.putUInt(key, agent.used7d);
    makeKey(key, index, "r7d");    prefs.putUInt(key, agent.resetEpoch7d);
    makeKey(key, index, "ivl");    prefs.putUInt(key, agent.syncIntervalSec);

    prefs.end();
}

void storage_delete(int index, Agent agents[MAX_AGENTS], int& count) {
    // Remove from NVS
    prefs.begin("ttracker", false);
    char key[16];
    const char* fields[] = {"name","model","pmodel","key","used","limit","reset","bal","active","en","u7d","r7d","ivl"};
    for (auto f : fields) {
        makeKey(key, index, f);
        prefs.remove(key);
    }
    prefs.end();

    // Shift array left
    for (int i = index; i < count - 1; i++) {
        agents[i] = agents[i + 1];
        storage_save(i, agents[i]);
    }
    memset(&agents[count - 1], 0, sizeof(Agent));
    count--;
}

void storage_setActive(int index, Agent agents[MAX_AGENTS], int count) {
    for (int i = 0; i < count; i++) {
        agents[i].active = (i == index);
        storage_save(i, agents[i]);
    }
}
