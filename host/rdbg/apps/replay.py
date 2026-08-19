"""Offline .rlog replay — same debugger UI, paced SSE (no UDP)."""

import json
import sys
import time
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse

from ..httputil import ThreadingHTTPServer, serve_page, try_serve_static
from ..rlog import load, sender_name, to_sse

# Max wait between successive image frames. Larger recorded gaps are compressed
# so short camera bursts stitch into continuous video (~30fps).
_IMG_FRAME_CAP_S = 1.0 / 25.0


def _record_ts(rec):
    return int(rec.get("ts") or 0)


def _prepare(records):
    """Index images for /img/N; strip jpeg bytes from record list."""
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


def _write_replay_sse(handler, records, images, sender, speed):
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
        # Image-driven clock: sleep only between frames; plots/logs flush with no wait
        # so camera bursts (~30fps) play as continuous video instead of a slideshow.
        last_img_ts = None
        last_img_wall = None
        n_plot = 0

        for i, rec in enumerate(records):
            is_img = rec.get("_rlog") == "img"
            ts = _record_ts(rec)

            if is_img:
                if last_img_ts is not None and last_img_wall is not None:
                    raw_s = (ts - last_img_ts) / 1e9
                    if raw_s < 0:
                        raw_s = 0
                    # Compress long gaps between camera bursts into one frame time.
                    wait = min(raw_s, _IMG_FRAME_CAP_S) / speed
                    delay = wait - (time.monotonic() - last_img_wall)
                    if delay > 0.0005:
                        time.sleep(delay)

                msg = _to_sse(rec)
                if msg:
                    emit(msg)
                    handler.wfile.flush()
                last_img_ts = ts
                last_img_wall = time.monotonic()
            else:
                msg = _to_sse(rec)
                if msg:
                    emit(msg)
                    n_plot += 1
                    if n_plot % 24 == 0:
                        handler.wfile.flush()

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
        f"sender={sender}  speed={speed}x  (image-paced video)",
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
                _write_replay_sse(self, records, images, sender, speed)
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
