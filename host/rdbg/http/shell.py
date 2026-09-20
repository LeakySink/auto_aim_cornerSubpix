"""HTTP shell: static files, SSE, and a route table for source plugins."""

import sys
import threading
from http.server import BaseHTTPRequestHandler
from urllib.parse import parse_qs, urlparse

from .httputil import ThreadingHTTPServer, serve_page, try_serve_static
from .sse import SSEQueue, write_sse


class Shell:
    def __init__(self, name="rdbg"):
        self.name = name
        self.sse = SSEQueue()
        self.last_bind_error = None
        self._pages = {}
        self._exact = {}
        self._prefixes = []

    def page(self, path, html_name):
        self._pages[path] = html_name

    def route(self, path, fn, methods=("GET",), prefix=False):
        for method in methods:
            method = method.upper()
            if prefix:
                self._prefixes.append((method, path, fn))
            else:
                self._exact[(method, path)] = fn
        self._prefixes.sort(key=lambda item: len(item[1]), reverse=True)

    def sse_route(self, path="/events", on_connect=None):
        def _events(handler):
            write_sse(handler, self.sse, on_connect=on_connect)

        self.route(path, _events)

    def bind(self, host, port, tries=1):
        handler = self._make_handler()
        self.last_bind_error = None
        n = max(1, int(tries))
        for p in range(port, port + n):
            try:
                return ThreadingHTTPServer((host, p), handler), p
            except OSError as e:
                self.last_bind_error = e
        return None, 0

    def serve(self, httpd, *, threaded=False, on_stop=None):
        try:
            if threaded:
                t = threading.Thread(target=httpd.serve_forever, daemon=True)
                t.start()
                while t.is_alive():
                    t.join(timeout=0.5)
            else:
                httpd.serve_forever()
        except KeyboardInterrupt:
            print(f"\n[{self.name}] shutting down...", file=sys.stderr)
        finally:
            if threaded:
                httpd.shutdown()
            httpd.server_close()
            if on_stop:
                on_stop()
        return 0

    def _make_handler(self):
        shell = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def send(self, code, body, ctype="application/octet-stream", cache="no-cache"):
                if isinstance(body, str):
                    body = body.encode("utf-8")
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", cache)
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                parsed = urlparse(self.path)
                path = parsed.path
                self.route_path = path
                self.query = parse_qs(parsed.query)
                self.body = b""

                html = shell._pages.get(path)
                if html:
                    serve_page(self, html)
                    return

                fn = shell._exact.get(("GET", path))
                if fn:
                    fn(self)
                    return

                for method, prefix, fn in shell._prefixes:
                    if method == "GET" and path.startswith(prefix):
                        fn(self)
                        return

                if try_serve_static(self, self.path):
                    return
                self.send_error(404)

            def do_POST(self):
                parsed = urlparse(self.path)
                path = parsed.path
                self.route_path = path
                self.query = parse_qs(parsed.query)
                length = int(self.headers.get("Content-Length") or 0)
                self.body = self.rfile.read(length) if length > 0 else b""

                fn = shell._exact.get(("POST", path))
                if fn:
                    fn(self)
                    return

                for method, prefix, fn in shell._prefixes:
                    if method == "POST" and path.startswith(prefix):
                        fn(self)
                        return
                self.send_error(404)

            def do_OPTIONS(self):
                self.send_response(204)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
                self.send_header("Access-Control-Allow-Headers", "Content-Type")
                self.end_headers()

        return Handler
