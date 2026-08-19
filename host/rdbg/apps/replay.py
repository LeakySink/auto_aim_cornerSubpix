"""Offline .rlog replay — timeline API + client-side player controls."""

import json
import sys
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse

from ..httputil import ThreadingHTTPServer, serve_page, try_serve_static
from ..rlog import load, sender_name, to_sse

# Max recorded gap (s) between image frames that still advances the playhead.
# Larger gaps are compressed so bursts stitch into continuous video.
IMG_FRAME_CAP_S = 1.0 / 25.0


def _prepare(records):
    images = []
    out = []
    for rec in records:
        if rec.get("_rlog") == "img":
            idx = len(images)
            images.append(rec.get("jpeg") or b"")
            out.append({
                "ts": rec.get("ts", 0),
                "_rlog": "img",
                "meta": rec.get("meta") if isinstance(rec.get("meta"), dict) else {},
                "_img_idx": idx,
            })
        else:
            out.append(rec)
    return out, images


def _build_events(records):
    events = []
    for rec in records:
        if rec.get("_rlog") == "img":
            events.append({
                "type": "image",
                "ts": rec.get("ts", 0),
                "idx": rec["_img_idx"],
                "meta": rec.get("meta") or {},
            })
            continue
        msg = to_sse(rec)
        if msg:
            events.append(msg)
    return events


def _json_response(handler, obj, code=200):
    body = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Cache-Control", "no-store")
    handler.end_headers()
    handler.wfile.write(body)


def run(rlog_path, http_port, speed=1.0):
    path = Path(rlog_path)
    if not path.is_file():
        print(f"[replay] file not found: {path}", file=sys.stderr)
        return 1

    raw = load(path)
    records, images = _prepare(raw)
    events = _build_events(records)
    sender = sender_name(raw, fallback=path.stem)
    timeline = {
        "mode": "replay",
        "sender": sender,
        "file": path.name,
        "img_cap": IMG_FRAME_CAP_S,
        "speed": float(speed) if speed else 1.0,
        "events": events,
        "n_img": len(images),
        "n_events": len(events),
    }
    print(
        f"[replay] {path}  {len(events)} events ({len(images)} images)  "
        f"sender={sender}",
        file=sys.stderr,
    )

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            pass

        def do_GET(self):
            route = urlparse(self.path).path
            if route == "/":
                serve_page(self, "debugger.html")
            elif route == "/api/timeline":
                _json_response(self, timeline)
            elif route == "/events":
                # Live SSE not used in replay; keep a quiet stream for old clients.
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                try:
                    payload = json.dumps(
                        {"type": "status", "connected": True, "sender": sender},
                        separators=(",", ":"),
                    )
                    self.wfile.write(f"data: {payload}\n\n".encode())
                    self.wfile.flush()
                except Exception:
                    pass
            elif route.startswith("/img/"):
                try:
                    idx = int(route.rsplit("/", 1)[-1].split("?")[0])
                except ValueError:
                    self.send_error(404)
                    return
                if idx < 0 or idx >= len(images):
                    self.send_error(404)
                    return
                body = images[idx]
                self.send_response(200)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
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
