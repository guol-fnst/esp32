import json
import math
import os
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import URLError
from urllib.request import urlopen


HOST = os.environ.get("QUOTA_DASHBOARD_HOST", "0.0.0.0")
PORT = int(os.environ.get("QUOTA_DASHBOARD_PORT", "8765"))
ANTIGRAVITY_API = os.environ.get("ANTIGRAVITY_API", "http://127.0.0.1:8045/api/accounts")
CODEX_ACCOUNTS_FILE = Path(
    os.environ.get(
        "CODEX_ACCOUNTS_FILE",
        r"C:\Users\leile\AppData\Roaming\com.carry.codex-tools\accounts.json",
    )
)
LOCAL_TZ = datetime.now().astimezone().tzinfo or timezone.utc


def safe_text(value, limit=48):
    text = "" if value is None else str(value)
    text = text.replace("|", "/").replace("\r", " ").replace("\n", " ")
    return text[:limit]


def fmt_reset(ts):
    if not ts:
        return "-"
    dt = datetime.fromtimestamp(ts, tz=timezone.utc).astimezone(LOCAL_TZ)
    return dt.strftime("%m-%d %H:%M")


def fmt_reset_iso(iso_text):
    if not iso_text:
        return "-"
    dt = datetime.fromisoformat(iso_text.replace("Z", "+00:00")).astimezone(LOCAL_TZ)
    return dt.strftime("%m-%d %H:%M")


def average_percent(models, prefixes):
    values = [
        float(model.get("percentage", 0))
        for model in models
        if any(str(model.get("name", "")).startswith(prefix) for prefix in prefixes)
    ]
    if not values:
        return None
    return round(sum(values) / len(values))


def nearest_reset_iso(models, prefixes):
    resets = [
        model.get("reset_time")
        for model in models
        if any(str(model.get("name", "")).startswith(prefix) for prefix in prefixes) and model.get("reset_time")
    ]
    if not resets:
        return "-"
    return fmt_reset_iso(sorted(resets)[0])


def load_antigravity_rows():
    with urlopen(ANTIGRAVITY_API, timeout=5) as response:
        payload = json.loads(response.read().decode("utf-8"))

    rows = []
    accounts = payload.get("accounts", [])
    for account in accounts:
        quota = account.get("quota") or {}
        models = quota.get("models") or []
        gemini = average_percent(models, ["gemini-"])
        claude = average_percent(models, ["claude-"])
        row = {
            "platform": "AG",
            "email": safe_text(account.get("email") or account.get("label") or "unknown"),
            "metric1": f"Gem {gemini}%" if gemini is not None else "Gem -",
            "metric2": f"Cld {claude}%" if claude is not None else "Cld -",
            "reset": nearest_reset_iso(models, ["gemini-", "claude-"]),
            "sort": 0 if account.get("is_current") else 1,
        }
        rows.append(row)

    rows.sort(key=lambda item: (item["sort"], item["email"].lower()))
    return rows


def load_codex_rows():
    payload = json.loads(CODEX_ACCOUNTS_FILE.read_text(encoding="utf-8"))
    rows = []
    for account in payload.get("accounts", []):
        usage = account.get("usage") or {}
        five_hour = usage.get("fiveHour") or {}
        one_week = usage.get("oneWeek") or {}
        remaining_5h = max(0, min(100, round(100 - float(five_hour.get("usedPercent", 0)))))
        remaining_1w = max(0, min(100, round(100 - float(one_week.get("usedPercent", 0)))))
        reset_candidates = [ts for ts in [five_hour.get("resetAt"), one_week.get("resetAt")] if ts]
        row = {
            "platform": "CX",
            "email": safe_text(account.get("email") or account.get("label") or "unknown"),
            "metric1": f"5H {remaining_5h}%",
            "metric2": f"1W {remaining_1w}%",
            "reset": fmt_reset(min(reset_candidates)) if reset_candidates else "-",
            "sort": 0,
        }
        rows.append(row)

    rows.sort(key=lambda item: item["email"].lower())
    return rows


def build_snapshot():
    now = datetime.now(tz=LOCAL_TZ)
    rows = []
    errors = []

    try:
        rows.extend(load_antigravity_rows())
    except (OSError, URLError, ValueError, json.JSONDecodeError) as exc:
        errors.append(f"antigravity: {exc}")

    try:
        rows.extend(load_codex_rows())
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        errors.append(f"codex: {exc}")

    return {
        "epoch": int(now.timestamp()),
        "updated_at": now.strftime("%Y-%m-%d %H:%M:%S"),
        "count": len(rows),
        "rows": rows,
        "errors": errors,
    }


def build_text(snapshot):
    lines = [f"META|{snapshot['epoch']}|{safe_text(snapshot['updated_at'], 24)}|{snapshot['count']}"]
    for row in snapshot["rows"]:
        lines.append(
            "ROW|{platform}|{email}|{metric1}|{metric2}|{reset}".format(
                platform=safe_text(row["platform"], 4),
                email=safe_text(row["email"], 48),
                metric1=safe_text(row["metric1"], 16),
                metric2=safe_text(row["metric2"], 16),
                reset=safe_text(row["reset"], 16),
            )
        )
    if snapshot["errors"]:
        lines.append("ERR|" + safe_text("; ".join(snapshot["errors"]), 180))
    return "\n".join(lines) + "\n"


class Handler(BaseHTTPRequestHandler):
    def _send(self, status_code, content_type, body):
        encoded = body.encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self):
        if self.path == "/health":
            self._send(200, "application/json; charset=utf-8", json.dumps({"status": "ok", "service": "quota-dashboard"}))
            return

        snapshot = build_snapshot()
        if self.path in ("/api/summary.json", "/"):
            self._send(200, "application/json; charset=utf-8", json.dumps(snapshot, ensure_ascii=False, indent=2))
            return
        if self.path == "/api/esp.txt":
            self._send(200, "text/plain; charset=utf-8", build_text(snapshot))
            return

        self._send(404, "application/json; charset=utf-8", json.dumps({"error": "not found"}))

    def log_message(self, fmt, *args):
        return


if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"quota-dashboard listening on http://{HOST}:{PORT}")
    server.serve_forever()
