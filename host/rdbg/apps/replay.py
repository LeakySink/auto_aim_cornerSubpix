"""Offline .rlog replay — same debugger UI, no UDP."""

import json
import sys
import time
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse

from ..httputil import ThreadingHTTPServer, serve_page, try_serve_static
from ..rlog import load, sender_name, to_sse


def _sse_messages(records, sender):
    msgs = [
        {"type": "state", "active_sender": sender, "senders": [sender]},
    ]
    for rec in records:
        msg = to_sse(rec)
        if msg:
            msgs.append(msg)
    msgs.append({"type": "status", "connected": True, "sender": sender})
    return [json.dumps(m, separators=(",", ":")) for m in msgs]


def _write_replay_sse(handler, lines, sender):
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.send_header("Connection", "keep-alive")
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    try:
        for i, line in enumerate(lines):
            handler.wfile.write(f"data: {line}\n\n".encode())
            if i % 200 == 0:
                handler.wfile.flush()
        handler.wfile.flush()
        keep = json.dumps(
            {"type": "status", "connected": True, "sender": sender},
            separators=(",", ":"),
        )
        while True:
            time.sleep(2)
            handler.wfile.write(f"data: {keep}\n\n".encode())
            handler.wfile.flush()
    except Exception:
        pass


def run(rlog_path, http_port):
    path = Path(rlog_path)
    if not path.is_file():
        print(f"[replay] file not found: {path}", file=sys.stderr)
        return 1

    records = load(path)
    sender = sender_name(records, fallback=path.stem)
    lines = _sse_messages(records, sender)
    n_img = sum(1 for r in records if r.get("_rlog") == "img")
    print(
        f"[replay] {path}  {len(records)} records ({n_img} images)  sender={sender}",
        file=sys.stderr,
    )

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            pass

        def do_GET(self):
            route = urlparse(self.path).path
            if route == "/":
                serve_page(self, "debugger.html")
            elif route == "/events":
                _write_replay_sse(self, lines, sender)
            elif try_serve_static(self, self.path):
                return
            else:
                self.send_error(404)

    try:
        httpd = ThreadingHTTPServer(("0.0.0.0", http_port), Handler)
    except OSError as e:
        print(f"[replay] cannot bind port {http_port}: {e}", file=sys.stderr)
        return 1

    print(f"[replay] http://localhost:{http_port}", file=sys.stderr)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[replay] shutting down...", file=sys.stderr)
    finally:
        httpd.server_close()
    return 0
