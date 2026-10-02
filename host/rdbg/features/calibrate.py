"""Calibrate feature — original calibrate.html UI on the hub.

Fleet routes + /calib + /done（host/API.md §3 Calibrate）。
"""

from __future__ import annotations

import threading

from ..calib_cmds import ALLOWED_CMDS, calib_payload, quit_robot_burst
from ..sources.fleet import fleet
from .base import json_body, send_json
from .fleet_bound import FleetBoundFeature


class CalibrateFeature(FleetBoundFeature):
    id = "calibrate"
    title = "Calibrate"
    description = "棋盘格相机内参标定：覆盖度、采样、一键标定"

    allow_rebind = True
    default_img_streams = ["calibrate"]
    use_source_push_state = False

    def __init__(self):
        super().__init__()
        self._done_lock = threading.Lock()
        self._done_sent = False

    @property
    def ui_path(self):
        # 标定仍用静态页；?i= 把实例 id 传给前端拼 /api/i/<id>
        iid = getattr(self, "instance_id", "")
        return f"/calibrate.html?i={iid}" if iid else "/calibrate.html"

    def attach(self, shell):
        p = getattr(self, "api_prefix", "/api/calibrate")
        self.attach_fleet_routes(shell, select=True, img_subscribe=False)
        shell.route(p + "/calib", self._handle_calib, methods=("GET", "POST"))
        shell.route(p + "/done", self._handle_done, methods=("GET", "POST"))

    def list_senders(self):
        return [row["name"] for row in fleet.snapshot()]

    def after_bind(self):
        self.push_page_state()

    def on_tick(self):
        rows = fleet.snapshot()
        names = [row["name"] for row in rows]
        if not self._sender and rows:
            prefer = next(
                (row["name"] for row in rows
                 if row.get("feature") == "calibrate" or row.get("app") == "calibrate"),
                names[0],
            )
            try:
                self._bind(prefer)
            except Exception:
                pass
        self.push_page_state(names)

    def _send_cmd(self, cmd, body=None):
        name, info = self._robot_info()
        if not name or not info or not self._source:
            return False
        self._source.client.send_json(
            info["ip"], info["control"], calib_payload(cmd, body))
        return True

    def _handle_calib(self, handler):
        from ..calib_cmds import parse_calib_request
        cmd, body = parse_calib_request(handler, json_body)
        if cmd not in ALLOWED_CMDS:
            send_json(handler, {"ok": False, "error": "bad cmd"}, code=400)
            return
        if cmd == "set_exposure":
            # also allow ?exposure_us= / ?exposure_ms=（数值均为微秒）
            if "exposure_us" not in body and "exposure_ms" not in body:
                q = (handler.query.get("exposure_us") or handler.query.get("exposure_ms") or [""])[0]
                if q:
                    body = dict(body)
                    body["exposure_us"] = q
        if not self._send_cmd(cmd, body):
            send_json(handler, {"ok": False, "error": "no robot"}, code=404)
            return
        send_json(handler, {"ok": True, "cmd": cmd, "robot": self._sender})

    def _handle_done(self, handler):
        """Browser saw calib_done: tell the car to quit. Hub stays up."""
        with self._done_lock:
            already = self._done_sent
            self._done_sent = True
        quit_robot = False
        if not already:
            name, info = self._robot_info()
            if info and self._source:
                quit_robot_burst(self._source.client, info["ip"], info["control"])
                quit_robot = True
        send_json(handler, {
            "ok": True,
            "quit_robot": quit_robot,
            "already": already,
        })
