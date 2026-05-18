"""
Companion HTTP server for the PolyCast5 "Claude Usage" tool.

WHY THIS EXISTS
    Anthropic does not expose Pro/Max subscription usage (the
    numbers shown at claude.ai/settings/usage) via any public API.
    The only way to surface them on the device is to scrape them
    from claude.ai using a session cookie from a logged-in browser,
    then re-serve them as JSON on the LAN. The ESP32 polls this
    server every 60 s.

WHAT THIS SCRIPT DOES
    Serves GET /usage with the shape the firmware expects:

        {
          "session": {"pct": 23, "reset_secs": 2400},
          "weekly":  {"pct": 6,  "reset_secs": 85000},
          "sonnet":  {"pct": 0,  "reset_secs": 85000, "unused": true},
          "design":  {"pct": 0,  "reset_secs": 85000, "unused": true}
        }

    The endpoint is unauthenticated. Anyone on your LAN who knows
    the URL can read your usage stats - run only on a trusted
    network.

USAGE
    python claude_usage_server.py \
        --session-key "<sessionKey cookie value>" \
        --org-id     "<organization uuid from /api/organizations/<this>/usage>"

    Both args may also be supplied as env vars CLAUDE_SESSION_KEY
    and CLAUDE_ORG_ID. Run with --help for everything else.

WHERE TO FIND THE INPUTS
    sessionKey  DevTools -> Application -> Cookies -> https://claude.ai
                copy the "Value" of the row named sessionKey
    org id      DevTools -> Network -> /settings/usage page reload ->
                click the `usage` request -> Request URL is
                claude.ai/api/organizations/<THIS-UUID>/usage

CAVEATS
    - claude.ai's internal endpoints can change without notice. This
      script is the brittle layer by design; the firmware stays
      decoupled.
    - The sessionKey cookie expires periodically (re-paste when it
      does).
    - Use at your own risk. Anthropic does not endorse this approach.
"""

# Stdlib only - no pip install needed. argparse drives the CLI; urllib does
# the HTTPS call to claude.ai; http.server hosts the LAN endpoint the ESP32
# polls; threading runs the background refresher independently of the server.
import argparse
import json
import os
import socket
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import request as urlrequest


# =============== LAN-IP detection ===============

def _detect_lan_ip():
    """Best-effort LAN IP detection. Opens a UDP socket to a public address
    so the OS picks the default-route interface, then reads the local
    socket name. No packet is actually sent. Returns None on failure."""
    # AF_INET + SOCK_DGRAM = IPv4 UDP. Using UDP (not TCP) means connect()
    # doesn't actually establish a session, just selects a route.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # 8.8.8.8 is a public anycast IP. connect() resolves a route, but
        # with UDP no datagram is sent until we call send/sendto.
        s.connect(("8.8.8.8", 80))
        # The local socket name now reflects the chosen interface's IP.
        return s.getsockname()[0]
    except OSError:
        # No network, no default route, etc. Caller falls back to a placeholder.
        return None
    finally:
        s.close()


# =============== Shared cache ===============
# Populated by the background refresher; read by every HTTP handler call.
# Lock guards both fields so a partial refresh never bleeds into a response.
_cache_lock = threading.Lock()
_cache = {
    "payload": None,    # Last good JSON dict from claude.ai, or None pre-first-fetch
    "fetched_at": 0.0,  # Wall-clock time of last successful fetch (unused today, here for future "last updated" debug output)
    "last_error": None, # repr() of the most recent fetch exception, or None on success
}


# =============== Field coercion helpers ===============

def _seconds_until(value):
    """Coerce an ISO-8601 timestamp OR a numeric duration into seconds-from-now."""
    # None: nothing to compute, treat as zero seconds
    if value is None:
        return 0
    # Already a number: assume it's already a duration in seconds; just clamp
    if isinstance(value, (int, float)):
        return max(0, int(value))
    # ISO-8601 string from claude.ai
    if isinstance(value, str):
        # Python's fromisoformat predates 3.11's native Z support; rewrite Z -> +00:00
        s = value.replace("Z", "+00:00")
        try:
            # Normalize to UTC so the subtraction below is timezone-correct
            target = datetime.fromisoformat(s).astimezone(timezone.utc)
            now = datetime.now(timezone.utc)
            # Negative deltas (timestamp already passed) clamp to 0 so the
            # firmware shows "--" instead of a confusing past time
            return max(0, int((target - now).total_seconds()))
        except ValueError:
            # Malformed string - degrade gracefully
            return 0
    # Anything else (lists, dicts, bools, etc.) - degrade gracefully
    return 0


