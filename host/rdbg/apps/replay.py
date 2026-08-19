"""Offline .rlog replay — same debugger UI, paced SSE (no UDP)."""

import json
import sys
import time
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse

from ..httputil import ThreadingHTTPServer, serve_page, try_serve_static
from ..rlog import load, sender_name, to_sse

# Compress idle gaps so long pauses don't feel stuck; still respects --speed.
_MAX_GAP_S = 0.35
# Drop intermediate frames only when clearly behind (keep UI responsive).
_IMG_CATCHUP_S = 0.15


def _record_ts(rec):
    if rec.get("_rlog") == "img":
        return int(rec.get("ts") or 0)
    return int(rec.get("ts") or 0)


def _prepare(records):
    """Index images for /img/N; strip jpeg bytes from SSE path."""
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


def _to_sse(rec):
    if rec.get("_rlog") == "img":
        meta = rec.get("meta") or {}
        return {
            "type": "image",
            "ts": rec.get("ts", 0),
            "meta": meta,
            "url": f"/img/{rec['_img_idx']}",
        }
    return to_sse(rec)


def _write_replay_sse(handler, records, sender, speed):
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.send_header("Connection", "keep-alive")
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()

    def emit(obj):
        handler.wfile.write(
            f"data: {json.dumps(obj, separators=(',', ':'))}\n\n".encode()
        )

    try:
        emit({"type": "state", "active_sender": sender, "senders": [sender]})
        emit({"type": "status", "connected": True, "sender": sender})
        handler.wfile.flush()

        if not records:
            keep = {"type": "status", "connected": True, "sender": sender}
            while True:
                time.sleep(2)
                emit(keep)
                handler.wfile.flush()
            return

        speed = max(0.05, float(speed))
        t0_rec = None
        t0_wall = time.monotonic()
        prev_ts = None
        n = len(records)

        for i, rec in enumerate(records):
            ts = _record_ts(rec)
            if t0_rec is None:
                t0_rec = ts
                prev_ts = ts

            target = (ts - t0_rec) / 1e9 / speed
            # Cap large idle gaps relative to previous sample.
            if prev_ts is not None and ts >= prev_ts:
                gap = (ts - prev_ts) / 1e9 / speed
                if gap > _MAX_GAP_S:
                    # Shift timeline so we don't wait forever on idle.
                    t0_wall -= (gap - _MAX_GAP_S)

            now = time.monotonic()
            delay = target - (now - t0_wall)
            is_img = rec.get("_rlog") == "img"

            # Behind schedule: skip intermediate images (keep last before a plot/log).
            if is_img and delay < -_IMG_CATCHUP_S:
                # Peek: if next is also image soon, drop this one.
                if i + 1 < n and records[i + 1].get("_rlog") == "img":
                    prev_ts = ts
                    continue
                # Last image before non-img (or EOF): still send, but don't wait.
                delay = 0

            if delay > 0.001:
                time.sleep(delay)

            msg = _to_sse(rec)
            if msg:
                emit(msg)
                # Flush images immediately so <img src=/img/N> can load without delay.
                if is_img or i % 16 == 0:
                    handler.wfile.flush()

            prev_ts = ts

        handler.wfile.flush()
        emit({"type": "status", "connected": True, "sender": sender})
        handler.wfile.flush()

        keep = {"type": "status", "connected": True, "sender": sender}
        while True:
            time.sleep(2)
            emit(keep)
            handler.wfile.flush()
    except Exception:
        pass


def run(rlog_path, http_port, speed=1.0):
    path = Path(rlog_path)
    if not path.is_file():
        print(f"[replay] file not found: {path}", file=sys.stderr)
        return 1

    raw = load(path)
    records, images = _prepare(raw)
    sender = sender_name(raw, fallback=path.stem)
    print(
        f"[replay] {path}  {len(records)} records ({len(images)} images)  "
        f"sender={sender}  speed={speed}x",
        file=sys.stderr,
    )

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            pass

        def do_GET(self):
            parsed = urlparse(self.path)
            route = parsed.path
            if route == "/":
                serve_page(self, "debugger.html")
            elif route == "/events":
                _write_replay_sse(self, records, sender, speed)
            elif route.startswith("/img/"):
                try:
                    idx = int(route.rsplit("/", 1)[-1])
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
                self.send_header("Cache-Control", "public, max-age=3600")
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
