#!/usr/bin/env python3
"""
Unified usage daemon for token-tracker (Claude, Cursor, Codex).

Each provider stores its own login/token locally and exposes usage via its
own probe. This script reads whichever ones you configure, on one process,
one port — instead of running a separate script per provider:

  - Claude: reads the Claude Code *login* OAuth token from
    ~/.claude/.credentials.json and probes /v1/messages for the real
    Pro/Max 5h + 7d subscription windows (unified rate-limit headers).
    This is the interactive-login token, NOT the `claude setup-token`
    flow Anthropic disabled for third-party clients (~Feb 2026) — that's
    a different token. Requires impersonating Claude Code (User-Agent +
    oauth-2025-04-20 beta header). Same approach as Clawdmeter.
  - Cursor: reads cursorAuth/accessToken from Cursor IDE's SQLite state db,
    probes /auth/usage.
  - Codex: reads tokens from Codex CLI's auth.json, probes /wham/usage.

Stdlib only — no pip install required.

Push mode — pushes each configured provider to its own agent slot on the
device, in a single shared loop. Each --push entry is provider:index, or
provider:index:model to filter to one model bucket instead of the
account-wide total (Claude ignores the model filter):
    python usage-daemon.py --ip 192.168.1.50 --push claude:0 cursor:1 codex:2:gpt-4o [--interval 120] [--once]

Bridge mode — one local server for browser-based testing (temporary.html),
serving every provider on one port via ?provider=:
    python usage-daemon.py --serve [--port 8765] [--cache 60]
    GET http://127.0.0.1:8765/usage?provider=claude
    GET http://127.0.0.1:8765/usage?provider=cursor[&model=gpt-4]
    GET http://127.0.0.1:8765/usage?provider=codex[&model=gpt-4o]

Claude: account-wide 5h + 7d windows, no model dimension (?model= ignored).
Cursor: ?model= filters to one model key from /auth/usage (e.g. "gpt-4") —
omit it to sum all models (the default). Codex: ?model= switches from the
account-wide rate-limit windows to that model's monthly credit total from
the daily usage breakdown — omit it to keep the default rate-limit view.

Find the device IP via its web UI footer, or the serial monitor on boot.
Find each agent's slot index by its position in the device's agent list
(0-based, in the order agents were added).
"""

import argparse
import json
import os
import platform
import re
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer

BROWSER_USER_AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")

# Claude's OAuth rate-limit probe only returns the Pro/Max "unified" 5h/7d
# windows when the request looks like it came from Claude Code itself — the
# server gates the anthropic-ratelimit-unified-* headers on a claude-code
# User-Agent plus the oauth-2025-04-20 beta header.
CLAUDE_CODE_USER_AGENT = "claude-code/2.1.5"


def log(msg: str) -> None:
    print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}", flush=True)


# ─── Cursor ───────────────────────────────────────────────────────────────

def cursor_state_db_path() -> str:
    system = platform.system()
    if system == "Windows":
        return os.path.expandvars(r"%APPDATA%\Cursor\User\globalStorage\state.vscdb")
    if system == "Darwin":
        return os.path.expanduser("~/Library/Application Support/Cursor/User/globalStorage/state.vscdb")
    return os.path.expanduser("~/.config/Cursor/User/globalStorage/state.vscdb")


def cursor_read_token() -> str:
    path = cursor_state_db_path()
    if not os.path.exists(path):
        raise RuntimeError(f"Cursor state database not found at {path}. Sign in to Cursor IDE first.")
    conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        cur = conn.cursor()
        cur.execute("SELECT value FROM ItemTable WHERE key = 'cursorAuth/accessToken' LIMIT 1")
        row = cur.fetchone()
    finally:
        conn.close()
    if not row or not row[0]:
        raise RuntimeError(f"No Cursor access token found in {path}.")
    return row[0]


