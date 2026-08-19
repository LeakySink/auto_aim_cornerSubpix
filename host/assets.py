"""Local vendor JS for offline / cross-platform host UI."""

import os
import sys

DIR = os.path.dirname(os.path.abspath(__file__))

FILES = {
    "/chart.js": "chart.umd.min.js",
    "/hammer.js": "hammer.min.js",
    "/chartjs-plugin-zoom.js": "chartjs-plugin-zoom.min.js",
}

CDN = {
    "/chart.js":
        "https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js",
    "/hammer.js":
        "https://cdn.jsdelivr.net/npm/hammerjs@2.0.8/hammer.min.js",
    "/chartjs-plugin-zoom.js":
        "https://cdn.jsdelivr.net/npm/chartjs-plugin-zoom@2.2.0/dist/chartjs-plugin-zoom.min.js",
}

DOWNLOADS = [
    ("chart.umd.min.js", CDN["/chart.js"]),
    ("hammer.min.js", CDN["/hammer.js"]),
    ("chartjs-plugin-zoom.min.js", CDN["/chartjs-plugin-zoom.js"]),
]


def resolve(url_path):
    name = FILES.get(url_path)
    if not name:
        return None
    path = os.path.join(DIR, name)
    return path if os.path.isfile(path) else None


def send_js(handler, path):
    with open(path, "rb") as f:
        body = f.read()
    handler.send_response(200)
    handler.send_header("Content-Type", "application/javascript; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Cache-Control", "public, max-age=86400")
    handler.end_headers()
    handler.wfile.write(body)


def try_serve(handler, url_path):
    """Serve local JS, or 302 to CDN if the file is not vendored. Return True if handled."""
    if url_path not in FILES:
        return False
    path = resolve(url_path)
    if path:
        send_js(handler, path)
        return True
    cdn = CDN.get(url_path)
    if cdn:
        handler.send_response(302)
        handler.send_header("Location", cdn)
        handler.end_headers()
        return True
    return False


def download_all():
    import urllib.request
    for name, url in DOWNLOADS:
        dst = os.path.join(DIR, name)
        print(f"[assets] downloading {url}", file=sys.stderr)
        urllib.request.urlretrieve(url, dst)
        print(f"[assets] saved {dst} ({os.path.getsize(dst)} bytes)", file=sys.stderr)
