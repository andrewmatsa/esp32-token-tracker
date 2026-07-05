# Usage daemon

One companion PC script (`usage-daemon.py`) that auto-fetches usage for
providers whose auth tokens either can't be typed into the device directly
(Claude's OAuth token, Codex's local login) or are easiest to read straight
from the app's own local storage (Cursor IDE). One process, one port for
browser testing, all three providers.

**Requires:** Python 3 (stdlib only — no `pip install`).

## 1. Find your device's IP

Open the token-tracker web UI — the IP is shown in the WiFi footer at the
bottom. (Or check the serial monitor output at boot.)

## 2. Find each agent's slot index

Agent slots are numbered by the order they were added in the web UI,
starting at 0. E.g. if you added Claude, Cursor, Codex in that order, their
indexes are `0`, `1`, `2`.

## 3. Add the agents with an empty API key (Claude/Codex) or a pasted key (Cursor)

- **Claude**: add a "Claude" agent, leave the API key field empty — all its
  data comes from this daemon's pushes. **Alternative (no daemon/PC needed
  at all):** run `claude setup-token` once on any machine with the CLI
  logged in — this mints a long-lived OAuth token — and paste its output
  directly into the device's API key field instead. The device then syncs
  Claude on its own forever, the same way it already does for OpenAI/
  DeepSeek/Cursor. A regular `sk-ant-api03-...` developer API key does
  *not* work here — the device authenticates as an OAuth session
  specifically to read the Pro/Max plan's rate-limit headers.
- **Codex**: add a "Codex" agent — the API key field is disabled entirely
  (there's no key at all; usage comes from Codex CLI's own local login).
- **Cursor**: works either way — paste the access token directly into the
  agent's API key field for on-device sync (no daemon needed), or leave it
  empty and use this daemon instead.

All three show a "Model" field in the web UI (free text — bucket names are
account-specific) to filter to one model instead of the account-wide total;
leave it blank for the default behavior.

## 4. Run the daemon

```bash
python tools/usage-daemon.py --ip 192.168.1.50 --push claude:0 cursor:1 codex:2
```

Only include the providers you've actually signed into. Use `--once` for a
single test cycle:

```bash
python tools/usage-daemon.py --ip 192.168.1.50 --push claude:0 codex:2 --once
```

Options:

| Flag         | Default | Description                                              |
| ------------ | ------- | --------------------------------------------------------- |
| `--ip`       | —       | Device IP (required unless `--serve`)                      |
| `--push`     | —       | One or more `provider:index[:model]` entries (required unless `--serve`) |
| `--interval` | `120`   | Seconds between probe/push cycles                          |
| `--model`    | `claude-haiku-4-5` | Claude probe model                              |
| `--once`     | off     | Run a single cycle and exit                                |

### Filtering to one model (Cursor / Codex)

Both `--push` and the web UI's model field accept a specific model instead
of the account-wide total — e.g. `--push cursor:1:gpt-4` or
`--push codex:2:gpt-4o`. Leave it blank for the default (Cursor: sum of all
models; Codex: account-wide rate-limit windows). Model bucket names are
account-specific (whatever Cursor/Codex have actually billed you for) — check
a real response (`?provider=cursor` with no `&model=`, the daemon's error
message lists available keys) rather than guessing.

## Browser testing (bridge mode)

For `temporary.html` — some of these providers reject or CORS-block direct
browser calls — run a local bridge instead of pushing to a device. One
server, one port, routed by `?provider=`:

```bash
python tools/usage-daemon.py --serve
```

```
GET http://127.0.0.1:8765/usage?provider=claude[&model=claude-haiku-4-5]
GET http://127.0.0.1:8765/usage?provider=cursor[&model=gpt-4]
GET http://127.0.0.1:8765/usage?provider=codex[&model=gpt-4o]
```

| Flag      | Default | Description                                                        |
| --------- | ------- | -------------------------------------------------------------------- |
| `--port`  | `8765`  | Local port to serve `GET /usage` on                                  |
| `--cache` | `60`    | Minimum seconds between real probes per provider; repeated requests within this window get the cached result |

## How each provider works

**Claude** — sends a minimal 1-token request to `POST /v1/messages`
(near-zero cost) purely to read the `anthropic-ratelimit-unified-5h-*` /
`-7d-*` response headers. Token read fresh each cycle from Claude Code's
local credential storage (`~/.claude/.credentials.json`, or macOS Keychain).

**Cursor** — reads `cursorAuth/accessToken` from Cursor IDE's local SQLite
state database (`state.vscdb`, path varies by OS), then
`GET https://api2.cursor.sh/auth/usage` with that token as a Bearer header
plus a browser-like `User-Agent` (Cloudflare otherwise rejects the request).
By default sums `numRequests`/`maxRequestUsage` across all model buckets in
the response (each response key is a model name, e.g. `"gpt-4"`); with a
model filter, only that one key's numbers are used. Reset = one month after
`startOfMonth`. Unlike Claude, this token has no Origin restriction, so it
also works pasted directly into the device (see `src/fetcher.cpp`'s
`syncCursor()`, which honors the same model filter via the agent's `model`
field) — no daemon required for that path.

**Codex** — reads `tokens.access_token`/`account_id` from Codex CLI's own
login file (`~/.codex/auth.json`). By default calls
`GET https://chatgpt.com/backend-api/wham/usage` (the same undocumented
endpoint Codex CLI itself uses) and reads `rate_limit.primary_window` /
`.secondary_window` (`used_percent`, `reset_after_seconds`) — same
two-window shape as Claude's 5h/7d, account-wide (no per-model breakdown
exists on this endpoint). With a model filter, it instead calls
`GET .../wham/usage/daily-token-usage-breakdown`, sums that model's
`credits` across this calendar month's per-day `models[]` entries, and
reports it as a plain used-count (no percentage/limit — same shape as
OpenAI's "no known limit" card). No on-device sync path exists for Codex
(the login lives only in a local file), so it's daemon/bridge-only either way.

## Notes / limitations

- No token-refresh logic — if a local token/login expires, re-run the
  relevant CLI's sign-in and the daemon will pick up the new one automatically
  (read fresh from disk every cycle).
- All three endpoints are unofficial/reverse-engineered (except Claude's
  documented rate-limit headers) — they can change without notice.
- No systemd/launchd unit yet — run it in a terminal, tmux, or your own
  process supervisor.
- The `/push` endpoint on the device is unauthenticated (same trust model
  as the existing `/wifi/reset` endpoint) — only use this on a network you
  trust.
