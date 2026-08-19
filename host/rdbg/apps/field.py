"""Multi-robot field-control HTTP/SSE app."""

import sys
from http.server import BaseHTTPRequestHandler
from urllib.parse import urlparse

from ..control import ControlServer
from ..httputil import ThreadingHTTPServer, serve_page, try_serve_static
from ..sse import SSEQueue, write_sse
from ..udp import UdpBackend


class FieldApp:
    def __init__(self, data_port, ctrl_port):
        self.sse = SSEQueue()
        self.control = ControlServer(
            port=ctrl_port,
            data_port_start=data_port,
            data_port_end=data_port,
            reuse_ports=True,
        )
        self.udp = UdpBackend()
        self.udp.on_output = lambda line: self.sse.put(line)

    def handler(self):
        app = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def do_GET(self):
                path = urlparse(self.path).path
                if path == "/":
                    serve_page(self, "field.html")
                elif path == "/events":
                    write_sse(self, app.sse)
                elif try_serve_static(self, self.path):
                    return
                else:
                    self.send_error(404)

        return Handler


def run(http_port, data_port, ctrl_port):
    app = FieldApp(data_port, ctrl_port)
    app.control.start()
    app.udp.start(data_port, "field")

    try:
        httpd = ThreadingHTTPServer(("0.0.0.0", http_port), app.handler())
    except OSError as e:
        print(f"[field] bind error {e}", file=sys.stderr)
        app.udp.stop()
        app.control.stop()
        return 1

    print(f"[field] http://localhost:{http_port}  data:{data_port}  ctrl:{ctrl_port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[field] shutdown")
    finally:
        httpd.server_close()
        app.udp.stop()
        app.control.stop()
    return 0
