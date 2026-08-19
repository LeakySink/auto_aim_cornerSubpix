"""Static file serving, vendor CDN fallback, HTTP server mixin."""

import http.server
import shutil
import socketserver
import subprocess
import sys
import webbrowser
from pathlib import Path
from urllib.parse import unquote, urlparse

PKG_DIR = Path(__file__).resolve().parent
STATIC_DIR = PKG_DIR / "static"
VENDOR_DIR = STATIC_DIR / "vendor"

MIME = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".mjs": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".map": "application/json",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".svg": "image/svg+xml",
    ".ico": "image/x-icon",
}

CDN = {
    "chart.umd.min.js":
        "https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js",
    "hammer.min.js":
        "https://cdn.jsdelivr.net/npm/hammerjs@2.0.8/hammer.min.js",
    "chartjs-plugin-zoom.min.js":
        "https://cdn.jsdelivr.net/npm/chartjs-plugin-zoom@2.2.0/dist/chartjs-plugin-zoom.min.js",
}

VENDOR_ALIASES = {
    "/chart.js": "chart.umd.min.js",
    "/hammer.js": "hammer.min.js",
    "/chartjs-plugin-zoom.js": "chartjs-plugin-zoom.min.js",
}


class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def _safe_file(rel):
    rel = rel.lstrip("/").replace("\\", "/")
    if ".." in Path(rel).parts:
        return None
    path = (STATIC_DIR / rel).resolve()
    try:
        path.relative_to(STATIC_DIR.resolve())
    except ValueError:
        return None
    return path if path.is_file() else None


def send_file(handler, path, cache="public, max-age=3600"):
    data = path.read_bytes()
    ctype = MIME.get(path.suffix.lower(), "application/octet-stream")
    handler.send_response(200)
    handler.send_header("Content-Type", ctype)
    handler.send_header("Content-Length", str(len(data)))
    handler.send_header("Cache-Control", cache)
    handler.end_headers()
    handler.wfile.write(data)


def serve_page(handler, name):
    path = STATIC_DIR / name
    if not path.is_file():
        handler.send_error(404, f"{name} not found")
        return
    send_file(handler, path, cache="no-cache")


def _serve_vendor(handler, filename):
    fp = VENDOR_DIR / filename
    if fp.is_file():
        send_file(handler, fp)
        return True
    cdn = CDN.get(filename)
    if cdn:
        handler.send_response(302)
        handler.send_header("Location", cdn)
        handler.end_headers()
        return True
    return False


def try_serve_static(handler, raw_path):
    path = unquote(urlparse(raw_path).path)
    alias = VENDOR_ALIASES.get(path)
    if alias:
        return _serve_vendor(handler, alias)
    if path.startswith("/static/vendor/"):
        return _serve_vendor(handler, path.rsplit("/", 1)[-1])
    if path.startswith("/static/"):
        fp = _safe_file(path[len("/static/"):])
        if fp:
            cache = "no-cache" if "/vendor/" not in path else "public, max-age=3600"
            send_file(handler, fp, cache=cache)
            return True
        handler.send_error(404)
        return True
    return False


def download_vendor():
    import urllib.request
    VENDOR_DIR.mkdir(parents=True, exist_ok=True)
    for name, url in CDN.items():
        dst = VENDOR_DIR / name
        print(f"[assets] downloading {url}", file=sys.stderr)
        urllib.request.urlretrieve(url, dst)
        print(f"[assets] saved {dst} ({dst.stat().st_size} bytes)", file=sys.stderr)


def open_browser(url):
    # Chrome/GTK 会往终端刷一堆无关警告，必须与 watch 的 stdout 隔离。
    for browser in ("google-chrome", "chromium-browser", "chromium", "microsoft-edge"):
        exe = shutil.which(browser)
        if exe:
            subprocess.Popen(
                [exe, f"--app={url}"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            return True
    webbrowser.open(url)
    return True
