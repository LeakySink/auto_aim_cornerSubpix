"""Calibrate app — live UDP + web UI for board coverage / buttons."""

import json
import sys
import threading
import time

from ..http.httputil import open_browser
from ..http.shell import Shell
from ..sources.live import LiveSource


class CalibrateSource(LiveSource):
    """Same discovery/register as watch, but calibrate page + cmd route."""

    id = "calibrate"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._httpd = None
        self._done_lock = threading.Lock()
        self._done_sent = False

    def attach(self, shell):
        self.shell = shell
        self.udp.on_output = lambda line: shell.sse.put(line)
        shell.page("/", "calibrate.html")
        shell.sse_route("/events", on_connect=self.push_state)
        shell.route("/select", self._handle_select)
        shell.route("/api/calib", self._handle_calib, methods=("GET", "POST"))
        shell.route("/api/done", self._handle_done, methods=("GET", "POST"))

    def _robot_info(self):
        senders = self.live_senders()
        name = self._selected if self._selected in senders else (
            senders[0] if senders else "")
        if not name:
            return "", {}
        with self._lock:
            info = dict(self.robots.get(name) or {})
        return name, info

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
        allowed = {"add", "calibrate", "drop", "reset", "quit", "done"}
        if cmd not in allowed:
            handler.send(400, b'{"ok":false,"error":"bad cmd"}', "application/json")
            return

        name, info = self._robot_info()
        if not name or not info:
            handler.send(404, b'{"ok":false,"error":"no robot"}', "application/json")
            return
        payload = {"cmd": cmd}
        if cmd == "calibrate":
            payload["host_time"] = time.strftime("%Y-%m-%d %H:%M:%S")
        self.client.send_json(info["ip"], info["control"], payload)
        handler.send(
            200,
            json.dumps({"ok": True, "cmd": cmd, "robot": name}).encode(),
            "application/json",
        )

    def _handle_done(self, handler):
        """Browser saw calib_done: tell car to quit, then stop host HTTP."""
        with self._done_lock:
            already = self._done_sent
            self._done_sent = True
        name, info = self._robot_info()
        if info:
            self.client.send_json(info["ip"], info["control"], {"cmd": "quit"})
            # 多发几次，避免 UDP 丢包
            for _ in range(3):
                time.sleep(0.03)
                self.client.send_json(info["ip"], info["control"], {"cmd": "quit"})
        handler.send(
            200,
            json.dumps({"ok": True, "quit_robot": bool(info), "already": already}).encode(),
            "application/json",
        )
        if self._httpd is not None and not already:
            def _stop():
                time.sleep(0.4)
                try:
                    self._httpd.shutdown()
                except Exception:
                    pass
            threading.Thread(target=_stop, daemon=True).start()


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
    source._httpd = httpd

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