def cursor_probe(model: str = None) -> dict:
    """If `model` is given, only that response key's numRequests/maxRequestUsage
    is used (e.g. "gpt-4") instead of summing every model bucket."""
    token = cursor_read_token()
    req = urllib.request.Request(
        "https://api2.cursor.sh/auth/usage", method="GET",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "User-Agent": BROWSER_USER_AGENT,  # Cloudflare rejects the default urllib UA
        },
    )
    try:
        resp = urllib.request.urlopen(req, timeout=15)
        data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            raise RuntimeError(f"Token rejected (HTTP {e.code}) — sign in to Cursor IDE again")
        raise RuntimeError(f"HTTP {e.code}: {e.read().decode(errors='replace')}")

    total_used = 0
    total_limit = 0
    limit_known = True

    if model:
        val = data.get(model)
        if not isinstance(val, dict):
            raise RuntimeError(f"No usage recorded for model '{model}' this month "
                                f"(available: {[k for k in data if k != 'startOfMonth']})")
        total_used = val.get("numRequests") or 0
        max_usage = val.get("maxRequestUsage")
        limit_known = max_usage is not None
        total_limit = max_usage or 0
    else:
        for key, val in data.items():
            if key == "startOfMonth" or not isinstance(val, dict):
                continue
            total_used += val.get("numRequests") or 0
            max_usage = val.get("maxRequestUsage")
            if max_usage is None:
                limit_known = False
            else:
                total_limit += max_usage

    reset_epoch = 0
    start_of_month = data.get("startOfMonth")
    if start_of_month:
        try:
            start_dt = datetime.fromisoformat(start_of_month.replace("Z", "+00:00"))
            month = start_dt.month % 12 + 1
            year = start_dt.year + (1 if start_dt.month == 12 else 0)
            reset_epoch = int(start_dt.replace(year=year, month=month).timestamp())
        except ValueError:
            pass

    # Cursor has no fixed model catalog either — report every model bucket
    # key actually billed this month (sorted by requests used), the same
    # best-effort "detected models" list used for Codex.
    models = sorted(
        (k for k, v in data.items() if k != "startOfMonth" and isinstance(v, dict)),
        key=lambda k: data[k].get("numRequests") or 0,
        reverse=True,
    )

    return {
        "used": total_used,
        "limit": total_limit if limit_known and total_limit > 0 else 0,
        "resetEpoch": reset_epoch,
        "model": models[0] if models else "",
        "models": models,
    }


# ─── Codex ────────────────────────────────────────────────────────────────

def codex_home() -> str:
    return os.environ.get("CODEX_HOME") or os.path.expanduser("~/.codex")


def codex_read_auth() -> tuple:
    path = os.path.join(codex_home(), "auth.json")
    if not os.path.exists(path):
        raise RuntimeError(f"Codex auth file not found at {path}. Sign in with `codex` first.")
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    tokens = data.get("tokens") or {}
    access_token = tokens.get("access_token")
    account_id = tokens.get("account_id")
    if not access_token or not account_id:
        raise RuntimeError(f"Missing access_token/account_id in {path}. Sign in with `codex` again.")
    return access_token, account_id


def _get_nested(obj, *keys):
    cur = obj
    for key in keys:
        if not isinstance(cur, dict):
            return None
        cur = cur.get(key)
    return cur


def _codex_request(path: str) -> dict:
    access_token, account_id = codex_read_auth()
    req = urllib.request.Request(
        f"https://chatgpt.com/backend-api{path}", method="GET",
        headers={
            "Authorization": f"Bearer {access_token}",
            "ChatGPT-Account-ID": account_id,
            "originator": "Codex Desktop",
            "User-Agent": "codex-usage-local-script/3.0",
        },
    )
    try:
        resp = urllib.request.urlopen(req, timeout=15)
        return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            raise RuntimeError(f"Auth rejected (HTTP {e.code}) — sign in with `codex` again")
        raise RuntimeError(f"HTTP {e.code}: {e.read().decode(errors='replace')}")


def codex_detect_models() -> list:
    """Return this month's model names actually seen in the account's usage
    breakdown, sorted by credits used (descending) — there is no single
    'current model' setting for Codex, so this is the closest available
    substitute for a default/autocomplete list."""
    data = _codex_request("/wham/usage/daily-token-usage-breakdown")
    rows = data.get("data") if isinstance(data.get("data"), list) else []

    now = datetime.now()
    totals: dict = {}
    for row in rows:
        date_str = row.get("date") or ""
        if not date_str.startswith(f"{now.year:04d}-{now.month:02d}"):
            continue
        for entry in row.get("models") or []:
            name = entry.get("model")
            if name:
                totals[name] = totals.get(name, 0) + (entry.get("credits") or 0)

    return [name for name, _ in sorted(totals.items(), key=lambda kv: kv[1], reverse=True)]


