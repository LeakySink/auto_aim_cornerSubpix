"""Replay feature — load .rlog session on start(config.path)."""

from __future__ import annotations

import base64
import json
import threading

from ..http.sse import SSEQueue, write_sse
from ..sources.replay import public_meta
from .base import Feature, json_body, send_json


class ReplayFeature(Feature):
    id = "replay"
    title = "Replay"
    description = "本地回放 .rlog：时间轴、曲线、图像、日志"

    def __init__(self):
        super().__init__()
        self.sse = SSEQueue()
        self._session = None

    def attach(self, shell):
        p = getattr(self, "api_prefix", "/api/replay")
        shell.route(p + "/events", self._handle_events)
        shell.route(p + "/meta", self._handle_meta)
        shell.route(p + "/session", self._handle_meta)
        shell.route(p + "/frame/", self._handle_frame, prefix=True)
        shell.route(p + "/load", self._handle_load, methods=("POST",))

    def on_start(self, config):
        path = config.get("path") or config.get("rlog") or ""
        max_mb = int(config.get("max_mb") or 1024)
        if path:
            self._load(path, max_mb)

    def on_stop(self):
        self._session = None

    def run(self, stop_event: threading.Event):
        while not stop_event.wait(0.5):
            pass

    def _load(self, path, max_mb=1024):
        max_bytes = max(64, int(max_mb)) << 20
        from ..log.session import load_session
        session = load_session(str(path), max_bytes=max_bytes)
        self._session = session
        name = session.get("file") or session.get("sender") or "replay"
        self.sse.put(json.dumps({
            "type": "state",
            "active_sender": name,
            "senders": [name],
        }))
        self.sse.put(json.dumps({
            "type": "status",
            "connected": True,
            "sender": name,
        }))

    def _handle_load(self, handler):
        body = json_body(handler)
        path = body.get("path") or body.get("rlog") or ""
        max_mb = int(body.get("max_mb") or 1024)
        if not path:
            send_json(handler, {"ok": False, "error": "path required"}, code=400)
            return
        try:
            self._load(path, max_mb)
            with self._lock:
                self._config["path"] = path
            send_json(handler, {"ok": True, "meta": public_meta(self._session)})
        except Exception as e:
            send_json(handler, {"ok": False, "error": str(e)}, code=400)

    def _handle_events(self, handler):
        write_sse(handler, self.sse)

    def _handle_meta(self, handler):
        if not self._session:
            send_json(handler, {"error": "no session"}, code=409)
            return
        body = json.dumps(public_meta(self._session), separators=(",", ":"))
        handler.send(200, body, "application/json; charset=utf-8")

    def _handle_frame(self, handler):
        if not self._session:
            handler.send_error(409)
            return
        try:
            idx = int(handler.route_path.rsplit("/", 1)[-1])
        except ValueError:
            handler.send_error(400)
            return
        frames = self._session.get("frames") or []
        if idx < 0 or idx >= len(frames):
            handler.send_error(404)
            return
        fr = frames[idx]
        jpeg = fr.get("jpeg")
        if not jpeg:
            b64 = fr.get("b64")
            if b64:
                jpeg = base64.b64decode(b64)
        if not jpeg:
            handler.send_error(404)
            return
        handler.send(200, jpeg, "image/jpeg", cache="public, max-age=86400")
