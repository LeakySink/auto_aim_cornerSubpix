"""Local HTTP server for replayer UI."""

import json
import mimetypes
import socketserver
import sys
import threading
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parent
STATIC = ROOT / "static"
VENDOR = STATIC / "vendor"

CDN = {
    "chart.umd.min.js":
        "https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js",
    "hammer.min.js":
        "https://cdn.jsdelivr.net/npm/hammerjs@2.0.8/hammer.min.js",
    "chartjs-plugin-zoom.min.js":
        "https://cdn.jsdelivr.net/npm/chartjs-plugin-zoom@2.2.0/dist/chartjs-plugin-zoom.min.js",
}


class ThreadingHTTPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def _safe(root, rel):
    rel = rel.lstrip("/").replace("\\", "/")
    if ".." in Path(rel).parts:
        return None
    path = (root / rel).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError:
        return None
    return path if path.is_file() else None


class Handler(BaseHTTPRequestHandler):
    session = None

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="application/octet-stream", cache="no-cache"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            fp = STATIC / "index.html"
            if fp.is_file():
                self._send(200, fp.read_bytes(), "text/html; charset=utf-8")
            else:
                self.send_error(404)
            return

        if path == "/api/session":
            if self.session is None:
                self.send_error(503, "session not loaded")
                return
            body = json.dumps(self.session, separators=(",", ":"))
            self._send(200, body, "application/json; charset=utf-8")
            return

        if path.startswith("/static/"):
            rel = path[len("/static/"):]
            fp = _safe(STATIC, rel)
            if fp:
                ctype = mimetypes.guess_type(str(fp))[0] or "application/octet-stream"
                self._send(200, fp.read_bytes(), ctype, cache="public, max-age=3600")
                return

        if path.startswith("/vendor/"):
            name = path.split("/")[-1]
            fp = VENDOR / name
            if fp.is_file():
                self._send(200, fp.read_bytes(), "application/javascript",
                            cache="public, max-age=86400")
                return
            url = CDN.get(name)
            if url:
                self.send_response(302)
                self.send_header("Location", url)
                self.end_headers()
                return

        self.send_error(404)


def run(session, host="127.0.0.1", port=8765):
    Handler.session = session
    try:
        httpd = ThreadingHTTPServer((host, port), Handler)
    except OSError as e:
        print(f"[replayer] bind {host}:{port} failed: {e}", file=sys.stderr)
        return None, 0

    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    return httpd, port
