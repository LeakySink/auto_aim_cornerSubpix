"""Calibrate app — live UDP + web UI for board coverage / buttons."""

import json
import sys
import time

from ..http.httputil import open_browser
from ..http.shell import Shell
from ..sources.live import LiveSource


class CalibrateSource(LiveSource):
    """Same discovery/register as watch, but calibrate page + cmd route."""

    id = "calibrate"

    def attach(self, shell):
        self.shell = shell
        self.udp.on_output = lambda line: shell.sse.put(line)
        shell.page("/", "calibrate.html")
        shell.sse_route("/events", on_connect=self.push_state)
        shell.route("/select", self._handle_select)
        shell.route("/api/calib", self._handle_calib, methods=("GET", "POST"))

    def _handle_calib(self, handler):
        cmd = ""
        if handler.command == "POST" and handler.body:
            try:
                body = json.loads(handler.body.decode())
                cmd = (body.get("cmd") or "").strip()
            except Exception:
                cmd = ""
        if not cmd:
            cmd = (handler.query.get("cmd") or [""])[0].strip()
        allowed = {"add", "calibrate", "save", "drop", "reset", "undistort", "quit"}
        if cmd not in allowed:
            handler.send(400, b'{"ok":false,"error":"bad cmd"}', "application/json")
            return

        senders = self.live_senders()
        name = self._selected if self._selected in senders else (
            senders[0] if senders else "")
        if not name:
            handler.send(404, b'{"ok":false,"error":"no robot"}', "application/json")
            return
        with self._lock:
            info = dict(self.robots.get(name) or {})
        if not info:
            handler.send(404, b'{"ok":false,"error":"unknown robot"}', "application/json")
            return
        self.client.send_json(info["ip"], info["control"], {"cmd": cmd})
        handler.send(
            200,
            json.dumps({"ok": True, "cmd": cmd, "robot": name}).encode(),
            "application/json",
        )


def run(http_port, data_port=15001, peer_port=15100, discover_port=15999,
        no_browser=False):
    source = CalibrateSource(data_port, peer_port, discover_port)
    shell = Shell(name="calibrate")
    source.attach(shell)
    source.start()

    httpd, bound = shell.bind("0.0.0.0", http_port, tries=1)
    if httpd is None:
        print(
            f"[calibrate] cannot bind port {http_port}: {shell.last_bind_error}",
            file=sys.stderr,
        )
        source.stop()
        return 1

    url = f"http://localhost:{bound}"
    print(f"[calibrate] {url}", file=sys.stderr)
    print(f"[calibrate] data {data_port}  peer {peer_port}  discover {discover_port}",
          file=sys.stderr)
    print("[calibrate] on robot: ./build/calibrate",
          file=sys.stderr)
    if not no_browser:
        time.sleep(0.15)
        open_browser(url)
    return shell.serve(httpd, threaded=False, on_stop=source.stop)