def _metric(node, fallback_reset):
    """Map one claude.ai limit sub-object to (pct, reset_secs, unused).

    Schema observed: {"utilization": 23.0, "resets_at": "2026-...+00:00"}
    Unused features have utilization=0.0 and resets_at=None (their window
    follows the weekly reset). Entirely-null nodes mean the feature doesn't
    apply to this account."""
    # Missing node, or present but without a utilization number → mark unused.
    # The fallback_reset lets callers tell us what to substitute (e.g. weekly
    # reset for sub-buckets that share the seven-day window).
    if not isinstance(node, dict) or node.get("utilization") is None:
        return 0, fallback_reset, True

    # Active bucket: clamp the utilization to [0, 100] integer percent so a
    # weird API response (101.5%, -1, etc.) never reaches the firmware
    util = node["utilization"]
    pct = max(0, min(100, int(round(util))))
    # Only honor resets_at when it's present; otherwise use the caller's fallback
    reset = _seconds_until(node.get("resets_at")) if node.get("resets_at") else fallback_reset
    # "unused" is informational - firmware uses it only for sonnet/design rows
    return pct, reset, util == 0.0


# =============== Live fetch from claude.ai ===============

def fetch_live_usage(session_key, org_id, debug_raw=False):
    """Hit https://claude.ai/api/organizations/<org_id>/usage and map the
    response onto the firmware's expected shape.

    Observed response (May 2026):
        {
          "five_hour":          {"utilization": 23.0, "resets_at": "..."},
          "seven_day":          {"utilization": 6.0,  "resets_at": "..."},
          "seven_day_sonnet":   {"utilization": 0.0,  "resets_at": null},
          "seven_day_omelette": {"utilization": 0.0,  "resets_at": null},
          ...other nulled-out variants...
        }

    "omelette" is Anthropic's internal codename for Claude Design.
    """
    # Organization-scoped endpoint; the org UUID acts as a per-account path
    url = f"https://claude.ai/api/organizations/{org_id}/usage"

    # Headers mirror what claude.ai's React app sends on the same call. The
    # anthropic-client-* headers are required - claude.ai 403s requests
    # without them. Cookie carries auth; everything else is browser cosplay.
    req = urlrequest.Request(
        url,
        headers={
            "Accept": "*/*",
            # Force identity so Cloudflare/claude.ai don't ship back zstd or br -
            # urllib doesn't auto-decompress either and we'd choke on json.loads.
            "Accept-Encoding": "identity",
            "Accept-Language": "en-US,en;q=0.9",
            "Anthropic-Client-Platform": "web_claude_ai",
            "Anthropic-Client-Version":  "1.0.0",
            "Cookie":  f"sessionKey={session_key}",
            "Referer": "https://claude.ai/settings/usage",
            "User-Agent": ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                           "AppleWebKit/537.36 (KHTML, like Gecko) "
                           "Chrome/148.0.0.0 Safari/537.36"),
        },
        method="GET",
    )

    # urlopen raises HTTPError on 4xx/5xx; the caller's exception handler
    # logs that and updates _cache["last_error"]
    with urlrequest.urlopen(req, timeout=15) as resp:
        raw = json.loads(resp.read().decode("utf-8"))

    # Optional debug dump - useful when claude.ai changes the schema and the
    # firmware suddenly shows "--%" everywhere
    if debug_raw:
        sys.stderr.write("--- raw claude.ai /usage response ---\n")
        # Cap at 4KB so a runaway response doesn't flood the terminal
        sys.stderr.write(json.dumps(raw, indent=2)[:4000])
        sys.stderr.write("\n--- end raw ---\n")
        sys.stderr.flush()

    # Flatten the four buckets the firmware cares about. Weekly is computed
    # first so its reset_secs can be used as a fallback for sonnet/design,
    # which share the seven-day cadence but report resets_at: null when unused.
    weekly_pct,  weekly_reset,  _      = _metric(raw.get("seven_day"),          0)
    session_pct, session_reset, _      = _metric(raw.get("five_hour"),          0)
    sonnet_pct,  sonnet_reset,  son_un = _metric(raw.get("seven_day_sonnet"),   weekly_reset)
    design_pct,  design_reset,  des_un = _metric(raw.get("seven_day_omelette"), weekly_reset)

    # Firmware contract - keys and field names match what wifi_claude.c's
    # parse_metric() expects. The "unused" flag is only sent for the two
    # sub-buckets that can legitimately be unused on an account.
    return {
        "session": {"pct": session_pct, "reset_secs": session_reset},
        "weekly":  {"pct": weekly_pct,  "reset_secs": weekly_reset},
        "sonnet":  {"pct": sonnet_pct,  "reset_secs": sonnet_reset, "unused": son_un},
        "design":  {"pct": design_pct,  "reset_secs": design_reset, "unused": des_un},
    }


