# Usage daemon

One companion PC script (`usage-daemon.py`) that auto-fetches usage for
providers whose auth tokens either can't be typed into the device directly
(Codex's local login) or are easiest to read straight from the app's own
local storage (Cursor IDE). One process, one port for browser testing.

**Claude isn't covered here** — see step 3 below. It authenticates
on-device with a regular API key; there's no local credential file for a
daemon to read automatically the way there is for Cursor/Codex.

**Requires:** Python 3 (stdlib only — no `pip install`).

## 1. Find your device's IP

Open the token-tracker web UI — the IP is shown in the WiFi footer at the
bottom. (Or check the serial monitor output at boot.)

## 2. Find each agent's slot index

Agent slots are numbered by the order they were added in the web UI,
starting at 0. E.g. if you added Claude, Cursor, Codex in that order, their
indexes are `0`, `1`, `2`.

## 3. Add the agents with an empty API key (Codex) or a pasted key (Claude/Cursor)

- **Claude**: paste a **regular developer API key** (`sk-ant-api03-...`
  from console.anthropic.com) directly into the device's API key field —
  no daemon needed, the device syncs on its own. ⚠️ Anthropic disabled
  OAuth session tokens (`claude setup-token`) for third-party clients
  around February 2026 — `Authorization: Bearer <oauth-token>` now returns
  `"OAuth authentication is currently not supported"` regardless of header
  shape, so that path (and this daemon's old Claude support, which relied
  on it) no longer works. With a regular API key the device reports the
  account tier's per-minute token rate limit instead of the old Pro/Max
  5h/7d percentages (see `src/fetcher.cpp`'s `syncAnthropic()`).
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
python tools/usage-daemon.py --ip 192.168.1.50 --push cursor:1 codex:2
```

Only include the providers you've actually signed into. Use `--once` for a
single test cycle:

```bash
python tools/usage-daemon.py --ip 192.168.1.50 --push codex:2 --once
```

Options:

| Flag         | Default | Description                                              |
| ------------ | ------- | --------------------------------------------------------- |
| `--ip`       | —       | Device IP (required unless `--serve`)                      |
| `--push`     | —       | One or more `provider:index[:model]` entries (required unless `--serve`) |
| `--interval` | `120`   | Seconds between probe/push cycles                          |
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
GET http://127.0.0.1:8765/usage?provider=cursor[&model=gpt-4]
GET http://127.0.0.1:8765/usage?provider=codex[&model=gpt-4o]
```

(Claude has no bridge entry here since it's no longer a daemon provider —
`temporary.html`'s Claude preview still needs its own look at whether a
regular API key call needs the same direct-browser-access header/CORS
handling the old OAuth path required; not covered by this change.)

| Flag      | Default | Description                                                        |
| --------- | ------- | -------------------------------------------------------------------- |
| `--port`  | `8765`  | Local port to serve `GET /usage` on                                  |
| `--cache` | `60`    | Minimum seconds between real probes per provider; repeated requests within this window get the cached result |

## How each provider works

**Claude** — not a daemon provider (see step 3 above). Handled entirely
on-device by `src/fetcher.cpp`'s `syncAnthropic()`: a minimal 1-token
request to `POST /v1/messages` with the regular API key via `x-api-key`,
reading the standard `anthropic-ratelimit-tokens-limit` /
`-tokens-remaining` / `-tokens-reset` response headers (account tier's
per-minute rate limit — the OAuth-only Pro/Max 5h/7d "unified" headers
this used to read are no longer reachable from third-party clients).

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
