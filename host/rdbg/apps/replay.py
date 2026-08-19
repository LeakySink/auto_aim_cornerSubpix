"""Local .rlog player — preload to memory, independent of live watch."""

import base64
import json
import shutil
import subprocess
import sys
import time
import webbrowser
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse

from ..httputil import ThreadingHTTPServer, serve_page, try_serve_static
from ..session import load_session_or_exit

PORT_TRY = 20


def public_meta(session):
    frames = [
        {"i": i, "t": f["t"], "meta": f.get("meta") or {}}
        for i, f in enumerate(session.get("frames") or [])
    ]
    return {
        "file": session.get("file", ""),
        "path": session.get("path", ""),
        "sender": session.get("sender", ""),
        "duration": session.get("duration", 0),
        "memory_bytes": session.get("memory_bytes", 0),
        "fields": session.get("fields") or [],
        "series": session.get("series") or {},
        "frames": frames,
    }


def _open_window(url):
    for browser in ("google-chrome", "chromium-browser", "chromium", "microsoft-edge"):
        exe = shutil.which(browser)
        if exe:
            subprocess.Popen([exe, f"--app={url}"])
            return True
    webbrowser.open(url)
    return True


class ReplayApp:
    def __init__(self, session):
        self.session = session

    def handler(self):
        app = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
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
                    serve_page(self, "replay.html")
                    return
                if path in ("/api/meta", "/api/session"):
                    body = json.dumps(public_meta(app.session), separators=(",", ":"))
                    self._send(200, body, "application/json; charset=utf-8")
                    return
                if path.startswith("/api/frame/"):
                    try:
                        idx = int(path.rsplit("/", 1)[-1])
                    except ValueError:
                        self.send_error(400)
                        return
                    frames = app.session.get("frames") or []
                    if idx < 0 or idx >= len(frames):
                        self.send_error(404)
                        return
                    fr = frames[idx]
                    jpeg = fr.get("jpeg")
                    if not jpeg:
                        b64 = fr.get("b64")
                        if b64:
                            jpeg = base64.b64decode(b64)
                    if not jpeg:
                        self.send_error(404)
                        return
                    self._send(200, jpeg, "image/jpeg", cache="public, max-age=86400")
                    return
                if try_serve_static(self, self.path):
                    return
                self.send_error(404)

        return Handler


def _bind(host, port, handler):
    for p in range(port, port + PORT_TRY):
        try:
            httpd = ThreadingHTTPServer((host, p), handler)
            if p != port:
                print(f"[replay] port {port} busy, using {p}", file=sys.stderr)
            return httpd, p
        except OSError:
            continue
    print(
        f"[replay] ports {port}-{port + PORT_TRY - 1} all busy",
        file=sys.stderr,
    )
    return None, 0


def run(rlog, host="127.0.0.1", port=8765, max_mb=1024, no_browser=False):
    max_bytes = max(64, int(max_mb)) << 20
    path = Path(rlog).expanduser()
    if not path.is_file():
        print(f"[replay] file not found: {rlog}", file=sys.stderr)
        return 1

    session = load_session_or_exit(str(path), max_bytes=max_bytes)
    app = ReplayApp(session)
    httpd, bound = _bind(host, port, app.handler())
    if httpd is None:
        return 1

    url = f"http://{host}:{bound}/"
    print(f"[replay] {url}", file=sys.stderr)
    if not no_browser:
        time.sleep(0.15)
        _open_window(url)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[replay] shutting down...", file=sys.stderr)
    finally:
        httpd.server_close()
    return 0