# =============== Background refresher ===============

def _refresh_once(session_key, org_id, debug_raw):
    """Pull a fresh snapshot and stash it in the cache."""
    try:
        # Network + JSON parse happen here. On success we replace the cache atomically.
        payload = fetch_live_usage(session_key, org_id, debug_raw=debug_raw)
        with _cache_lock:
            _cache["payload"] = payload
            _cache["fetched_at"] = time.time()
            _cache["last_error"] = None
        # stdout for happy path so it's easy to confirm the loop is alive
        print(f"[{time.strftime('%H:%M:%S')}] refresh ok", flush=True)
    except Exception as exc:  # noqa: BLE001 - daemon thread must never die
        # Catch-all: TimeoutError, ConnectionResetError, SSL errors, JSON parse errors, anything claude.ai might throw
        with _cache_lock:
            _cache["last_error"] = repr(exc)
        # stderr for failures so they stand out in the user's terminal
        print(f"[{time.strftime('%H:%M:%S')}] refresh failed: {exc!r}",
              file=sys.stderr, flush=True)


def _refresher_thread(session_key, org_id, debug_raw, interval_secs, stop_event):
    """Background loop that keeps the cache fresh.

    Defense-in-depth: the inner _refresh_once() already swallows fetch
    errors, but we also wrap the loop body so a bug in our own code can't
    kill the refresher and silently freeze the cache."""
    while not stop_event.is_set():
        try:
            # One fetch attempt per iteration
            _refresh_once(session_key, org_id, debug_raw)
        except Exception as exc:  # noqa: BLE001
            print(f"[{time.strftime('%H:%M:%S')}] refresher loop error: {exc!r}",
                  file=sys.stderr, flush=True)
        # wait() returns early when stop_event is set, so shutdown is prompt
        stop_event.wait(interval_secs)


# =============== HTTP handler ===============

class _Handler(BaseHTTPRequestHandler):
    """Single-endpoint server: GET /usage returns the cached snapshot."""

    # Identifies us in the Server: response header
    server_version = "PolyCast5-Companion/1.0"

    # Override the default address_string() so we don't pay a reverse-DNS
    # lookup on every request (it can stall the response by seconds)
    def address_string(self):
        return self.client_address[0]

    def log_message(self, fmt, *args):
        # Custom one-line log format so requests interleave cleanly with refresh logs
        sys.stdout.write(f"[{self.log_date_time_string()}] "
                         f"{self.address_string()} {fmt % args}\n")

    def _send(self, code, payload):
        """Serialize a dict to JSON and emit it with appropriate headers."""
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        # no-store so the ESP32 always pulls fresh data, never a 304 etc.
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        # Only /usage exists; anything else is 404
        if self.path != "/usage":
            self._send(404, {"error": "not found"})
            return

        # Snapshot the cache under the lock - keeps payload and last_error consistent
        with _cache_lock:
            payload = _cache["payload"]
            last_err = _cache["last_error"]

        # No successful fetch yet (script just started, or every fetch has failed)
        if payload is None:
            self._send(503, {"error": f"no data yet: {last_err or 'starting up'}"})
            return

        # Happy path - return the cached snapshot verbatim
        self._send(200, payload)


