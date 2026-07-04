#pragma once
#include "storage.h"

// Fetch current usage/balance from the provider API using the stored API key.
// Updates agent.used, agent.balance, and agent.resetEpoch where available.
// Returns true if any data was successfully retrieved.
bool fetcher_sync(Agent& agent);