def codex_probe_rate_limit() -> dict:
    """Account-wide rate-limit windows (no per-model breakdown available here)."""
    usage = _codex_request("/wham/usage")

    primary_pct = _get_nested(usage, "rate_limit", "primary_window", "used_percent") or 0
    primary_reset = _get_nested(usage, "rate_limit", "primary_window", "reset_after_seconds") or 0
    secondary_pct = _get_nested(usage, "rate_limit", "secondary_window", "used_percent") or 0
    secondary_reset = _get_nested(usage, "rate_limit", "secondary_window", "reset_after_seconds") or 0

    models = codex_detect_models()

    now = int(time.time())
    return {
        "used": max(0, min(100, round(primary_pct))),
        "limit": 100,
        "resetEpoch": now + int(primary_reset),
        "used7d": max(0, min(100, round(secondary_pct))),
        "resetEpoch7d": now + int(secondary_reset),
        "model": models[0] if models else "",
        "models": models,
    }


def codex_probe_model_credits(model: str) -> dict:
    """Sum this calendar month's `credits` for one model from the daily
    per-model breakdown (the rate-limit endpoint has no model dimension).
    Also reports every model name seen this month (`models`), reusing the
    same response, so the caller can populate a model picker."""
    data = _codex_request("/wham/usage/daily-token-usage-breakdown")
    rows = data.get("data") if isinstance(data.get("data"), list) else []

    now = datetime.now()
    total_credits = 0.0
    totals: dict = {}
    for row in rows:
        date_str = row.get("date") or ""
        if not date_str.startswith(f"{now.year:04d}-{now.month:02d}"):
            continue
        for entry in row.get("models") or []:
            name = entry.get("model")
            credits = entry.get("credits") or 0
            if name:
                totals[name] = totals.get(name, 0) + credits
            if name == model:
                total_credits += credits

    models = [name for name, _ in sorted(totals.items(), key=lambda kv: kv[1], reverse=True)]
    reset_dt = datetime(now.year + 1, 1, 1) if now.month == 12 else datetime(now.year, now.month + 1, 1)
    return {"used": round(total_credits), "limit": 0, "resetEpoch": int(reset_dt.timestamp()), "models": models}


def codex_probe(model: str = None) -> dict:
    if model:
        return codex_probe_model_credits(model)
    return codex_probe_rate_limit()


# ─── Claude ───────────────────────────────────────────────────────────────
# Reads the Claude Code *login* OAuth token (claudeAiOauth.accessToken) from
# ~/.claude/.credentials.json and makes a 1-token probe to /v1/messages. The
# response carries the account's real Pro/Max subscription usage via the
# anthropic-ratelimit-unified-5h/7d-utilization headers (fraction 0..1).
#
# This is NOT the `claude setup-token` OAuth flow that Anthropic disabled for
# third-party clients (~Feb 2026) — that's a different token. The interactive
# Claude Code login token still works, provided the request impersonates
# Claude Code (User-Agent + oauth-2025-04-20 beta header). Same approach as
# Clawdmeter (https://github.com/HermannBjorgvin/Clawdmeter).
#
# Claude Code refreshes this token in the file as it runs; we just read the
# freshest copy each cycle. No refresh logic here — if it expires (401), the
# user re-runs Claude Code.

def claude_credentials_path() -> str:
    # macOS Claude Code stores the token in the Keychain (service
    # "Claude Code-credentials"), not this file — TODO if macOS support is
    # needed. The file path covers Windows and Linux.
    return os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json")


def claude_read_token() -> str:
    path = claude_credentials_path()
    if not os.path.exists(path):
        raise RuntimeError(f"Claude credentials not found at {path}. Sign in to Claude Code first.")
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    token = _get_nested(data, "claudeAiOauth", "accessToken")
    if not token:
        raise RuntimeError(f"No claudeAiOauth.accessToken in {path}. Sign in to Claude Code again.")
    return token


