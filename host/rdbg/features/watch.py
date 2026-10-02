"""Watch feature — one thread per page, one UDP port per robot.

Routes: fleet events/bind/state/select + img_subscribe + record/*（host/API.md §3）。
"""

from __future__ import annotations

from ..log.recorder import LiveRecorder, default_record_path
from ..sources.fleet import fleet
from .base import json_body, send_json
from .fleet_bound import FleetBoundFeature


class WatchFeature(FleetBoundFeature):
    id = "watch"
    title = "Watch"
    description = "实时接收车上 RemoteLogger：曲线、日志、图像"

    allow_rebind = False
    use_source_push_state = True

    def __init__(self):
        super().__init__()
        self._recorder = LiveRecorder()
        self._tx_profile = {}  # max_width / max_quality / max_fps / max_level

    def attach(self, shell):
        # select：查询当前绑车；img_subscribe：Watch 独有图像话题订阅
        self.attach_fleet_routes(shell, select=True, img_subscribe=True)
        p = getattr(self, "api_prefix", f"/api/{self.id}")
        shell.route(p + "/record/start", self._handle_record_start, methods=("POST",))
        shell.route(p + "/record/stop", self._handle_record_stop, methods=("POST",))
        shell.route(p + "/record/status", self._handle_record_status)

    def on_stop(self):
        self._detach_recorder()
        self._recorder.stop()
        super().on_stop()

    def status(self):
        st = super().status()
        st["record"] = self._recorder.status()
        st["tx_profile"] = dict(self._tx_profile)
        return st

    def _fan(self):
        sender = self._sender
        if not sender:
            return None
        with fleet._lock:
            slot = fleet._slots.get(sender)
            return slot["fan"] if slot else None

    def _attach_recorder(self):
        fan = self._fan()
        if fan is None:
            return
        if self._recorder not in fan.subs:
            fan.subs.append(self._recorder)

    def _detach_recorder(self):
        fan = self._fan()
        if fan is None:
            return
        try:
            fan.subs.remove(self._recorder)
        except ValueError:
            pass

    def _handle_record_start(self, handler):
        if not self._source or not self._sender:
            send_json(handler, {"ok": False, "error": "not bound"}, code=409)
            return
        body = json_body(handler)
        path = (body.get("path") or body.get("dir") or "").strip()
        if path:
            from pathlib import Path
            p = Path(path).expanduser()
            if p.is_dir() or path.endswith("/"):
                path = str(default_record_path(self._sender, str(p)))
        else:
            path = str(default_record_path(self._sender))
        try:
            out = self._recorder.start(path)
            self._attach_recorder()
        except Exception as e:
            send_json(handler, {"ok": False, "error": str(e)}, code=500)
            return
        send_json(handler, {"ok": True, "path": out, **self._recorder.status()})

    def _handle_record_stop(self, handler):
        self._detach_recorder()
        path = self._recorder.stop()
        send_json(handler, {"ok": True, "path": path, **self._recorder.status()})

    def _handle_record_status(self, handler):
        send_json(handler, self._recorder.status())

    def _handle_img_subscribe(self, handler):
        if not self._source:
            send_json(handler, {"ok": False, "error": "not running"}, code=409)
            return
        from ..util import parse_stream_list
        raw = (handler.query.get("streams") or [""])[0]
        streams = parse_stream_list(raw)

        def _int(name):
            vals = handler.query.get(name) or []
            if not vals or vals[0] == "":
                return None
            try:
                return int(vals[0])
            except ValueError:
                return None

        profile = {}
        for key in ("max_width", "max_quality", "max_fps", "max_level"):
            v = _int(key)
            if v is not None:
                profile[key] = v
        if profile:
            self._tx_profile = profile
            self._source.set_tx_profile(profile)
        self._source.set_img_streams(id(self.sse), streams)
        send_json(handler, {
            "ok": True,
            "streams": streams,
            "sender": self._sender,
            "tx_profile": dict(self._tx_profile),
        })