# =============== CLI ===============

def _build_argparser():
    """Set up argparse with env-var fallbacks for every flag."""
    p = argparse.ArgumentParser(
        prog="claude_usage_server.py",
        description="Re-serves your claude.ai subscription usage as JSON for the "
                    "PolyCast5 device.",
    )
    # Required (no env fallback default = enforced by the missing-arg check below)
    p.add_argument(
        "--session-key", "-s",
        default=os.environ.get("CLAUDE_SESSION_KEY"),
        help="Value of the `sessionKey` cookie from a logged-in claude.ai tab. "
             "May also be supplied via the CLAUDE_SESSION_KEY env var.",
    )
    p.add_argument(
        "--org-id", "-o",
        default=os.environ.get("CLAUDE_ORG_ID"),
        help="Organization UUID from claude.ai/api/organizations/<this>/usage. "
             "May also be supplied via the CLAUDE_ORG_ID env var.",
    )
    # Optional knobs - all have sensible defaults, env-var override available
    p.add_argument(
        "--port", "-p", type=int,
        default=int(os.environ.get("COMPANION_PORT", "8765")),
        help="TCP port to listen on (default: 8765).",
    )
    p.add_argument(
        "--bind", "-b",
        default=os.environ.get("COMPANION_BIND", "0.0.0.0"),
        help="Address to bind to (default: 0.0.0.0, all interfaces).",
    )
    p.add_argument(
        "--refresh", "-r", type=int,
        default=int(os.environ.get("REFRESH_SECONDS", "15")),
        help="Seconds between background refreshes of claude.ai (default: 15, "
             "to match the device's 15s poll cadence).",
    )
    p.add_argument(
        "--debug-raw",
        action="store_true",
        default=os.environ.get("DEBUG_RAW") == "1",
        help="Dump the raw claude.ai response to stderr on every refresh.",
    )
    return p


# =============== Entry point ===============

def main():
    args = _build_argparser().parse_args()

    # Required-arg enforcement. We don't use argparse's required=True because
    # we want env-var fallbacks to count - argparse doesn't know about defaults
    # that came from os.environ.
    missing = [name for name, val in (("--session-key", args.session_key),
                                       ("--org-id",      args.org_id)) if not val]
    if missing:
        sys.stderr.write(f"error: missing required argument(s): {', '.join(missing)}\n")
        sys.stderr.write("       run with --help for usage.\n")
        sys.exit(2)

    # Spin up the background refresher as a daemon thread so it dies with the
    # main process; stop_event gives us a clean cancellation hook for Ctrl-C.
    stop = threading.Event()
    t = threading.Thread(
        target=_refresher_thread,
        args=(args.session_key, args.org_id, args.debug_raw, args.refresh, stop),
        daemon=True,
    )
    t.start()

    # ThreadingHTTPServer spawns a worker thread per request so a slow handler
    # never stalls a concurrent ESP32 poll.
    server = ThreadingHTTPServer((args.bind, args.port), _Handler)

    # Pick the host string the device should actually point at - for the
    # banner only. The actual binding happens on args.bind (often 0.0.0.0).
    if args.bind in ("0.0.0.0", "::"):
        # Wildcard bind: print the actual LAN IP the device should target
        host = _detect_lan_ip() or "<this-host>"
    else:
        # Explicit bind address: print it as-is
        host = args.bind

    # Startup banner - everything the user needs to verify the script is ready
    print(f"PolyCast5 Claude-usage companion")
    print(f"  bind     = {args.bind}:{args.port}")
    print(f"  refresh  = every {args.refresh}s")
    if args.debug_raw:
        print(f"  debug    = DEBUG_RAW on (raw responses to stderr)")
    print(f"  endpoint = http://{host}:{args.port}/usage")
    print(f"Ctrl-C to stop.")
    try:
        # Blocks until KeyboardInterrupt or server.shutdown() is called
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down")
    finally:
        # Signal the refresher to exit its loop, then close listening sockets
        stop.set()
        server.server_close()


if __name__ == "__main__":
    main()