# Anthropic doesn't expose a real dollar amount for Pro/Max subscription
# usage (it's a flat monthly fee, not pay-as-you-go) — so "billing" here is
# an *estimate* of what today's tokens would cost at standard API rates,
# computed locally from Claude Code's own JSONL transcripts. $ per 1M tokens
# (input, output); unmatched/unknown models fall back to the sonnet-5 rate
# as an approximation (noted, not exact).
CLAUDE_PRICING = {
    "claude-fable-5":     (10.0, 50.0),
    "claude-mythos-5":    (10.0, 50.0),
    "claude-opus-4-8":    (5.0, 25.0),
    "claude-opus-4-7":    (5.0, 25.0),
    "claude-opus-4-6":    (5.0, 25.0),
    "claude-opus-4-5":    (5.0, 25.0),
    "claude-sonnet-5":    (3.0, 15.0),
    "claude-sonnet-4-6":  (3.0, 15.0),
    "claude-sonnet-4-5":  (3.0, 15.0),
    "claude-haiku-4-5":   (1.0, 5.0),
}
CLAUDE_PRICING_FALLBACK = (3.0, 15.0)  # sonnet-5 rate — approximation for unrecognized models

CACHE_WRITE_5M_MULT = 1.25
CACHE_WRITE_1H_MULT = 2.0
CACHE_READ_MULT     = 0.1


def claude_projects_dir() -> str:
    return os.path.join(os.path.expanduser("~"), ".claude", "projects")


def _claude_pricing_for(model: str):
    if model in CLAUDE_PRICING:
        return CLAUDE_PRICING[model]
    # Strip a trailing dated-snapshot suffix like "-20251001" and retry.
    stripped = re.sub(r"-\d{8}$", "", model)
    if stripped in CLAUDE_PRICING:
        return CLAUDE_PRICING[stripped]
    return CLAUDE_PRICING_FALLBACK


def claude_scan_usage_today() -> dict:
    """Scans local Claude Code JSONL transcripts for: the most recently used
    model (regardless of date) and an estimated USD cost for today's tokens
    (local calendar day), broken out by model using official per-token rates.
    Lines are deduplicated by message.id — the same message can appear on
    multiple JSONL rows as it streams to disk."""
    root = claude_projects_dir()
    if not os.path.isdir(root):
        return {"model": "", "costUSD": 0.0}

    cutoff_mtime = time.time() - 2 * 86400
    today = datetime.now().astimezone().date()

    seen_ids = set()
    last_ts = None
    last_model = ""
    today_totals: dict = {}  # model -> {"input":.., "output":.., "cache_5m":.., "cache_1h":.., "cache_read":..}

    for dirpath, _dirs, files in os.walk(root):
        for fname in files:
            if not fname.endswith(".jsonl"):
                continue
            fpath = os.path.join(dirpath, fname)
            try:
                if os.path.getmtime(fpath) < cutoff_mtime:
                    continue
            except OSError:
                continue
            try:
                with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            row = json.loads(line)
                        except ValueError:
                            continue
                        message = row.get("message") or {}
                        usage = message.get("usage")
                        model = message.get("model")
                        msg_id = message.get("id")
                        ts = row.get("timestamp")
                        if not usage or not model or not msg_id or not ts:
                            continue
                        if msg_id in seen_ids:
                            continue
                        seen_ids.add(msg_id)

                        try:
                            dt = datetime.fromisoformat(ts.replace("Z", "+00:00"))
                        except ValueError:
                            continue

                        if last_ts is None or dt > last_ts:
                            last_ts = dt
                            last_model = model

                        if dt.astimezone().date() != today:
                            continue

                        bucket = today_totals.setdefault(model, {
                            "input": 0, "output": 0, "cache_5m": 0, "cache_1h": 0, "cache_read": 0,
                        })
                        bucket["input"]  += usage.get("input_tokens") or 0
                        bucket["output"] += usage.get("output_tokens") or 0
                        bucket["cache_read"] += usage.get("cache_read_input_tokens") or 0
                        creation = usage.get("cache_creation")
                        if isinstance(creation, dict):
                            bucket["cache_5m"] += creation.get("ephemeral_5m_input_tokens") or 0
                            bucket["cache_1h"] += creation.get("ephemeral_1h_input_tokens") or 0
                        else:
                            bucket["cache_5m"] += usage.get("cache_creation_input_tokens") or 0
            except OSError:
                continue

    total_cost = 0.0
    for model, tok in today_totals.items():
        in_price, out_price = _claude_pricing_for(model)
        total_cost += tok["input"] / 1_000_000 * in_price
        total_cost += tok["output"] / 1_000_000 * out_price
        total_cost += tok["cache_5m"] / 1_000_000 * in_price * CACHE_WRITE_5M_MULT
        total_cost += tok["cache_1h"] / 1_000_000 * in_price * CACHE_WRITE_1H_MULT
        total_cost += tok["cache_read"] / 1_000_000 * in_price * CACHE_READ_MULT

    return {"model": last_model, "costUSD": round(total_cost, 2)}


