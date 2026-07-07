# Usage daemon

One companion PC script (`usage-daemon.py`) that auto-fetches usage for
providers whose auth tokens either can't be typed into the device directly
(Claude's and Codex's local logins) or are easiest to read straight from the
app's own local storage (Cursor IDE). One process, one port for browser
testing.

**Requires:** Python 3 (stdlib only — no `pip install`).

## 1. Find your device's IP

Open the token-tracker web UI — the IP is shown in the WiFi footer at the
bottom. (Or check the serial monitor output at boot.)

## 2. Find each agent's slot index

Agent slots are numbered by the order they were added in the web UI,
starting at 0. E.g. if you added Claude, Cursor, Codex in that order, their
indexes are `0`, `1`, `2`.

## 3. Add the agents with an empty API key (Claude/Codex) or a pasted key (Cursor)

- **Claude**: add a "Claude" agent and **leave the API key field empty** —
  the daemon supplies the usage. It reads the Claude Code *login* OAuth
  token from `~/.claude/.credentials.json` and reports the real Pro/Max
  **5h + 7d** subscription windows (see "How each provider works" below).
  Keep Claude Code signed in — it refreshes that token in the file as it
  runs; the daemon just reads the freshest copy each cycle.
  *(Alternatively, paste a regular developer API key `sk-ant-api03-...`
  for on-device sync with no daemon — but a plain API key only exposes the
  account tier's per-minute rate limit, one window, not the 5h/7d
  subscription percentages. See `src/fetcher.cpp`'s `syncAnthropic()`.)*
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
python tools/usage-daemon.py --push claude:0 cursor:1 codex:2
```

Only include the providers you've actually signed into. Use `--once` for a
single test cycle:

```bash
python tools/usage-daemon.py --push claude:0 --once
```

By default the daemon talks to `token-tracker.local` — the device's mDNS
name (advertised by the firmware itself, so it survives DHCP lease/IP
changes). If your OS can't resolve `.local` names (Windows without Bonjour
or its own mDNS support), pass a plain IP instead:

```bash
python tools/usage-daemon.py --ip 192.168.1.50 --push claude:0
```

Options:

| Flag         | Default               | Description                                              |
| ------------ | ---------------------- | --------------------------------------------------------- |
| `--ip`       | `token-tracker.local`  | Device address — IP or hostname                            |
| `--push`     | —                       | One or more `provider:index[:model]` entries (required unless `--serve`) |
| `--interval` | `100`                   | Seconds between probe/push cycles — **initial/fallback default only**, see below |
| `--once`     | off                     | Run a single cycle and exit                                |

### The web UI's "Update every (sec)" is live, not just a suggestion

`--interval` only applies until the daemon can reach the device. After each
push cycle, the daemon re-reads each target agent's own "Update every
(sec)" field (`GET /state`'s `syncInterval`) and sleeps for that instead —
so changing the value in the browser takes effect on the *next* cycle,
with no need to restart this process. If the device is unreachable that
cycle, or an agent's field is unset, `--interval` is used for that agent as
a fallback. Whatever value comes from the device is clamped to a minimum
of 60s regardless of what the web UI allows — a background loop hitting
the real provider API needs a higher floor than the field's own `min="10"`
(tuned for the on-device probe, not a perpetual daemon loop), or a low web
value would burn real rate-limit budget for no benefit.

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
GET http://127.0.0.1:8765/usage?provider=claude
GET http://127.0.0.1:8765/usage?provider=cursor[&model=gpt-4]
GET http://127.0.0.1:8765/usage?provider=codex[&model=gpt-4o]
```

(Claude is account-wide 5h + 7d — no `&model=` dimension.)

| Flag      | Default | Description                                                        |
| --------- | ------- | -------------------------------------------------------------------- |
| `--port`  | `8765`  | Local port to serve `GET /usage` on                                  |
| `--cache` | `60`    | Minimum seconds between real probes per provider; repeated requests within this window get the cached result |

## How each provider works

**Claude** — reads the Claude Code *login* OAuth token
(`claudeAiOauth.accessToken`) from `~/.claude/.credentials.json`, then makes
a minimal 1-token `POST /v1/messages` request that impersonates Claude Code:
`Authorization: Bearer <token>` plus `anthropic-beta: oauth-2025-04-20` and a
`User-Agent: claude-code/...` header (the server gates the subscription
headers on these). The response carries the account's real Pro/Max windows
via `anthropic-ratelimit-unified-5h-utilization` / `-5h-reset` /
`-7d-utilization` / `-7d-reset` (utilization is a `0..1` fraction → percent),
which the device shows as two cards. This is **not** the `claude setup-token`
OAuth flow Anthropic disabled for third-party clients (~Feb 2026) — that's a
different token; the interactive login token still works. No token refresh
logic here: Claude Code refreshes the token in the file as it runs, and the
daemon reads the freshest copy each cycle. If it expires (HTTP 401), the
daemon logs an error asking you to sign in to Claude Code again.
(For the alternative on-device path — a plain API key via `x-api-key`,
reporting only the per-minute `anthropic-ratelimit-tokens-*` window — see
`src/fetcher.cpp`'s `syncAnthropic()`.)

The daemon also scans local Claude Code JSONL transcripts
(`~/.claude/projects/**/*.jsonl`) to show the **most recently used model**
and an **estimated cost for today**. This cost is **not a real Pro/Max
charge** — Anthropic doesn't expose one, since the subscription is a flat
monthly fee, not pay-as-you-go. It's computed locally from today's actual
token counts (input/output/cache-read/cache-write, deduplicated by message
ID) at standard per-model API rates, purely as a spend-awareness estimate.

**Cursor** — reads `cursorAuth/accessToken` from Cursor IDE's local SQLite
state database (`state.vscdb`, path varies by OS), then
`GET https://api2.cursor.sh/auth/usage` with that token as a Bearer header
plus a browser-like `User-Agent` (Cloudflare otherwise rejects the request).
By default sums `numRequests`/`maxRequestUsage` across all model buckets in
the response (each response key is a model name, e.g. `"gpt-4"`); with a
model filter, only that one key's numbers are used. Reset = one month after
`startOfMonth`. This token has no Origin restriction, so it also works
pasted directly into the device (see `src/fetcher.cpp`'s `syncCursor()`,
which honors the same model filter via the agent's `model` field) — no
daemon required for that path.

**Codex** — reads `tokens.access_token`/`account_id` from Codex CLI's own
login file (`~/.codex/auth.json`). By default calls
`GET https://chatgpt.com/backend-api/wham/usage` (the same undocumented
endpoint Codex CLI itself uses) and reads `rate_limit.primary_window` /
`.secondary_window` (`used_percent`, `reset_after_seconds`) — a
dual-window shape, account-wide (no per-model breakdown exists on this
endpoint). With a model filter, it instead calls
`GET .../wham/usage/daily-token-usage-breakdown`, sums that model's
`credits` across this calendar month's per-day `models[]` entries, and
reports it as a plain used-count (no percentage/limit — same shape as
OpenAI's "no known limit" card). No on-device sync path exists for Codex
(the login lives only in a local file), so it's daemon/bridge-only either way.

## Notes / limitations

- No token-refresh logic — if a local token/login expires, re-run the
  relevant CLI's sign-in and the daemon will pick up the new one automatically
  (read fresh from disk every cycle).
- Both Cursor and Codex endpoints are unofficial/reverse-engineered — they
  can change without notice (as Claude's own OAuth-for-third-parties path
  already has, once).
- No systemd/launchd unit yet — run it in a terminal, tmux, or your own
  process supervisor.
- The `/push` endpoint on the device is unauthenticated (same trust model
  as the existing `/wifi/reset` endpoint) — only use this on a network you
  trust.
