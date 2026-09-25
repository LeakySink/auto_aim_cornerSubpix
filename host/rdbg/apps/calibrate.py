"""Calibrate app — legacy standalone process (prefer hub portal Calibrate)."""

import sys
import threading
import time

from ..calib_cmds import ALLOWED_CMDS, calib_payload, quit_robot_burst
from ..features.base import json_body, send_json
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
        from ..calib_cmds import parse_calib_request
        cmd, body = parse_calib_request(handler, json_body)
        if cmd not in ALLOWED_CMDS:
            send_json(handler, {"ok": False, "error": "bad cmd"}, code=400)
            return
        name, info = self._robot_info()
        if not name or not info:
            send_json(handler, {"ok": False, "error": "no robot"}, code=404)
            return
        if cmd == "set_exposure" and "exposure_ms" not in body:
            q = (handler.query.get("exposure_ms") or [""])[0]
            if q:
                body = dict(body)
                body["exposure_ms"] = q
        self.client.send_json(info["ip"], info["control"], calib_payload(cmd, body))
        send_json(handler, {"ok": True, "cmd": cmd, "robot": name})

    def _handle_done(self, handler):
        """Browser saw calib_done: tell car to quit, then stop host HTTP."""
        with self._done_lock:
            already = self._done_sent
            self._done_sent = True
        name, info = self._robot_info()
        if info and not already:
            quit_robot_burst(self.client, info["ip"], info["control"], times=4)
        send_json(handler, {
            "ok": True,
            "quit_robot": bool(info),
            "already": already,
        })
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