def claude_probe(model: str = None) -> dict:
    """Account-wide Pro/Max 5h + 7d subscription windows. `model` is accepted
    for interface parity but ignored — these windows have no model dimension."""
    token = claude_read_token()
    body = json.dumps({
        "model": "claude-haiku-4-5-20251001",
        "max_tokens": 1,
        "messages": [{"role": "user", "content": "hi"}],
    }).encode("utf-8")
    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages", data=body, method="POST",
        headers={
            "Authorization": f"Bearer {token}",       # OAuth token → Bearer, not x-api-key
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",      # unlocks the unified rate-limit headers
            "Content-Type": "application/json",
            "User-Agent": CLAUDE_CODE_USER_AGENT,      # server gates the headers on this
        },
    )
    try:
        resp = urllib.request.urlopen(req, timeout=15)
        headers = resp.headers
        resp.read()  # drain body, not needed
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            raise RuntimeError(f"Token rejected (HTTP {e.code}) — sign in to Claude Code again "
                               f"(the login token expires periodically; Claude Code refreshes it)")
        raise RuntimeError(f"HTTP {e.code}: {e.read().decode(errors='replace')}")

    def pct(name: str) -> int:
        raw = headers.get(name)
        if raw is None:
            return 0
        try:
            return max(0, min(100, round(float(raw) * 100)))
        except ValueError:
            return 0

    def epoch(name: str) -> int:
        raw = headers.get(name)
        try:
            return int(raw) if raw is not None else 0
        except ValueError:
            return 0

    local = claude_scan_usage_today()

    return {
        "used":         pct("anthropic-ratelimit-unified-5h-utilization"),
        "limit":        100,
        "resetEpoch":   epoch("anthropic-ratelimit-unified-5h-reset"),
        "used7d":       pct("anthropic-ratelimit-unified-7d-utilization"),
        "resetEpoch7d": epoch("anthropic-ratelimit-unified-7d-reset"),
        "model":        local["model"],
        "models":       [],
        "balance":      local["costUSD"],  # estimated cost for today, not a real Pro/Max charge
    }


# ─── Provider registry ──────────────────────────────────────────────────────

PROVIDERS = {
    "claude": claude_probe,
    "cursor": cursor_probe,
    "codex":  codex_probe,
}


def probe(provider: str, model: str = None) -> dict:
    fn = PROVIDERS.get(provider)
    if not fn:
        raise RuntimeError(f"Unknown provider '{provider}' (expected one of: {', '.join(PROVIDERS)})")
    return fn(model)


# ─── Push mode ──────────────────────────────────────────────────────────────

def push_to_device(ip: str, index: int, usage: dict) -> None:
    payload = {
        "index": index,
        "used": usage.get("used", 0),
        "limit": usage.get("limit", 0),
        "resetEpoch": usage.get("resetEpoch", 0),
        "used7d": usage.get("used7d", 0),
        "resetEpoch7d": usage.get("resetEpoch7d", 0),
    }
    # Only present for providers that resolve a current model / cost estimate
    # (currently just Claude) — omit entirely rather than sending an empty
    # value, so other providers' existing on-device model/balance aren't
    # overwritten by an absent field.
    if usage.get("model"):
        payload["model"] = usage["model"]
    if usage.get("balance") is not None:
        payload["balance"] = usage["balance"]
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        f"http://{ip}/push", data=body, method="POST",
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=8) as resp:
        resp.read()


def run_once(ip: str, targets: dict) -> None:
    for prov, (index, model) in targets.items():
        try:
            usage = probe(prov, model)
            push_to_device(ip, index, usage)
            log(f"{prov}: used={usage.get('used')} used7d={usage.get('used7d', 0)} "
                f"-> pushed to {ip} (agent index {index}{f', model={model}' if model else ''})")
        except Exception as e:
            log(f"{prov}: ERROR: {e}")


