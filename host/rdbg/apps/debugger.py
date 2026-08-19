"""Single-robot Remote Debugger HTTP/SSE app."""

import json
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler

from ..control import ControlServer
from ..httputil import ThreadingHTTPServer, open_browser, serve_page, try_serve_static
from ..sse import SSEQueue, write_sse
from ..udp import UdpBackend


class DebuggerApp:
    def __init__(self, control_port):
        self.sse = SSEQueue()
        self.control = ControlServer(port=control_port)
        self.udp = UdpBackend()
        self.udp.on_output = lambda line: self.sse.put(line)
        self.udp.on_state = self.push_state
        self._last_version = -1
        self._last_senders = []

    def push_state(self):
        state = {
            "type": "state",
            "active_sender": self.udp.active_sender,
            "senders": self.control.get_senders(),
        }
        self.sse.put(json.dumps(state))

    def select(self, name):
        if not name:
            return
        info = self.control.get_sender_info(name)
        if info:
            self.udp.switch(name, info["data_port"])
        self.push_state()

    def poller(self):
        while self.control._running:
            time.sleep(0.5)
            try:
                version = self.control.version()
                senders = self.control.get_senders()
                if version != self._last_version or senders != self._last_senders:
                    self._last_version = version
                    self._last_senders = senders
                    self.push_state()
                if senders:
                    info = self.control.get_sender_info(senders[0])
                    if info and (not self.udp.running or
                                 self.udp.active_sender != senders[0] or
                                 self.udp.active_port != info["data_port"]):
                        self.udp.switch(senders[0], info["data_port"])
                elif self.udp.running:
                    self.udp.stop()
            except Exception:
                pass

    def handler(self):
        app = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def do_GET(self):
                path = urllib.parse.urlparse(self.path).path
                if path == "/":
                    serve_page(self, "debugger.html")
                elif path == "/events":
                    write_sse(self, app.sse, on_connect=app.push_state)
                elif path == "/select":
                    qs = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
                    app.select(qs.get("sender", [""])[0])
                    body = b'{"ok":true}'
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                elif try_serve_static(self, self.path):
                    return
                else:
                    self.send_error(404)

        return Handler


def run(http_port, control_port, no_browser=False):
    app = DebuggerApp(control_port)
    app.control.start()
    threading.Thread(target=app.poller, daemon=True).start()
    app.push_state()

    try:
        httpd = ThreadingHTTPServer(("0.0.0.0", http_port), app.handler())
    except OSError as e:
        print(f"[debugger] cannot bind port {http_port}: {e}", file=sys.stderr)
        app.control.stop()
        return 1

    url = f"http://localhost:{http_port}"
    print(f"[debugger] {url}", file=sys.stderr)
    print(f"[debugger] control port {control_port}", file=sys.stderr)
    if not no_browser:
        time.sleep(0.15)
        open_browser(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[debugger] shutting down...", file=sys.stderr)
    finally:
        httpd.server_close()
        app.udp.stop()
        app.control.stop()
    return 0
