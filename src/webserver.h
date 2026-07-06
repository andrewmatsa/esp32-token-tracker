#pragma once
#include "storage.h"

typedef void (*OnAgentUpdate)(int index, const Agent& agent);
typedef void (*OnSetActive)(int index);
typedef void (*OnSetEnabled)(int index, bool enabled);
typedef void (*OnDelete)(int index);
typedef void (*OnExternalPush)(int index, uint32_t used, uint32_t limit, uint32_t resetEpoch,
                               uint32_t used7d, uint32_t resetEpoch7d);

// Initialize the HTTP server (REST /command + /state, /push, /wifi/*, static
// files). SPIFFS must already be mounted. The browser polls GET /state, so
// there's no server-initiated push and nothing to service from loop().
void webserver_init(Agent agents[MAX_AGENTS], int* count,
                    OnAgentUpdate  cbUpdate,
                    OnSetActive    cbActive,
                    OnSetEnabled   cbEnabled,
                    OnDelete       cbDelete,
                    OnExternalPush cbPush);
