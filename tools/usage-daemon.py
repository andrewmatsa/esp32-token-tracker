#!/usr/bin/env python3
"""
Unified usage daemon for token-tracker (Cursor, Codex).

Claude is not covered here — it authenticates on-device with a regular
API key (see the project's tools/README.md), since Anthropic disabled
OAuth session tokens (`claude setup-token`) for third-party clients
around February 2026, which is what this daemon's Claude support relied
on. There's no local-file credential to auto-read for a regular API key,
so routing it through a daemon adds no value over pasting it directly
into the device's web UI.

Each remaining provider stores its own login/token locally (Cursor IDE's
SQLite state db, Codex CLI's auth.json) and exposes usage via its own
probe (Cursor's /auth/usage, Codex's /wham/usage). This script reads
whichever ones you configure, on one process, one port — instead of
running two separate scripts.

Stdlib only — no pip install required.

Push mode — pushes each configured provider to its own agent slot on the
device, in a single shared loop. Each --push entry is provider:index, or
provider:index:model to filter to one model bucket instead of the
account-wide total:
    python usage-daemon.py --ip 192.168.1.50 --push cursor:1 codex:2:gpt-4o [--interval 120] [--once]

Bridge mode — one local server for browser-based testing (temporary.html),
serving both providers on one port via ?provider=:
    python usage-daemon.py --serve [--port 8765] [--cache 60]
    GET http://127.0.0.1:8765/usage?provider=cursor[&model=gpt-4]
    GET http://127.0.0.1:8765/usage?provider=codex[&model=gpt-4o]

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


# ─── Provider registry ──────────────────────────────────────────────────────

PROVIDERS = {
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
    body = json.dumps({
        "index": index,
        "used": usage.get("used", 0),
        "limit": usage.get("limit", 0),
        "resetEpoch": usage.get("resetEpoch", 0),
        "used7d": usage.get("used7d", 0),
        "resetEpoch7d": usage.get("resetEpoch7d", 0),
    }).encode("utf-8")
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
    parser.add_argument("--ip", help="ESP32 device IP, e.g. 192.168.1.50 (required unless --serve)")
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

    if not args.ip:
        parser.error("--ip is required unless --serve is used")
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