# ─── Bridge mode ────────────────────────────────────────────────────────────

class BridgeHandler(BaseHTTPRequestHandler):
    cache_seconds = 60
    _cache: dict = {}  # (provider, model) -> (usage, cached_at)

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path != "/usage":
            self.send_response(404)
            self._cors()
            self.end_headers()
            return
        query = urllib.parse.parse_qs(parsed.query)
        prov = (query.get("provider", [None])[0] or "").lower()
        model = query.get("model", [None])[0] or None
        if prov not in PROVIDERS:
            self.send_response(400)
            self._cors()
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"error": "missing or unknown ?provider= (cursor|codex)"}).encode())
            return
        cache_key = (prov, model)
        try:
            now = time.monotonic()
            cached_entry = BridgeHandler._cache.get(cache_key)
            if cached_entry is not None and (now - cached_entry[1]) < BridgeHandler.cache_seconds:
                usage, cached = cached_entry[0], True
            else:
                usage = probe(prov, model)
                BridgeHandler._cache[cache_key] = (usage, now)
                cached = False
            body = json.dumps(usage).encode("utf-8")
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(body)
            log(f"/usage?provider={prov} -> used={usage.get('used')}{' (cached)' if cached else ''}")
        except Exception as e:
            body = json.dumps({"error": str(e)}).encode("utf-8")
            self.send_response(502)
            self._cors()
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(body)
            log(f"/usage?provider={prov} -> ERROR: {e}")

    def log_message(self, fmt, *fmt_args):
        pass  # quiet — we log ourselves above


def run_server(port: int, cache_seconds: int) -> None:
    BridgeHandler.cache_seconds = cache_seconds
    server = HTTPServer(("127.0.0.1", port), BridgeHandler)
    log(f"Bridge server running at http://127.0.0.1:{port}/usage?provider=cursor|codex (cache={cache_seconds}s)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


# ─── CLI ────────────────────────────────────────────────────────────────────

def parse_targets(pairs: list) -> dict:
    """Parse 'provider:index' or 'provider:index:model' entries."""
    targets = {}
    for pair in pairs:
        parts = pair.split(":")
        if len(parts) not in (2, 3):
            raise SystemExit(f"--push entries must be provider:index or provider:index:model, got '{pair}'")
        prov = parts[0].strip().lower()
        idx = parts[1]
        model = parts[2] if len(parts) == 3 and parts[2] else None
        if prov not in PROVIDERS:
            raise SystemExit(f"Unknown provider '{prov}' in --push (expected one of: {', '.join(PROVIDERS)})")
        try:
            targets[prov] = (int(idx), model)
        except ValueError:
            raise SystemExit(f"--push index must be an integer, got '{idx}' in '{pair}'")
    return targets


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ip", default="token-tracker.local",
                         help="ESP32 device address — IP or hostname (default: token-tracker.local, "
                              "the device's mDNS name; use a plain IP instead if your OS can't resolve "
                              ".local names, e.g. Windows without Bonjour/mDNS support installed)")
    parser.add_argument("--push", nargs="+", default=[],
                        help="provider:index[:model] entries to sync each cycle, e.g. cursor:1 codex:2:gpt-4o")
    parser.add_argument("--interval", type=int, default=120, help="Seconds between probes (default: 120)")
    parser.add_argument("--once", action="store_true", help="Run a single probe/push cycle and exit")
    parser.add_argument("--serve", action="store_true",
                        help="Run a local HTTP bridge (GET /usage?provider=...) for browser-based testing")
    parser.add_argument("--port", type=int, default=8765, help="Port for --serve (default: 8765)")
    parser.add_argument("--cache", type=int, default=60,
                        help="Serve mode: minimum seconds between real probes per provider (default: 60)")
    args = parser.parse_args()

    if args.serve:
        run_server(args.port, args.cache)
        return 0

    if not args.push:
        parser.error("--push provider:index [provider:index ...] is required unless --serve is used")

    targets = parse_targets(args.push)

    if args.once:
        run_once(args.ip, targets)
        return 0

    log(f"Starting daemon: device={args.ip} targets={targets} interval={args.interval}s")
    while True:
        run_once(args.ip, targets)
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
